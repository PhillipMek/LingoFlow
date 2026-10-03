#pragma once
//
// Translation backend contract (task 007). This header is the whole seam between
// the product and any translation provider: lifecycle, translated audio output,
// text events, errors and session state.
//
// Boundary rules (AGENTS.md 7, 8):
//   * This header is protocol-neutral: no event names, session fields, model
//     identifiers or wire formats. OpenAI specifics live only inside the
//     Network/OpenAIRealtime* backend (task 009) and are mapped onto the types
//     below. A provider rejection becomes TranslationErrorCategory, not a copy
//     of the provider's error vocabulary.
//   * AudioEngine never includes this header. Translated audio reaches the
//     engine through the sink (ApplicationController routes it), so the audio
//     path stays free of provider-specific protocol - the FAIL criterion of
//     this task.
//   * Callbacks arrive on a network/worker thread, never on the audio thread.
//   * Audio handed to the backend is copied by the backend: the caller's buffer
//     is only valid for the duration of the call.
//   * A backend failure must never close the audio device (AGENTS.md 12). The
//     contract makes that possible: errors and state are events to the sink,
//     and nothing in this interface can reach a device.
//
// Deliberately NOT here yet:
//   * getCapabilities() (SPEC "Translation Provider Interface") - task 011
//     creates TranslationCapabilities/LanguageRegistry; a half-defined
//     capability type here would be re-invented by that task. Backend-specific
//     pair validation in the meantime is minimal and honest: reject what you
//     cannot do, with a message.
//
// What task 013 added around this seam (typed text, bounded history):
//   Translation/TextPipeline.h defines TranslationTextEvent (sequence, arrival
//   stamp, partial/final) and the pipeline that owns them. The sink itself
//   still carries the string pair below - deliberately: identities belong to
//   the product side, and a backend that had to stamp sequences would know
//   more about the application than a seam should. What 013 fixed is the
//   MEANING of those strings (see onPartialText): whole-line snapshots, never
//   provider fragments - assembling a provider's pieces into a line is the
//   provider's backend's job, upstream of this interface.

#include <string>
#include <string_view>

namespace liveai {
namespace translation {

enum class SessionState
{
    closed = 0,     ///< no session, nothing is streamed
    connecting,     ///< session being established
    connected,      ///< streaming normally
    reconnecting,   ///< lost connection, recovery in progress (task 010); submitAudio
                    ///< may refuse while in this state and must not be treated as fatal
    faulted         ///< this session is over; the application keeps running and a new
                    ///< openSession() is the way back
};

std::string_view nameOf(SessionState state) noexcept;

/// Language pair as opaque tags. Vocabulary and validation belong to
/// LanguageRegistry (task 011); the contract only carries what the caller asked
/// for.
struct LanguagePair
{
    std::string input;
    std::string output;

    friend constexpr bool operator==(const LanguagePair&, const LanguagePair&) = default;
};

/// Session request: everything a backend needs to start translating, and nothing
/// it does not. The rates travel here because the contract cannot otherwise
/// know at what rate submitAudio() hands frames, nor at what rate the
/// application wants delivered audio.
struct SessionRequest
{
    LanguagePair pair;
    std::string instructions;

    /// Opaque provider model identifier, empty = "backend default". The set of
    /// legal values comes from the official documentation research (task 008)
    /// and the capability manifest (task 011); nothing here invents one.
    std::string model;

    /// Sample rate of the mono float32 frames submitAudio() will carry, Hz.
    /// Zero means the caller does not know - a backend that needs to know
    /// refuses the session with an error instead of guessing (AGENTS.md 8).
    int inputSampleRate = 0;

    /// Sample rate the application will play delivered audio at, Hz. Blocks
    /// arriving at any other rate are the receiver's to reject and count -
    /// resampling is a backend's job on its own threads (task 009), never
    /// something the receiver does silently, and playing audio at the wrong
    /// speed is not an option.
    int outputSampleRate = 0;

    /// Recovery tests assert that the replayed request is identical to the
    /// stored one; equality of the whole value is also what makes "replay the
    /// stored SessionRequest" (protocol doc section 10) checkable.
    friend constexpr bool operator==(const SessionRequest&, const SessionRequest&) = default;
};

/// What can go wrong, in vocabulary that survives changing providers. Task 009
/// maps protocol-specific failures onto these categories; nothing in the
/// application learns the provider's own error names.
enum class TranslationErrorCategory
{
    connection = 0,   ///< transport-level failure: dropped, refused, timed out
    rejectedRequest,  ///< the session request itself was refused: pair, instructions,
                      ///< model or rates the backend will not accept
    audioFormat,      ///< audio cannot be used: bad rate or format on either side
    protocol,         ///< event stream broke the agreed shape (task 010 decides what
                      ///< is recoverable)
    internal          ///< anything else; the message is the whole truth we have
};

std::string_view nameOf(TranslationErrorCategory category) noexcept;

struct TranslationError
{
    TranslationErrorCategory category = TranslationErrorCategory::internal;

    /// Human-readable, for the log and the operator's diagnostics. Never
    /// contains credentials (AGENTS.md 10); provider payload worth keeping
    /// belongs in the diagnostics dump (task 017), not in this string.
    std::string message;

    /// true  = this session cannot continue. The backend also reports the state
    ///         change (normally to faulted). The application does not stop
    ///         anything - it may open a new session (task 010 owns when).
    /// false = an event, not a death sentence: the backend keeps the session or
    ///         is retrying (task 010). The sink records and moves on.
    bool fatal = false;

    /// Recovery hint (task 010): when non-zero the backend learned from the
    /// service that retrying sooner than this is pointless ("wait at least as
    /// long as Retry-After specifies", protocol doc section 9). A wait at
    /// least this long is the minimum the recovery policy must honour; 0 means
    /// "no hint, use the policy's own backoff".
    int retryAfterMs = 0;
};

/// Implemented by whoever consumes translation output. The application
/// controller is the only production implementation: it routes audio to the
/// engine's jitter buffer, text to NDI and diagnostics, state and errors to the
/// log and counters.
///
/// Thread and independence rules:
///   * All methods can be called from any backend thread at any time, one at a
///     time per backend instance is NOT guaranteed - implementations must
///     tolerate interleaving.
///   * Audio and text are independent channels (SPEC "Translation Provider
///     Interface"): either can arrive without the other, at any pace, and the
///     sink must not couple them. A sink that drops text must not drop audio
///     because of it, and vice versa.
///   * onTranslatedAudio runs on a worker thread: it must enqueue and return.
///     The audio callback is not involved anywhere in this interface.
class ITranslationSink
{
public:
    virtual ~ITranslationSink() = default;

    /// Translated mono float32 audio at `sampleRate` Hz. `samples` is only valid
    /// for the duration of the call. A rate the receiver does not expect is the
    /// receiver's to reject and count, never to resample silently or play at the
    /// wrong speed.
    virtual void onTranslatedAudio(const float* samples, int frameCount, int sampleRate) = 0;

    /// The translated line as it currently reads: a whole-line snapshot, not a
    /// fragment - the backend assembles whatever its provider streams before
    /// it crosses this seam (task 013). Every call replaces the previous one
    /// for the same line; may arrive many times per utterance.
    virtual void onPartialText(std::string_view text) = 0;

    /// Completed text; final events are authoritative for history and NDI. An
    /// empty final closes the open line with its own last-known words (the
    /// TextPipeline rule task 013 defined); a final with words replaces the
    /// draft as the authoritative text of that line.
    virtual void onFinalText(std::string_view text) = 0;

    /// Session lifecycle changes (UI status, diagnostics counters). A backend
    /// reports every real transition exactly once (see ITranslationBackend).
    virtual void onSessionStateChanged(SessionState state) = 0;

    /// Something went wrong. The sink records it; deciding whether and when to
    /// retry or reopen is task 010, and nothing here may stop the audio device.
    virtual void onTranslationError(const TranslationError& error) = 0;
};

/// Control side of the backend. All methods are called from non-realtime
/// threads. Lifecycle rules that implementations MUST honour, because the
/// integration tests and the reconnect logic of task 010 build on them:
///
///   1. setSink() before openSession(); without a sink, openSession() refuses
///      with an error rather than producing an unusable session.
///   2. openSession() on a session that is not closed refuses with an error and
///      changes nothing. Callers close first, deliberately.
///   3. A refused openSession() may leave the state closed or move it to
///      faulted (a rejected pair, for instance). Whatever it does, every real
///      state change is reported through onSessionStateChanged exactly once,
///      and a refusal that changed nothing reports nothing.
///   4. submitAudio() succeeds only while connected. Outside that it returns
///      false and sets `error`; the caller keeps operating - refused audio is
///      not a fault (AGENTS.md 12).
///   5. closeSession() is idempotent and guarantees that no sink callback is
///      invoked after it returns. In-flight worker callbacks are joined or
///      dropped inside closeSession(), not left to race with the caller's
///      teardown. This is what lets ApplicationController stop the session
///      before the audio device.
///   6. After closeSession(), openSession() may be called again; the new
///      session is unrelated to the old one. A backend resets per-session
///      state so a reopened session behaves like a fresh one.
class ITranslationBackend
{
public:
    virtual ~ITranslationBackend() = default;

    virtual std::string_view name() const noexcept = 0;

    /// Readable from any thread; a relaxed snapshot, not a lock.
    virtual SessionState state() const noexcept = 0;

    /// Attaches the sink. Must be called before openSession() (rule 1).
    virtual void setSink(ITranslationSink& sink) noexcept = 0;

    /// Establishes a translation session. Credentials are resolved by the
    /// backend from the credential store (AGENTS.md 10) - they are never passed
    /// through this interface as a plain string in logs or config.
    virtual bool openSession(const SessionRequest& request, std::string& error) = 0;

    /// Queues mono float32 audio at the session's inputSampleRate for
    /// translation. Non-blocking: it hands data to the backend's own buffering
    /// and returns. Returns false when the session cannot accept data; the
    /// caller keeps operating (silence, not a crash).
    virtual bool submitAudio(const float* samples, int frameCount, std::string& error) = 0;

    /// Ends the session and releases resources. Idempotent; no sink callbacks
    /// after it returns (rule 5).
    virtual void closeSession() noexcept = 0;
};

} // namespace translation
} // namespace liveai
