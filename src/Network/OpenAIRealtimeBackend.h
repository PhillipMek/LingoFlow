#pragma once
//
// OpenAIRealtimeBackend - the production translation backend.
//
// It implements the translation contract against the protocol documented in
// docs/openai-realtime-protocol.md - that file is the single normative source;
// nothing here may assume a protocol fact the file does not record, and every
// protocol shape below cites its section. All OpenAI knowledge stops at this
// translation boundary: the sink sees only SessionState, TranslationError
// categories and float audio, exactly as the contract header requires.
//
// Threading (the project rules, docs/threading.md). The shape is dictated by the
// live full-duplex diagnostic of 2026-10-02, not by taste: the synchronous
// WinHTTP WinHttpWebSocketReceive BLOCKS INDEFINITELY while the socket is idle
// (it ignores receive timeouts) and returns ERROR_WINHTTP_OPERATION_CANCELLED
// when another thread closes the handle; meanwhile WinHttpWebSocketSend from a
// second thread works while a receive is pending (39 concurrent appends sent
// and answered live). A single loop cannot both stream at cadence and drain,
// so full duplex means exactly two session threads:
//   * the RECEIVER runs the handshake (created -> update -> updated) then
//     blocks in receive(), dispatching every server event to the sink;
//   * the SENDER keeps the continuous 200 ms append cadence (protocol section
//     7, "keep appending, including silence" - also the application-level
//     keepalive of section 15) and sends session.close when the stop is
//     requested.
//
// Blocking contract surface (documented, bounded):
//   * Sizes: every inbound byte is budgeted before it is allocated - 16 MiB
//     reassembled WebSocket message (transport severs the connection beyond it),
//     256 KiB decoded audio delta (block dropped, session kept). The bounds and
//     their margins live in Network/NetworkLimits.h.
//   * openSession() connects, handshakes and returns only when the session is
//     usable or over - bounded by handshakeTimeoutMs;
//   * closeSession() asks the sender to stop, sends session.close, waits the
//     receiver's drain within closeDrainTimeoutMs (measured live: most late
//     deltas arrive AFTER close, protocol section 15 - draining is the whole
//     point of closing), force-cancels the socket if session.closed never
//     comes, and joins both threads - which is what guarantees rule 5's "no
//     sink callback after closeSession() returns";
//   * submitAudio() touches only a mutex-guarded queue: nothing in this file
//     is reachable from the audio callback.
//   * openSession()/closeSession()/submitAudio() are called from one serialized
//     worker (the application controller); they are not designed for
//     concurrent open+close.
//
// Rates (protocol section 7): the wire speaks 24 kHz PCM16 mono in both
// directions; inputSampleRate/outputSampleRate from the request define the
// conversion done HERE on the worker threads by PcmResampler. The backend
// accepts 24/44.1/48/88.2/96 kHz on either side - exactly the config's device
// set plus the wire rate itself, so no device choice the operator may legally
// save can die at Start Translation. A pair
// outside that set is refused at openSession(), never guessed at.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Network/IWebSocketTransport.h"
#include "Network/PcmResampler.h"
#include "Translation/ITranslationBackend.h"

namespace liveai {
namespace security {
class ISecretStore;
}

namespace translation {
class LanguageRegistry;
}

namespace network {

/// Tunables with protocol-derived defaults. Tests drive them; the product uses
/// the defaults.
struct OpenAIRealtimeOptions
{
    std::string host = "api.openai.com";
    /// Protocol section 11: the only model this backend may talk to until the
    /// docs say otherwise. Any other value in SessionRequest::model is a
    /// rejectedRequest-style refusal.
    std::string defaultModel = "gpt-realtime-translate";
    /// The optional OpenAI-Safety-Identifier value (docs section 3, code
    /// review P2 2026-10-05): the composition root places the installation
    /// digest here (Network/SafetyIdentifier.h). Empty means the header is
    /// not sent; a non-empty but unsendable value is dropped at connect with
    /// a warning - the provider recommends this header and never requires it,
    /// so nothing about it may take a show down.
    std::string safetyIdentifier;
    int handshakeTimeoutMs = 15000;
    /// Measured live drain after session.close ran ~5.5 s (protocol section 15);
    /// 10 s bounds it with headroom.
    int closeDrainTimeoutMs = 10000;
    /// The engine frame size, protocol section 7: "the engine consumes 200 ms
    /// frames" and recommends one such chunk per append.
    int cadenceMs = 200;
    /// Backpressure bound for submitAudio(): beyond this the backend refuses,
    /// it never drops silently (the streamer decides caller behavior). 10 s absorbs
    /// transient network stalls measured live.
    int maxQueuedInputMs = 10000;
    /// Subtitle line policy, a PRODUCT decision, not a protocol
    /// fact: the wire gives no line boundary (protocol sections 6 and 8 - no
    /// `*.done` exists and transcript deltas are append-only fragments), so
    /// the backend assembles fragments into one translated line and calls it
    /// final when the line has received no new fragment for this long while
    /// the session kept streaming. A line that ends this way is complete as
    /// far as the provider is concerned; closeSession flushes the open line
    /// unconditionally. 0 disables the pause rule (the line then ends only at
    /// session close); the TextPipeline's draft cap is the last bound either
    /// way. Tests use small values; 2500 ms is a readable-subtitle guess, to
    /// be confirmed against the operator's eyes at a rig checkpoint.
    int transcriptSettleMs = 2500;
    /// Capability source for pair validation: nullptr = the shipped
    /// frozen manifest (`translation::openAiManifest()`). The seam exists
    /// because the project rules prefers dynamic capabilities when a provider offers
    /// them - this key does not (protocol section 15), so the manifest is the
    /// default, not a fallback; a future dynamic manifest plugs in here.
    const translation::LanguageRegistry* capabilities = nullptr;
};

using TransportFactory = std::function<std::unique_ptr<IWebSocketTransport>()>;

class OpenAIRealtimeBackend final : public translation::ITranslationBackend
{
public:
    /// Credentials are resolved from the store at openSession() time (contract:
    /// they never travel through the interface); the factory seam exists for
    /// offline protocol tests and defaults to the production WinHTTP transport.
    explicit OpenAIRealtimeBackend(security::ISecretStore& secrets,
                                   OpenAIRealtimeOptions options = {},
                                   TransportFactory factory = &makeWinHttpTransport);

    ~OpenAIRealtimeBackend() override;

    OpenAIRealtimeBackend(const OpenAIRealtimeBackend&) = delete;
    OpenAIRealtimeBackend& operator=(const OpenAIRealtimeBackend&) = delete;

    // --------------------------------------------------------------- contract
    std::string_view name() const noexcept override;
    translation::SessionState state() const noexcept override;
    void setSink(translation::ITranslationSink& sink) noexcept override;
    bool openSession(const translation::SessionRequest& request, std::string& error) override;
    bool submitAudio(const float* samples, int frameCount, std::string& error) override;
    void closeSession() noexcept override;

    /// The server's own words about when this session ends, recorded from
    /// session.created/session.updated (protocol docs section 4bis). Read by the
    /// reconnect supervisor when it arms its session deadline.
    bool serverSessionExpiryRemainingMs(long long& remainingMsOut) const noexcept override;

private:
    enum class EventResult
    {
        keepGoing,
        endClean, ///< session.closed observed: the receiver exits normally
        endFatal  ///< a mapped fatal failure ended this session
    };

    // ---- receiver thread ----
    void receiverLoop();
    bool handshake(); ///< created -> update -> updated; reports its own failures

    // ---- sender thread ----
    void senderLoop();

    // ---- shared plumbing ----
    EventResult handleEvent(const std::string& text);
    void deliverAudioDelta(const std::vector<std::uint8_t>& pcm16Bytes);
    bool sendAppendFrame(std::vector<float>& inChunk,
                         std::vector<float>& wireFloat,
                         std::vector<std::int16_t>& wirePcm);
    bool sendTextEvent(const std::string& jsonText);
    std::string buildSessionUpdate();
    std::string buildAppend(const std::string& base64Audio);
    std::string buildSessionClose();
    std::string nextEventId();

    /// Report a state transition to the sink exactly once per real change
    /// (contract rule 3).
    void reportTransition(translation::SessionState next);
    void reportError(translation::TranslationErrorCategory category,
                     const std::string& message,
                     bool fatal,
                     int retryAfterMs = 0);

    /// Read `session.expires_at` out of a session.created/session.updated event
    /// text and project it onto the steady clock (protocol docs section 4bis:
    /// server wall clock vs ours, one delta at arrival; non-positive or >24 h
    /// answers are refused as unusable with a log line, never trusted). An
    /// absent field keeps the last known announcement - the session still has
    /// whatever deadline it declared; only closing the session clears it.
    /// Written by the receiver thread; the atomics make every other thread's
    /// read safe.
    void noteSessionExpiry(const std::string& eventText);

    OpenAIRealtimeOptions options_;
    security::ISecretStore& secrets_;
    TransportFactory factory_;

    translation::ITranslationSink* sink_ = nullptr;
    std::atomic<int> state_ { static_cast<int>(translation::SessionState::closed) };

    std::unique_ptr<IWebSocketTransport> transport_;
    std::thread receiverThread_;
    std::thread senderThread_;

    // ---- per-session configuration (written before the threads start) ----
    translation::SessionRequest request_;
    int chunkInputFrames_ = 0; ///< cadenceMs worth of frames at inputSampleRate
    int queueCapacityFrames_ = 0;
    std::atomic<bool> closeRequested_ { false };
    std::atomic<bool> senderStop_ { false };  ///< fatal/teardown: loops leave
    std::atomic<bool> receiverStop_ { false }; ///< force-cancel the pending receive
    std::atomic<bool> closeSent_ { false };   ///< sender delivered session.close
    std::atomic<bool> drainForced_ { false }; ///< last socket close was our cancel
    std::atomic<std::uint64_t> eventCounter_ { 0 };

    /// Server-announced session expiry (protocol docs section 4bis): deadline in
    /// steady-clock milliseconds, valid only while expiryAnnounced_ is true. The
    /// deadline is stored before the flag (release/acquire pairing) so a reader
    /// that sees the flag also sees the value.
    std::atomic<bool> expiryAnnounced_ { false };
    std::atomic<long long> expiryDeadlineSteadyMs_ { 0 };

    // ---- rendezvous flags (guarded by lifeMutex_/lifeCv_) ----
    std::mutex lifeMutex_;
    std::condition_variable lifeCv_;
    bool handshakeDone_ = false;
    bool handshakeOk_ = false;
    bool senderDone_ = false;
    bool receiverDone_ = false;

    PcmResampler inputResampler_;  ///< request input rate -> 24 kHz wire
    PcmResampler outputResampler_; ///< 24 kHz wire -> request output rate

    // ---- submit queue: the shared wall between callers and the sender ----
    struct QueuedChunk
    {
        std::vector<float> samples;
        int readPos = 0;
    };
    std::mutex queueMutex_;
    std::deque<QueuedChunk> queue_;
    int queuedFrames_ = 0;

    // ---- translated subtitle line assembly ----
    // Protocol section 8: transcript deltas are append-only fragments carrying
    // their own spacing; the sink contract wants whole-line snapshots. The
    // assembly lives HERE - protocol semantics stop at this file, and the
    // TextPipeline above never sees a fragment. Owner threads: the receiver
    // appends, the sender applies the settle policy, closeSession flushes -
    // all worker threads, never the audio callback; the sink is never called
    // while transcriptMutex_ is held (no lock chain into pipeline/NDI).
    std::mutex transcriptMutex_;
    std::string transcriptLine_;
    long long lastTranscriptMs_ = 0;  ///< guarded by transcriptMutex_

    /// Finalize the open line if the pause policy says it is complete (header
    /// of transcriptSettleMs: a product decision, not a protocol fact).
    /// Cheap no-op when nothing is open or the pause has not elapsed.
    void settleTranscriptLine(long long nowMs);

    /// The open line as it currently reads - the whole-line snapshot the
    /// receiver emits after appending a fragment.
    std::string transcriptSnapshot();

    /// Take whatever line is open, unconditionally (session close). Returns
    /// empty when there is nothing to flush.
    std::string takeTranscriptLine();
};

} // namespace network
} // namespace liveai
