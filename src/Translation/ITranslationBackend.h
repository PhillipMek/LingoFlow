#pragma once
//
// Translation backend contract (skeleton of the task 007 contract).
//
// Boundary rules (AGENTS.md 7, 8):
//   * This header is protocol-neutral: no event names, session fields, model
//     identifiers or wire formats. OpenAI specifics live only inside
//     Network/OpenAIRealtime* backends (task 009).
//   * Callbacks arrive on a network/worker thread, never on the audio thread.
//   * Audio handed to the backend is copied by the backend: the caller's buffer
//     is only valid for the duration of the call.
//   * A backend failure must never close the audio device (AGENTS.md 12).

#include <string>
#include <string_view>

namespace liveai {
namespace translation {

enum class SessionState
{
    closed = 0,     ///< no session, nothing is streamed
    connecting,     ///< session being established
    connected,      ///< streaming normally
    reconnecting,   ///< lost connection, recovery in progress (task 010)
    faulted         ///< unrecoverable for this session; caller may retry
};

std::string_view nameOf(SessionState state) noexcept;

/// Language pair as opaque tags. Vocabulary and validation belong to
/// LanguageRegistry (task 011); the contract only carries what the caller asked
/// for.
struct LanguagePair
{
    std::string input;
    std::string output;
};

/// Session request. `instructions` is the interpreter instruction text
/// (SPEC "Translation instructions"); model/wire identifiers are deliberately
/// not part of this skeleton - task 008/009 introduce them from official docs.
struct SessionRequest
{
    LanguagePair pair;
    std::string instructions;
};

/// Implemented by whoever consumes translation output (the application
/// controller wires it to the audio engine, text pipeline and NDI).
class ITranslationSink
{
public:
    virtual ~ITranslationSink() = default;

    /// Translated mono float32 audio at the session sample rate. Called from a
    /// worker/network thread; the implementation must enqueue it, not play it.
    /// `samples` is only valid during the call.
    virtual void onTranslatedAudio(const float* samples, int frameCount, int sampleRate) = 0;

    /// In-progress transcript/translation; may arrive many times per utterance.
    virtual void onPartialText(std::string_view text) = 0;

    /// Completed text; final events are authoritative for history and NDI.
    virtual void onFinalText(std::string_view text) = 0;

    /// Session lifecycle changes (UI status, diagnostics counters).
    virtual void onSessionStateChanged(SessionState state) = 0;
};

/// Control side of the backend. All methods are called from non-realtime threads.
class ITranslationBackend
{
public:
    virtual ~ITranslationBackend() = default;

    virtual std::string_view name() const noexcept = 0;
    virtual SessionState state() const noexcept = 0;

    /// Attaches the sink. Must be called before openSession().
    virtual void setSink(ITranslationSink& sink) noexcept = 0;

    /// Establishes a translation session. Credentials are resolved by the
    /// backend from the credential store (AGENTS.md 10) - they are never passed
    /// through this interface as a plain string in logs or config.
    virtual bool openSession(const SessionRequest& request, std::string& error) = 0;

    /// Queues mono float32 audio for translation. Returns false when the session
    /// cannot accept data; the caller keeps operating (silence, not a crash).
    virtual bool submitAudio(const float* samples, int frameCount, std::string& error) = 0;

    /// Ends the session and releases resources. Idempotent.
    virtual void closeSession() noexcept = 0;
};

} // namespace translation
} // namespace liveai
