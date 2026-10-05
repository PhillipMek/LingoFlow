#include "Network/OpenAIRealtimeBackend.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>

#include <nlohmann/json.hpp>

#include "Network/Base64.h"
#include "Security/ISecretStore.h"
#include "Translation/LanguageRegistry.h"
#include "Utils/Log.h"

namespace liveai {
namespace network {

using json = nlohmann::json;

namespace {

constexpr int kWireSampleRate = 24000;  ///< protocol section 7, both directions
constexpr int kWireChannels = 1;        ///< protocol section 7
constexpr int kWirePort = 443;
const char* kLogComponent = "network.openai";

/// Stream clock for the subtitle settle policy (task 013): wall-clock
/// milliseconds off a monotonic source, never a calendar.
long long steadyNowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

/// Sanity bound for a server expiry announcement (protocol docs section 4bis):
/// the documented ceiling is 60 minutes, and anything beyond this window is
/// either a broken epoch, a milliseconds-vs-seconds confusion, or a local clock
/// skewed so badly that the announcement cannot be trusted. An announcement
/// outside the window is refused - the machine keeps its local age policy -
/// never silently obeyed.
constexpr long long kExpirySanityMaxMs = 24LL * 60LL * 60LL * 1000LL;

/// The calendar clock enters at exactly one point: deriving the single delta
/// `expires_at - now` at arrival. Ongoing measurement then rides the steady
/// clock, so a later time adjustment (NTP, the user, a DST rewrite) can neither
/// move up nor push out the deadline the show relies on.
long long wallNowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

/// Protocol section 9 connection-level table: HTTP status before the upgrade
/// maps to product categories; nothing provider-named crosses the sink seam.
translation::TranslationErrorCategory categoryForHttpStatus(int status)
{
    switch (status)
    {
        case 401:
        case 403:
            return translation::TranslationErrorCategory::rejectedRequest;
        default:
            return translation::TranslationErrorCategory::connection;
    }
}

/// Protocol section 9: a refused upgrade may carry an error code in its JSON
/// body that changes the recovery answer - billing/account refusals are
/// operator-actionable ("retrying won't restore API access"). Only the code is
/// extracted here for classification; the provider's vocabulary never crosses
/// the sink seam, and unparseable bodies keep the plain status-table mapping.
std::string refusalErrorCode(const std::string& body)
{
    if (body.empty())
        return {};
    const auto parsed = nlohmann::json::parse(body, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object())
        return {};
    const auto& error = parsed["error"];
    if (!error.is_object() || !error.contains("code") || !error["code"].is_string())
        return {};
    return error["code"].get<std::string>();
}

bool containsAudioWord(const std::string& text)
{
    std::string lower = text;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return lower.find("audio") != std::string::npos;
}

std::int16_t floatToInt16(float v)
{
    const long scaled = std::lround(v * 32768.0f);
    if (scaled >= 32767L)
        return 32767;
    if (scaled <= -32768L)
        return -32768;
    return static_cast<std::int16_t>(scaled);
}

/// Type-guarded field access. nlohmann's typed getters throw on a type
/// mismatch, and protocol parsing must never ride an exception path: a field
/// of the wrong type is treated like the field being absent (AGENTS.md 12).
std::string stringField(const json& j, const char* key)
{
    if (j.contains(key) && j[key].is_string())
        return j[key].get<std::string>();
    return {};
}

bool intField(const json& j, const char* key, int& out)
{
    if (j.contains(key) && j[key].is_number_integer())
    {
        out = j[key].get<int>();
        return true;
    }
    return false;
}

} // namespace

using translation::SessionState;
using translation::TranslationError;
using translation::TranslationErrorCategory;

OpenAIRealtimeBackend::OpenAIRealtimeBackend(security::ISecretStore& secrets,
                                             OpenAIRealtimeOptions options,
                                             TransportFactory factory)
    : options_ (std::move(options)), secrets_ (secrets), factory_ (std::move(factory))
{
    if (!factory_)
        factory_ = &makeWinHttpTransport;
}

OpenAIRealtimeBackend::~OpenAIRealtimeBackend()
{
    closeSession();
}

std::string_view OpenAIRealtimeBackend::name() const noexcept
{
    return "OpenAI Realtime Translation";
}

SessionState OpenAIRealtimeBackend::state() const noexcept
{
    return static_cast<SessionState>(state_.load(std::memory_order_relaxed));
}

void OpenAIRealtimeBackend::setSink(translation::ITranslationSink& sink) noexcept
{
    sink_ = &sink;
}

// ------------------------------------------------------------------ openSession

bool OpenAIRealtimeBackend::openSession(const translation::SessionRequest& request,
                                        std::string& error)
{
    error.clear();

    // Contract rule 1: no sink, no session.
    if (sink_ == nullptr)
    {
        error = "openai: setSink() must be called before openSession()";
        return false;
    }

    // Contract rule 2: one session at a time; a refusal that changes nothing
    // reports nothing (rule 3).
    if (state() != SessionState::closed)
    {
        error = std::string("openai: a session is already ")
              + std::string(translation::nameOf(state()))
              + "; call closeSession() first";
        return false;
    }

    // Previous session threads, if any finished on their own, are reaped here;
    // the transport object outlives a session deliberately (post-close
    // inspection), the next open replaces it.
    if (receiverThread_.joinable())
        receiverThread_.join();
    if (senderThread_.joinable())
        senderThread_.join();
    receiverThread_ = std::thread {};
    senderThread_ = std::thread {};
    transport_.reset();

    // Protocol section 11: the model identifier is validated, never guessed.
    const std::string model = request.model.empty() ? options_.defaultModel : request.model;
    if (model != options_.defaultModel)
    {
        error = "openai: unsupported model '" + request.model
              + "'; documented supported model: '" + options_.defaultModel + "'";
        return false;
    }

    if (request.pair.output.empty())
    {
        error = "openai: a target language is required (pair.output is empty)";
        return false;
    }

    // Capabilities (task 011): a pair outside the versioned manifest is refused
    // offline, before a single byte goes to the network - the provider would
    // reject the target anyway (and the source is auto-detected, so a wrong
    // input declaration is a product error, not a retryable one). The manifest
    // is the product's capability source (docs section 15: no dynamic discovery
    // scope for this key); options_.capabilities is the seam a future dynamic
    // manifest replaces it through, unchanged logic.
    const translation::LanguageRegistry& capabilities =
        options_.capabilities != nullptr ? *options_.capabilities
                                         : translation::openAiManifest();
    const translation::PairCheck pair = capabilities.checkPair(request.pair.input,
                                                               request.pair.output);
    if (!pair)
    {
        error = "openai: unsupported language pair: " + pair.detail;
        return false;
    }

    // Rates: the backend must know both (contract: zero = refused, not guessed)
    // and the resampler covers exactly 24/48/96 kHz against the 24 kHz wire.
    if (!PcmResampler::isSupportedPair(request.inputSampleRate, kWireSampleRate))
    {
        error = "openai: unsupported input sample rate " + std::to_string(request.inputSampleRate)
              + "; backend resamples 24000/48000/96000 Hz to the 24 kHz wire";
        return false;
    }
    if (!PcmResampler::isSupportedPair(kWireSampleRate, request.outputSampleRate))
    {
        error = "openai: unsupported output sample rate " + std::to_string(request.outputSampleRate)
              + "; backend resamples the 24 kHz wire to 24000/48000/96000 Hz";
        return false;
    }

    // Credentials come from the store only (contract + AGENTS.md 10). The value
    // is used to build the one auth header and never written anywhere else.
    const auto apiKey = secrets_.load(security::kOpenAiApiKey);
    if (!apiKey.has_value() || apiKey->empty())
    {
        error = "openai: no API key available in the credential store";
        return false;
    }

    request_ = request;
    {
        // 64-bit intermediates: rate * ms overflows int32 for large windows,
        // and an overflowing capacity was a real failure mode of this file's
        // first draft (caught by the test suite, recorded in the task report).
        const long long chunk = static_cast<long long>(request_.inputSampleRate) * options_.cadenceMs / 1000;
        const long long cap = static_cast<long long>(request_.inputSampleRate) * options_.maxQueuedInputMs / 1000;
        chunkInputFrames_ = static_cast<int>(chunk < 1 ? 1 : (chunk > 2'000'000'000LL ? 2'000'000'000LL : chunk));
        queueCapacityFrames_ = static_cast<int>(cap < 1 ? 1 : (cap > 2'000'000'000LL ? 2'000'000'000LL : cap));
    }

    inputResampler_.configure(request_.inputSampleRate, kWireSampleRate);
    outputResampler_.configure(kWireSampleRate, request_.outputSampleRate);
    inputResampler_.reset();
    outputResampler_.reset();

    {
        const std::lock_guard<std::mutex> lock (queueMutex_);
        queue_.clear();
        queuedFrames_ = 0;
    }

    closeRequested_ = false;
    senderStop_ = false;
    receiverStop_ = false;
    closeSent_ = false;
    drainForced_ = false;
    eventCounter_ = 0;
    {
        // A reopened session must behave like a fresh one (contract rule 6):
        // no words from the old translation survive into the new line.
        const std::lock_guard<std::mutex> lock (transcriptMutex_);
        transcriptLine_.clear();
        lastTranscriptMs_ = steadyNowMs();
    }

    // Contract rule 6: a new session is unrelated to the old one. An expiry
    // announcement belonged to the session that carried it; a server that does
    // not repeat it must not find the previous deadline still armed here.
    expiryAnnounced_.store(false, std::memory_order_relaxed);

    {
        const std::lock_guard<std::mutex> lock (lifeMutex_);
        handshakeDone_ = false;
        handshakeOk_ = false;
        senderDone_ = false;
        receiverDone_ = false;
    }

    reportTransition(SessionState::connecting);

    transport_ = factory_();
    const std::string path = "/v1/realtime/translations?model=" + model; // protocol section 2
    const ConnectResult cr = transport_->connect(options_.host, kWirePort, path,
                                                 { "Authorization: Bearer " + *apiKey });
    if (!cr.upgraded)
    {
        auto category = categoryForHttpStatus(cr.httpStatus);
        // Protocol section 9: the documented billing/account 429 is not a
        // transient connection failure - retrying cannot restore access - so it
        // maps to the category the recovery policy stops on. The code name
        // itself is classification input, not output: it never reaches the sink.
        if (cr.httpStatus == 429 && refusalErrorCode(cr.refusalBody) == "credit_balance_exhausted")
            category = translation::TranslationErrorCategory::rejectedRequest;

        const std::string message = cr.httpStatus != 0
            ? "openai: connection refused with HTTP status " + std::to_string(cr.httpStatus)
            : "openai: transport connect failed: " + cr.transportError;
        // Protocol section 9: honour Retry-After; 0 = "no hint, use backoff".
        const int retryAfterMs = cr.retryAfterSec > 0
            ? static_cast<int>((std::min)(static_cast<long long>(cr.retryAfterSec) * 1000,
                                          static_cast<long long>(24 * 3600 * 1000)))
            : 0;
        reportError(category, message, true, retryAfterMs);
        reportTransition(SessionState::faulted);
        error = message;
        return false;
    }

    // The handshake runs on the receiver because the production receive blocks
    // indefinitely; openSession bounds its wait and cancels the socket to
    // release the thread if the service never completes the handshake.
    try
    {
        receiverThread_ = std::thread([this] { receiverLoop(); });
    }
    catch (const std::exception&)
    {
        reportError(TranslationErrorCategory::internal,
                    "openai: could not start the network threads", true);
        reportTransition(SessionState::faulted);
        error = "openai: could not start the network threads";
        return false;
    }

    bool ok = false;
    bool timedOut = false;
    {
        std::unique_lock<std::mutex> lock (lifeMutex_);
        timedOut = !lifeCv_.wait_for(lock, std::chrono::milliseconds (options_.handshakeTimeoutMs),
                                     [this] { return handshakeDone_; });
        ok = handshakeOk_;
    }

    if (timedOut)
    {
        // Abort the blocked handshake by cancelling the socket (the documented
        // and live-proven release mechanism). Whatever the receiver reports
        // stands as the error event; this session is over either way.
        senderStop_ = true;
        drainForced_ = true;
        transport_->closeTransport("handshake deadline exceeded");
        if (receiverThread_.joinable())
            receiverThread_.join();
        error = "openai: handshake timed out";
        return false;
    }

    if (!ok)
    {
        // The handshake reported its own failure and faulted the session; the
        // receiver exits after publishing the outcome.
        if (receiverThread_.joinable())
            receiverThread_.join();
        error = "openai: handshake did not complete";
        return false;
    }

    // Connected (the receiver made the transition after configuration
    // confirmed - the usable trigger of protocol section 15). The sender owns
    // all writes from here on.
    try
    {
        senderThread_ = std::thread([this] { senderLoop(); });
    }
    catch (const std::exception&)
    {
        senderStop_ = true;
        transport_->closeTransport("sender unavailable");
        if (receiverThread_.joinable())
            receiverThread_.join();
        reportError(TranslationErrorCategory::internal,
                    "openai: could not start the audio sender", true);
        reportTransition(SessionState::faulted);
        error = "openai: could not start the audio sender";
        return false;
    }

    log::info(kLogComponent,
              "session open: model=" + model + " language=" + request_.pair.output
                  + " rates in/out=" + std::to_string(request_.inputSampleRate) + "/"
                  + std::to_string(request_.outputSampleRate));

    // Protocol section 12.1: this model has NO custom prompting. The field is
    // accepted (it stays in the contract for other backends) and IGNORED with a
    // warning - faking instruction support is forbidden (AGENTS.md 19).
    // Owner decision 2026-10-03 (task 012): this notice lives in the LOG only.
    // It is not a TranslationError: the session request was not refused, a
    // capability of the model simply does not include prompting - and
    // rejectedRequest is the supervisor's TERMINAL category (task 010), so
    // reporting an ordinary every-start fact as that error would mislabel the
    // operator's status line as a failure. The warning says the whole truth.
    if (!request_.instructions.empty())
    {
        log::warning(kLogComponent,
                     "session instructions ignored: the translation model supports no "
                     "custom prompting (docs/openai-realtime-protocol.md section 12.1)");
    }

    return true;
}

// ----------------------------------------------------------------- submitAudio

bool OpenAIRealtimeBackend::submitAudio(const float* samples, int frameCount, std::string& error)
{
    error.clear();

    // Contract rule 4: accepted only while connected, and a refusal is an
    // ordinary answer, not a fault - the caller keeps operating.
    if (state() != SessionState::connected)
    {
        error = std::string("openai: no open session (state is ")
              + std::string(translation::nameOf(state())) + ")";
        return false;
    }

    if (samples == nullptr || frameCount <= 0)
    {
        error = "openai: empty audio block rejected";
        return false;
    }

    const std::lock_guard<std::mutex> lock (queueMutex_);
    if (queuedFrames_ + frameCount > queueCapacityFrames_)
    {
        // Backpressure: refuse, never drop silently (AGENTS.md 19).
        error = "openai: input queue full (" + std::to_string(options_.maxQueuedInputMs)
              + " ms); block refused";
        return false;
    }

    QueuedChunk chunk;
    chunk.samples.assign(samples, samples + frameCount);
    chunk.readPos = 0;
    queue_.push_back(std::move(chunk));
    queuedFrames_ += frameCount;
    return true;
}

// ---------------------------------------------------------------- closeSession

void OpenAIRealtimeBackend::closeSession() noexcept
{
    const SessionState current = state();

    if (current == SessionState::connecting)
    {
        // A concurrent open is not the designed usage; leave the honest
        // bail-out: ask the receiver to abort and cancel the socket.
        senderStop_ = true;
        drainForced_ = true;
        if (transport_)
            transport_->closeTransport("open aborted");
    }
    else if (current == SessionState::connected)
    {
        // Contract rule 5 / protocol section 4: the sender stops appending and
        // sends session.close; the receiver keeps draining until
        // session.closed (most late deltas arrive in this window - section 15),
        // bounded by closeDrainTimeoutMs; a silent service is force-cancelled.
        closeRequested_ = true;
        lifeCv_.notify_all();

        {
            std::unique_lock<std::mutex> lock (lifeMutex_);
            lifeCv_.wait_for(lock, std::chrono::milliseconds (2000 + 2 * options_.cadenceMs),
                             [this] { return senderDone_; });
        }
        if (senderThread_.joinable())
            senderThread_.join();

        bool drained = false;
        {
            std::unique_lock<std::mutex> lock (lifeMutex_);
            drained = lifeCv_.wait_for(lock, std::chrono::milliseconds (options_.closeDrainTimeoutMs),
                                       [this] { return receiverDone_; });
            if (!drained)
            {
                drainForced_ = true;
                lock.unlock();
                transport_->closeTransport("drain deadline exceeded");
                lock.lock();
                lifeCv_.wait(lock, [this] { return receiverDone_; });
            }
        }
    }

    // Faulted/closed: the threads have ended or are ending; reaping is all
    // that is left. join cannot be cancelled: if a thread ever outlives every
    // bound above, blocking here is the honest behavior (no callbacks racing
    // the caller's teardown, rule 5).
    try
    {
        if (senderThread_.joinable())
            senderThread_.join();
        if (receiverThread_.joinable())
            receiverThread_.join();
    }
    catch (const std::exception&)
    {
        log::error(kLogComponent, "thread join failed during closeSession");
    }
    senderThread_ = std::thread {};
    receiverThread_ = std::thread {};

    // Task 013: session close is the one text boundary the provider guarantees
    // (protocol section 6 - nothing else closes a line), so the open subtitle
    // line is flushed as a final while callbacks are still lawful: before this
    // function returns (rule 5) and before the session is announced closed,
    // which keeps the audience's tail of the translation in order in history.
    // A faulted death may have let the controller close the same words first;
    // the TextPipeline duplicate guard eats whichever copy arrives second.
    {
        const std::string line = takeTranscriptLine();

        if (!line.empty() && sink_ != nullptr)
            sink_->onFinalText(line);
    }

    reportTransition(SessionState::closed);

    // Per-session state resets so a reopened session is unrelated (rule 6).
    // The transport object stays alive until the next open or the destructor
    // (the socket itself was released by closeTransport); that is deliberate:
    // post-close inspection of sent frames is how the protocol tests observe
    // the graceful close.
    {
        const std::lock_guard<std::mutex> lock (queueMutex_);
        queue_.clear();
        queuedFrames_ = 0;
    }
    inputResampler_.reset();
    outputResampler_.reset();
    eventCounter_ = 0;
    closeRequested_ = false;
    senderStop_ = false;
    receiverStop_ = false;
    closeSent_ = false;
    drainForced_ = false;
    expiryAnnounced_.store(false, std::memory_order_relaxed);   // rule 6 again: nothing survives the session
    {
        const std::lock_guard<std::mutex> lock (lifeMutex_);
        handshakeDone_ = false;
        handshakeOk_ = false;
        senderDone_ = false;
        receiverDone_ = false;
    }
}

bool OpenAIRealtimeBackend::serverSessionExpiryRemainingMs(long long& remainingMsOut) const noexcept
{
    if (!expiryAnnounced_.load(std::memory_order_acquire))
    {
        remainingMsOut = 0;
        return false;
    }
    remainingMsOut = expiryDeadlineSteadyMs_.load(std::memory_order_relaxed) - steadyNowMs();
    return true;
}

// ---------------------------------------------------------------- event loops

void OpenAIRealtimeBackend::noteSessionExpiry(const std::string& eventText)
{
    json parsed = json::parse(eventText, nullptr, false);
    if (parsed.is_discarded() || !parsed.contains("session") || !parsed["session"].is_object())
        return;   // nothing this event can teach us about the deadline

    const json& session = parsed["session"];
    if (!session.contains("expires_at") || !session["expires_at"].is_number())
    {
        // Absent is documented behavior (section 4bis: the field is optional)
        // and says NOTHING about the deadline - least of all "revoked": the
        // very event that completes our handshake is a session.updated that may
        // omit the field, while the session.created of the same session
        // announced it. So an absent field keeps the last known announcement;
        // only the SESSION's end clears it (contract rule 6, in closeSession).
        return;
    }

    const long long expiresAtEpochSeconds = session["expires_at"].get<long long>();
    const long long remainingMs = expiresAtEpochSeconds * 1000LL - wallNowMs();

    if (remainingMs > 0 && remainingMs <= kExpirySanityMaxMs)
    {
        // Value first, flag with release after it: a reader that acquires the
        // flag is guaranteed to see this deadline, never a stale pairing.
        expiryDeadlineSteadyMs_.store(steadyNowMs() + remainingMs, std::memory_order_relaxed);
        expiryAnnounced_.store(true, std::memory_order_release);
        log::info(kLogComponent, "server announced session expiry in " + std::to_string(remainingMs / 1000)
                                     + " s (session.expires_at, protocol docs section 4bis)");
    }
    else
    {
        const bool hadAnnouncement = expiryAnnounced_.exchange(false, std::memory_order_relaxed);
        log::warning(kLogComponent, "session.expires_at announced an unusable value ("
                                         + std::to_string(expiresAtEpochSeconds)
                                         + " epoch s; the delta against our clock must land in (0, 24 h]"
                                         + (hadAnnouncement ? ", previous announcement dropped" : "")
                                         + ") - local age policy applies");
    }
}

void OpenAIRealtimeBackend::receiverLoop()
{
    const bool ok = handshake();

    if (ok)
    {
        // The usable trigger of protocol section 15: configuration confirmed.
        // Reported BEFORE the rendezvous flag so a caller that observes a
        // successful open always observes the connected transition too.
        reportTransition(SessionState::connected);
    }

    {
        const std::lock_guard<std::mutex> lock (lifeMutex_);
        handshakeDone_ = true;
        handshakeOk_ = ok;
    }
    lifeCv_.notify_all();

    if (ok)
    {
        while (true)
        {
            const ReceiveEvent ev = transport_->receive();
            bool endLoop = false;

            switch (ev.kind)
            {
                case ReceiveKind::message:
                {
                    const EventResult r = handleEvent(ev.text);
                    if (r != EventResult::keepGoing)
                        endLoop = true;
                    break;
                }
                case ReceiveKind::timeout:
                    // Only the scripted test transport times out; leaving on
                    // stop keeps offline loops bounded.
                    if (senderStop_.load())
                        endLoop = true;
                    break;
                case ReceiveKind::closed:
                    if (closeSent_.load() || state() != SessionState::connected)
                        reportTransition(SessionState::closed); // expected tail of our close
                    else
                    {
                        std::string message = "openai: connection closed by peer mid-session";
                        if (!ev.closeReason.empty())
                            message += " (reason: " + ev.closeReason + ")";
                        reportError(TranslationErrorCategory::connection, message, true);
                        reportTransition(SessionState::faulted);
                    }
                    endLoop = true;
                    break;
                case ReceiveKind::error:
                    if (drainForced_.load())
                    {
                        // Our cancel after the drain deadline: the service
                        // never confirmed the close. Honest, non-fatal: the
                        // session ends as closed; late audio may have been lost.
                        reportError(
                            TranslationErrorCategory::protocol,
                            "openai: timed out waiting for the end-of-session event; socket "
                            "dropped, late translated audio may have been lost",
                            false);
                        reportTransition(SessionState::closed);
                    }
                    else if (state() == SessionState::connected)
                    {
                        reportError(TranslationErrorCategory::connection,
                                    "openai: transport error: " + ev.transportError, true);
                        reportTransition(SessionState::faulted);
                    }
                    endLoop = true;
                    break;
            }
            if (endLoop)
                break;
        }
    }

    // Leave nothing hanging: the sender must not append into a dead session,
    // and a blocked/sleeping loop must see the end.
    senderStop_ = true;
    transport_->closeTransport("session ended");
    {
        const std::lock_guard<std::mutex> lock (lifeMutex_);
        receiverDone_ = true;
    }
    lifeCv_.notify_all();
}

bool OpenAIRealtimeBackend::handshake()
{
    // Protocol sections 2/4 and section 15: session.created must be first;
    // our session.update is answered by session.updated; only then is the
    // session usable. An error answer to our own request means this session
    // can never be configured: faulted. (The open deadline lives in
    // openSession; this function returns when the socket reports anything
    // decisive or dies - the cancel of a timed-out wait arrives as an error.)
    enum class Phase
    {
        created,
        updated
    };
    Phase phase = Phase::created;

    while (true)
    {
        if (senderStop_.load() && phase == Phase::created)
        {
            reportError(TranslationErrorCategory::connection, "openai: open aborted", true);
            reportTransition(SessionState::faulted);
            return false;
        }

        const ReceiveEvent ev = transport_->receive();
        switch (ev.kind)
        {
            case ReceiveKind::timeout:
                continue;
            case ReceiveKind::closed:
            {
                std::string message = "openai: connection closed during handshake";
                if (!ev.closeReason.empty())
                    message += " (peer reason: " + ev.closeReason + ")";
                reportError(TranslationErrorCategory::connection, message, true);
                reportTransition(SessionState::faulted);
                return false;
            }
            case ReceiveKind::error:
            {
                const std::string what = phase == Phase::created ? "session creation"
                                                                  : "configuration confirmation";
                reportError(TranslationErrorCategory::connection,
                            "openai: handshake did not complete (" + what
                                + "): transport error"
                                + (drainForced_.load() ? " - open deadline exceeded" : ": " + ev.transportError),
                            true);
                reportTransition(SessionState::faulted);
                return false;
            }
            case ReceiveKind::message:
            {
                json parsed = json::parse(ev.text, nullptr, false);
                if (parsed.is_discarded())
                {
                    log::warning(kLogComponent, "malformed JSON during handshake, ignored");
                    continue;
                }
                const std::string type = stringField(parsed, "type");
                if (type == (phase == Phase::created ? "session.created" : "session.updated"))
                {
                    // Both handshake events carry the session object; the expiry
                    // announcement is recorded from whichever arrived (section 4bis).
                    noteSessionExpiry(ev.text);
                    if (phase == Phase::created)
                    {
                        phase = Phase::updated;
                        if (!sendTextEvent(buildSessionUpdate()))
                        {
                            reportError(TranslationErrorCategory::connection,
                                        "openai: failed to send session configuration", true);
                            reportTransition(SessionState::faulted);
                            return false;
                        }
                    }
                    else
                        return true;
                    continue;
                }
                if (type == "error")
                {
                    std::string detail = "unspecified";
                    if (parsed.contains("error") && parsed["error"].is_object())
                    {
                        const std::string m = stringField(parsed["error"], "message");
                        if (!m.empty())
                            detail = m;
                    }
                    const auto category = containsAudioWord(detail)
                        ? TranslationErrorCategory::audioFormat
                        : TranslationErrorCategory::rejectedRequest;
                    reportError(category, "openai: request rejected during handshake: " + detail,
                                true);
                    reportTransition(SessionState::faulted);
                    return false;
                }
                log::debug(kLogComponent, "pre-ready event: " + type);
                continue;
            }
        }
    }
}

void OpenAIRealtimeBackend::senderLoop()
{
    using clock = std::chrono::steady_clock;
    const auto cadence = std::chrono::milliseconds(options_.cadenceMs);
    auto nextDue = clock::now();

    std::vector<float> inChunk (static_cast<std::size_t>(chunkInputFrames_));
    std::vector<float> wireFloat (static_cast<std::size_t>(
        PcmResampler::maxOutputFor(chunkInputFrames_)));
    std::vector<std::int16_t> wirePcm (static_cast<std::size_t>(
        PcmResampler::maxOutputFor(chunkInputFrames_)));

    while (true)
    {
        if (senderStop_.load())
            break;

        if (closeRequested_.load())
        {
            // Stop appending; the receiver drains. Section 4 rule 5: the close
            // event must actually reach the service before the socket dies.
            if (sendTextEvent(buildSessionClose()))
                closeSent_ = true;
            else
            {
                reportError(TranslationErrorCategory::connection,
                            "openai: failed to send session close", true);
                reportTransition(SessionState::faulted);
                senderStop_ = true;
            }
            break;
        }

        auto now = clock::now();
        if (now >= nextDue)
        {
            // Send everything due (catch-up): model time is contiguous over
            // appended audio (section 7), so after a scheduling hiccup the
            // honest recovery is late real audio, never skipped audio time and
            // never a silence-only lie about the gap. The submit queue bounds
            // the backlog by refusing new frames (rule 4).
            bool failed = false;
            int sentThisPass = 0;
            while (now >= nextDue && !closeRequested_.load() && !senderStop_.load())
            {
                if (!sendAppendFrame(inChunk, wireFloat, wirePcm))
                {
                    reportError(TranslationErrorCategory::connection,
                                "openai: socket write failed while streaming audio", true);
                    reportTransition(SessionState::faulted);
                    senderStop_ = true;
                    failed = true;
                    break;
                }
                nextDue += cadence;
                ++sentThisPass;
                now = clock::now();
            }
            if (failed)
                break;
            if (sentThisPass > 5)
                log::warning(kLogComponent, "append sender caught up " + std::to_string(sentThisPass)
                                                + " chunks in one pass (scheduling stall)");
        }

        // Subtitle settle policy (task 013): the sender owns the stream clock,
        // so the line pause is judged here - once per wake-up, a cheap peek at
        // the assembled line under its own mutex. transcriptSettleMs documents
        // why the pause rule exists at all: the wire has no line boundary, so
        // the product must supply one.
        settleTranscriptLine(steadyNowMs());

        // Sleep until the next chunk is due, with a bounded slice so stop/close
        // requests are seen quickly; the cv lets closeSession wake us at once.
        auto wakeAt = nextDue;
        const auto sliceEnd = clock::now() + std::chrono::milliseconds (50);
        if (sliceEnd < wakeAt)
            wakeAt = sliceEnd;
        {
            std::unique_lock<std::mutex> lock (lifeMutex_);
            lifeCv_.wait_until(lock, wakeAt);
        }
    }

    {
        const std::lock_guard<std::mutex> lock (lifeMutex_);
        senderDone_ = true;
    }
    lifeCv_.notify_all();
}

// --------------------------------------------------------------- event mapping

OpenAIRealtimeBackend::EventResult OpenAIRealtimeBackend::handleEvent(const std::string& text)
{
    json parsed = json::parse(text, nullptr, false);
    if (parsed.is_discarded())
    {
        reportError(TranslationErrorCategory::protocol,
                    "openai: malformed event received", false);
        return EventResult::keepGoing;
    }

    const std::string type = stringField(parsed, "type");
    if (type.empty())
    {
        reportError(TranslationErrorCategory::protocol,
                    "openai: event without a type field", false);
        return EventResult::keepGoing;
    }

    if (type == "session.created")
    {
        log::warning(kLogComponent, "unexpected repeat of the session-created event");
        noteSessionExpiry(text);   // even unexpected, the session object's word counts
        return EventResult::keepGoing;
    }

    if (type == "session.updated")
    {
        log::debug(kLogComponent, "session configuration confirmed/changed");
        noteSessionExpiry(text);   // the full object is re-carried; the deadline refreshes
        return EventResult::keepGoing;
    }

    if (type == "session.closed")
    {
        if (!closeRequested_.load())
            log::warning(kLogComponent, "service closed the session before our stop request");
        reportTransition(SessionState::closed);
        return EventResult::endClean;
    }

    if (type == "session.output_audio.delta")
    {
        if (!parsed.contains("delta") || !parsed["delta"].is_string())
        {
            reportError(TranslationErrorCategory::protocol,
                        "openai: audio delta without a payload", false);
            return EventResult::keepGoing;
        }

        // Optional metadata fields are authoritative when present (section 7);
        // their absence keeps the documented wire default of 24 kHz mono PCM16.
        if (parsed.contains("format") && stringField(parsed, "format") != "pcm16")
        {
            reportError(TranslationErrorCategory::audioFormat,
                        "openai: audio delta declared an unsupported format; block dropped",
                        false);
            return EventResult::keepGoing;
        }
        int channels = kWireChannels;
        if (intField(parsed, "channels", channels) && channels != kWireChannels)
        {
            reportError(TranslationErrorCategory::audioFormat,
                        "openai: audio delta channel count does not match the 24 kHz mono wire "
                        "contract; block dropped",
                        false);
            return EventResult::keepGoing;
        }
        int sampleRate = kWireSampleRate;
        if (intField(parsed, "sample_rate", sampleRate) && sampleRate != kWireSampleRate)
        {
            reportError(TranslationErrorCategory::audioFormat,
                        "openai: audio delta sample rate does not match the 24 kHz wire contract; "
                        "block dropped",
                        false);
            return EventResult::keepGoing;
        }

        std::vector<std::uint8_t> bytes;
        if (!base64Decode(parsed["delta"].get<std::string>(), bytes))
        {
            reportError(TranslationErrorCategory::protocol,
                        "openai: audio delta payload is not valid base64; block dropped", false);
            return EventResult::keepGoing;
        }
        deliverAudioDelta(bytes);
        return EventResult::keepGoing;
    }

    if (type == "session.output_transcript.delta")
    {
        // Append-only fragments, verbatim (section 8: the fragments carry their
        // own spacing; no unconditional space is ever inserted here). Task 013
        // put the assembly HERE on purpose: the sink's partial channel means
        // "the whole line as it currently reads" - the snapshot semantics the
        // TextPipeline and every downstream consumer rely on - and assembling
        // fragments into that snapshot is provider knowledge, which stops at
        // this file (AGENTS.md 7).
        if (!parsed.contains("delta") || !parsed["delta"].is_string())
        {
            reportError(TranslationErrorCategory::protocol,
                        "openai: transcript delta without a payload", false);
            return EventResult::keepGoing;
        }
        {
            const std::lock_guard<std::mutex> lock(transcriptMutex_);
            transcriptLine_ += stringField(parsed, "delta");
            lastTranscriptMs_ = steadyNowMs();
        }
        if (sink_ != nullptr)
            sink_->onPartialText(transcriptSnapshot());
        return EventResult::keepGoing;
    }

    if (type == "session.input_transcript.delta")
    {
        // Source-language transcript. The typed text pipeline exists now
        // (task 013), but the sink's contract is one translated-text channel:
        // a source lane would be a contract decision, not an accident of this
        // file. We never enable input transcription in session.update (section
        // 12.7: omit = not sent), so receiving one is unexpected: log it, do
        // not misdeliver it as translated text, do not pretend it is an error.
        log::debug(kLogComponent,
                   "source transcript delta ignored (single text channel by contract; "
                   "source lane is an explicit extension point)");
        return EventResult::keepGoing;
    }

    if (type == "error")
    {
        // Section 9: an error event is an event, not a death sentence. The
        // session stays open unless the transport itself dies.
        std::string message = "unspecified";
        std::string errorType;
        if (parsed.contains("error") && parsed["error"].is_object())
        {
            message = stringField(parsed["error"], "message");
            if (message.empty())
                message = "unspecified";
            errorType = stringField(parsed["error"], "type");
        }

        auto category = TranslationErrorCategory::internal;
        if (containsAudioWord(message))
            category = TranslationErrorCategory::audioFormat;
        else if (errorType == "invalid_request_error")
            category = TranslationErrorCategory::rejectedRequest;

        // The provider's message text is passed through for the operator's
        // diagnostics; provider type/code NAMES stay in this log line, never
        // in the sink message (section 9 mapping rule).
        log::warning(kLogComponent,
                     "server error event (type=" + errorType + ") mapped to "
                         + std::string(translation::nameOf(category)) + ": " + message);
        reportError(category, "openai: translation service reported: " + message, false);
        return EventResult::keepGoing;
    }

    // Protocol section 6: the reference contains exactly seven server events.
    // An eighth kind is not protocol; report it as a protocol break, keep the
    // session (tolerant of future additions, never silent).
    log::warning(kLogComponent, "unrecognized server event type: " + type);
    reportError(TranslationErrorCategory::protocol,
                "openai: unrecognized event received from the service", false);
    return EventResult::keepGoing;
}

void OpenAIRealtimeBackend::deliverAudioDelta(const std::vector<std::uint8_t>& pcm16Bytes)
{
    if (pcm16Bytes.empty())
        return;

    if (pcm16Bytes.size() % 2 != 0)
    {
        reportError(TranslationErrorCategory::protocol,
                    "openai: audio delta length is not a whole number of samples; block dropped",
                    false);
        return;
    }

    const int wireFrames = static_cast<int>(pcm16Bytes.size() / 2);
    std::vector<float> floats (static_cast<std::size_t>(wireFrames));
    const std::int16_t* pcm = reinterpret_cast<const std::int16_t*>(pcm16Bytes.data());
    for (int i = 0; i < wireFrames; ++i)
        floats[static_cast<std::size_t>(i)] = static_cast<float>(pcm[i]) / 32768.0f;

    std::vector<float> out (static_cast<std::size_t>(
        PcmResampler::maxOutputFor(wireFrames)));
    const int produced = outputResampler_.process(floats.data(), wireFrames, out.data(),
                                                  static_cast<int>(out.size()));
    if (produced <= 0)
    {
        if (produced < 0)
            reportError(TranslationErrorCategory::internal,
                        "openai: resampler capacity violation; block dropped", false);
        return;
    }

    if (sink_ != nullptr)
        sink_->onTranslatedAudio(out.data(), produced, request_.outputSampleRate);
}

bool OpenAIRealtimeBackend::sendAppendFrame(std::vector<float>& inChunk,
                                            std::vector<float>& wireFloat,
                                            std::vector<std::int16_t>& wirePcm)
{
    // Continuous cadence including silence (protocol section 7 / section 15:
    // this is also what keeps the connection from idling out).
    int taken = 0;
    while (taken < chunkInputFrames_)
    {
        const std::lock_guard<std::mutex> lock (queueMutex_);
        if (queue_.empty())
            break;
        QueuedChunk& front = queue_.front();
        const int available = static_cast<int>(front.samples.size()) - front.readPos;
        const int take = std::min(chunkInputFrames_ - taken, available);
        std::copy(front.samples.begin() + front.readPos,
                  front.samples.begin() + front.readPos + take,
                  inChunk.begin() + taken);
        front.readPos += take;
        taken += take;
        queuedFrames_ -= take;
        if (front.readPos >= static_cast<int>(front.samples.size()))
            queue_.pop_front();
    }
    if (taken < chunkInputFrames_)
        std::fill(inChunk.begin() + taken, inChunk.end(), 0.0f);

    const int produced = inputResampler_.process(inChunk.data(), chunkInputFrames_,
                                                 wireFloat.data(),
                                                 static_cast<int>(wireFloat.size()));
    if (produced < 0)
    {
        reportError(TranslationErrorCategory::internal,
                    "openai: input resampler capacity violation; frame skipped", false);
        return true;
    }
    if (produced == 0)
        return true; // nothing to emit on this tick; the cadence clock advances

    for (int i = 0; i < produced; ++i)
        wirePcm[static_cast<std::size_t>(i)] = floatToInt16(wireFloat[static_cast<std::size_t>(i)]);

    // Windows is little-endian; the int16 array is the wire PCM16 payload.
    const std::string encoded = base64Encode(
        reinterpret_cast<const std::uint8_t*>(wirePcm.data()),
        static_cast<std::size_t>(produced) * sizeof(std::int16_t));

    return sendTextEvent(buildAppend(encoded));
}

// ---------------------------------------------------------------- protocol map

std::string OpenAIRealtimeBackend::buildSessionUpdate()
{
    // Protocol section 5.1, wire shape verified live: the configuration travels
    // inside a `session` object - {"type":"session.update","session":{"audio":
    // {"output":{"language":...}}}}. The first draft omitted the wrapper and
    // the service answered "Missing required parameter: 'session'" (the
    // protocol doc section 15 keeps the record). Transcription and noise
    // reduction are omitted, never sent with guessed values (section 12.7);
    // instructions have no wire equivalent (section 12.1, warning handled in
    // openSession).
    json j;
    j["event_id"] = nextEventId();
    j["type"] = "session.update";
    j["session"]["audio"]["output"]["language"] = request_.pair.output;
    return j.dump();
}

std::string OpenAIRealtimeBackend::buildAppend(const std::string& base64Audio)
{
    json j;
    j["event_id"] = nextEventId();
    j["type"] = "session.input_audio_buffer.append";
    j["audio"] = base64Audio;
    return j.dump();
}

std::string OpenAIRealtimeBackend::buildSessionClose()
{
    json j;
    j["event_id"] = nextEventId();
    j["type"] = "session.close";
    return j.dump();
}

std::string OpenAIRealtimeBackend::nextEventId()
{
    return "evt_" + std::to_string(++eventCounter_);
}

// ------------------------------------------------------- subtitle line assembly

void OpenAIRealtimeBackend::settleTranscriptLine(long long nowMs)
{
    if (options_.transcriptSettleMs <= 0)
        return;   // pause rule disabled: the line ends only at session close

    std::string line;

    {
        const std::lock_guard<std::mutex> lock (transcriptMutex_);

        if (transcriptLine_.empty())
            return;

        if (nowMs - lastTranscriptMs_ < options_.transcriptSettleMs)
            return;

        // The pause has elapsed with the stream still running: the translator
        // stopped talking. The line as delivered is what there is of it -
        // complete as far as this endpoint's stream tells us - and it goes out
        // as a final. The next fragment starts a new line.
        line.swap(transcriptLine_);
    }

    // Sink callback outside the lock (never chain transcriptMutex_ into the
    // pipeline's lock or the NDI publish; the receiver uses the same rule).
    log::debug(kLogComponent,
               "subtitle line settled after a " + std::to_string(options_.transcriptSettleMs)
                   + " ms pause: " + std::to_string(line.size()) + " bytes");

    if (sink_ != nullptr)
        sink_->onFinalText(line);
}

std::string OpenAIRealtimeBackend::transcriptSnapshot()
{
    const std::lock_guard<std::mutex> lock (transcriptMutex_);
    return transcriptLine_;
}

std::string OpenAIRealtimeBackend::takeTranscriptLine()
{
    const std::lock_guard<std::mutex> lock (transcriptMutex_);

    std::string line;
    line.swap(transcriptLine_);
    return line;
}

bool OpenAIRealtimeBackend::sendTextEvent(const std::string& text)
{
    std::string error;
    if (transport_ == nullptr || !transport_->sendText(text, error))
    {
        log::error(kLogComponent, "send failed: " + error);
        return false;
    }
    return true;
}

// --------------------------------------------------------------------- plumbing

void OpenAIRealtimeBackend::reportTransition(SessionState next)
{
    const int prev = state_.exchange(static_cast<int>(next), std::memory_order_relaxed);
    if (prev != static_cast<int>(next) && sink_ != nullptr)
        sink_->onSessionStateChanged(next);
}

void OpenAIRealtimeBackend::reportError(TranslationErrorCategory category,
                                        const std::string& message,
                                        bool fatal,
                                        int retryAfterMs)
{
    if (fatal)
        log::error(kLogComponent, message);
    else
        log::warning(kLogComponent, message);

    if (sink_ != nullptr)
    {
        TranslationError err { category, message, fatal };
        err.retryAfterMs = retryAfterMs;
        sink_->onTranslationError(err);
    }
}

} // namespace network
} // namespace liveai
