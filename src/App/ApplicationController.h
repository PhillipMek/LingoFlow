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
// At this stage every subsystem is a Null implementation: no device is opened and
// no network is used (tasks 004/005/009 replace them).

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

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
    void onTranslatedAudio(const float* samples, int frameCount, int sampleRate) override;
    void onPartialText(std::string_view text) override;
    void onFinalText(std::string_view text) override;
    void onSessionStateChanged(translation::SessionState state) override;

private:
    bool startAudio(std::string& error);
    void stopAudio() noexcept;
    bool startSession(std::string& error);
    void stopSession() noexcept;
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

    ApplicationState state_ = ApplicationState::stopped;
    std::string faultReason_;
    std::string lastAudioError_;
    std::atomic<std::uint64_t> ndiSequence_{ 0 };
};

} // namespace liveai
