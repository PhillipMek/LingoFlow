#pragma once
//
// ApplicationController - composition root. It owns the subsystem shells and is
// the only place that wires them together (docs/architecture.md).
//
// Dependency direction (AGENTS.md 7):
//   UI -> ApplicationController -> { AudioEngine, ITranslationBackend, INdiOutput,
//                                    ConfigManager, DiagnosticsManager }
//   AudioEngine -> IAudioBackend
// Nothing above the controller knows a wire protocol, and nothing below it knows
// the UI.
//
// The audio path is real since task 005 (device + engine + gain), the translation
// path is a contract since task 007, implemented against the real service in
// task 009 and wrapped by the recovery supervisor in task 010. Task 012 mounted
// the production chain here: the composition root (Main.cpp) injects
// ReconnectSupervisor(OpenAIRealtimeBackend), and between a session opening and
// closing this controller runs the TranslationStreamer worker that feeds the
// backend from the engine's input ring. Which backend object arrives is the
// composition root's choice: the controller only ever sees the contract, so
// tests and developer mode (task 019) swap it without touching this file.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include "App/TranslationStreamer.h"
#include "Audio/AudioEngine.h"
#include "Config/ConfigManager.h"
#include "Diagnostics/DiagnosticsManager.h"
#include "NDI/INdiOutput.h"
#include "Translation/ITranslationBackend.h"

namespace liveai {

enum class ApplicationState
{
    stopped,     ///< nothing is running
    starting,    ///< subsystems are being brought up
    running,     ///< normal operation
    stopping,    ///< subsystems are being shut down
    faulted      ///< a subsystem failed; the reason is recorded
};

std::string_view nameOf(ApplicationState state) noexcept;

/// Everything the UI may display about the system, as opaque state values.
/// Adding a field here is the only way subsystem state reaches the UI - the UI
/// never includes backend headers (PASS criteria of task 002).
struct AppStatus
{
    ApplicationState application = ApplicationState::stopped;
    audio::BackendState audio = audio::BackendState::closed;
    translation::SessionState session = translation::SessionState::closed;
    ndi::OutputState ndi = ndi::OutputState::disabled;
    std::string audioBackendName;
    std::string detail;
};

/// One line per subsystem, for the UI label and the smoke log. Formatting lives in
/// the App module so Main.cpp only needs App/ApplicationController.h: the UI stays
/// free of subsystem types and of any wire protocol.
std::string describeStatus(const AppStatus& status);

class ApplicationController : public translation::ITranslationSink
{
public:
    ApplicationController();

    // ------------------------------------------------------------- dependencies
    /// Injected for tests and Developer/Mock mode (task 019). Ownership moves to
    /// the controller; must be called while stopped.
    void setAudioBackend(std::unique_ptr<audio::IAudioBackend> backend);
    void setTranslationBackend(std::unique_ptr<translation::ITranslationBackend> backend);
    void setNdiOutput(std::unique_ptr<ndi::INdiOutput> output);

    /// Builds the backend for a device the operator selected in settings. Installed
    /// by the composition root (Main.cpp) because the platform adapter is the only
    /// JUCE user: the core must not know how an ASIO device is opened, only that a
    /// device name can become an IAudioBackend. Must be called while stopped.
    using AudioBackendFactory =
        std::function<std::unique_ptr<audio::IAudioBackend>(const audio::DeviceRequest&, std::string& error)>;

    void setAudioBackendFactory(AudioBackendFactory factory);

    // ------------------------------------------------------------------ lifecycle
    /// Brings audio, translation session and NDI up. Returns false and enters the
    /// faulted state when a subsystem refuses. A translation/NDI failure does not
    /// abort the audio path (AGENTS.md 12) - the call still succeeds with the
    /// failure recorded.
    bool start();

    /// Stops in reverse order. Always safe to call.
    void stop();

    ApplicationState state() const noexcept { return state_; }
    bool isRunning() const noexcept { return state_ == ApplicationState::running; }
    std::string_view faultReason() const noexcept { return faultReason_; }

    AppStatus status() const;

    // -------------------------------------------------------------------- access
    AudioEngine& engine() noexcept { return engine_; }
    const AudioEngine& engine() const noexcept { return engine_; }
    ConfigManager& config() noexcept { return config_; }
    const ConfigManager& config() const noexcept { return config_; }
    DiagnosticsManager& diagnostics() noexcept { return diagnostics_; }
    const DiagnosticsManager& diagnostics() const noexcept { return diagnostics_; }
    translation::SessionState sessionState() const noexcept;

    /// The capture-side streaming worker, non-null while a translation session
    /// is open (task 012). nullptr before the session opens and after it closes.
    const TranslationStreamer* translationStreamer() const noexcept { return streamer_.get(); }

    // ------------------------------------------------------------------- settings
    /// Points the settings area at `file` and reads it. Returns false only when the
    /// store could not be read at all; `note` always says what happened (defaults,
    /// per-field repair, backup restore, refused future schema version). The result
    /// is always a usable configuration.
    ///
    /// When `persistTo` is set and differs from `file`, the read settings are written
    /// there once and the store is re-pointed at it, so every later save goes to the
    /// new location. This is how a pre-rename installation ("Live AI Interpreter")
    /// migrates into the LingoFlow folder instead of being read from the old path
    /// forever. A failure to persist is logged, not reported as a load failure: the
    /// settings in memory are valid either way.
    bool loadSettings(const std::filesystem::path& file, std::string& note,
                      const std::filesystem::path& persistTo = {});

    /// Writes the current settings atomically through the attached store.
    bool saveSettings(std::string& error);

    const config::LoadResult& settingsLoad() const noexcept { return config_.lastLoad(); }

    // -------------------------------------------------- translation::ITranslationSink
    // Called on network/worker threads, never on the audio thread.
    //
    // onTranslatedAudio is the delivery point of the task 007 contract: blocks
    // that pass the checks are written straight into the engine's output jitter
    // buffer - the one and only source of played audio (task 005's safety rule).
    // Everything refused is counted and warned about once, never swallowed
    // silently, and never retried at the wrong speed: there is no resampler in
    // this product yet, and playing 16 kHz audio at 48 kHz would be a lie with
    // chipmunk voice.
    void onTranslatedAudio(const float* samples, int frameCount, int sampleRate) override;
    void onPartialText(std::string_view text) override;
    void onFinalText(std::string_view text) override;
    void onSessionStateChanged(translation::SessionState state) override;

    /// Records the failure and keeps everything else running: a translation
    /// error must never stop the audio path (AGENTS.md 12), and deciding to
    /// reconnect or reopen is task 010's, not this callback's.
    void onTranslationError(const translation::TranslationError& error) override;

private:
    bool startAudio(std::string& error);
    void stopAudio() noexcept;
    bool startSession(std::string& error);
    void stopSession() noexcept;
    /// Streams the engine's capture into the session. Refusal to start is not a
    /// session failure: the log and the counters say the translator is not being
    /// fed, and everything else keeps running.
    void startStreaming();
    void stopStreaming() noexcept;
    /// Returns true when NDI is disabled in settings (nothing to start) or when
    /// the output started; false only on a real start failure.
    bool startNdi(std::string& error);
    void stopNdi() noexcept;
    void publishToNdi(std::string_view text, bool isFinal);
    void fault(std::string reason);

    DiagnosticsManager diagnostics_;
    ConfigManager config_;
    AudioEngine engine_{ &diagnostics_ };

    std::unique_ptr<audio::IAudioBackend> audioBackend_;
    std::unique_ptr<translation::ITranslationBackend> translationBackend_;
    std::unique_ptr<ndi::INdiOutput> ndiOutput_;

    /// Lives exactly as long as an open session (startStreaming/stopStreaming);
    /// it holds pointers into the engine's pipeline, so it must be gone before
    /// the engine deactivates - stop() runs session teardown before audio teardown.
    std::unique_ptr<TranslationStreamer> streamer_;

    AudioBackendFactory audioBackendFactory_;
    /// True once setAudioBackend() was called. Distinguishes "the tests or developer
    /// mode chose this backend" from "this is the default null device", so a device
    /// configured in settings is never quietly served by the null backend.
    bool audioBackendOverridden_ = false;

    ApplicationState state_ = ApplicationState::stopped;
    std::string faultReason_;
    std::string lastAudioError_;
    std::atomic<std::uint64_t> ndiSequence_{ 0 };

    /// A wrong-rate delivery repeats per block; the log says it once and the
    /// counters carry the frames. A translation error is never silenced at all.
    std::atomic<bool> warnedRateMismatch_{ false };
    std::atomic<bool> warnedNoBuffer_{ false };
    std::atomic<bool> warnedBadBlock_{ false };
    std::string lastTranslationError_;  ///< guarded by subsystemMutex_
    mutable std::mutex subsystemMutex_; ///< protects lastTranslationError_ only; mutable
                                        ///< because status() is a const read of it

    /// Records a translation error for AppStatus.detail and logs it: the first
    /// occurrence of a message is a warning, repeats are debug-level, because a
    /// backend in a bad loop must not fill the log.
    void noteTranslationError(const translation::TranslationError& error);
};

} // namespace liveai
