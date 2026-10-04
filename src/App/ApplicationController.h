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
#include "Audio/Asio/AsioDeviceInfo.h"
#include "Audio/AudioEngine.h"
#include "Config/ConfigManager.h"
#include "Diagnostics/DiagnosticsManager.h"
#include "NDI/INdiOutput.h"
#include "Security/ISecretStore.h"
#include "Translation/ITranslationBackend.h"
#include "Translation/TextPipeline.h"

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

    /// Device enumeration seam (task 014). The list of selectable devices is
    /// platform knowledge, exactly like opening one, so it arrives as a function
    /// installed by the composition root; the controller only caches its result
    /// for the UI to read. A UI refresh calls refreshDevices(); nothing in here
    /// opens or touches a device.
    using DeviceLister = std::function<std::vector<asio::DeviceEntry>()>;
    void setDeviceLister(DeviceLister lister);
    /// The cached enumeration result: empty until the first refreshDevices(), and
    /// after it the truth as of that scan (the UI labels it with a refresh button).
    const std::vector<asio::DeviceEntry>& devices() const noexcept { return devices_; }

    /// Runs the lister and replaces the cache. Returns the entry count; without
    /// an installed lister it logs the fact and returns 0 - an unavailable
    /// enumeration is said out loud, never presented as "no devices exist".
    int refreshDevices();

    /// Retry path for the operator (task 014): a fault that has been corrected in
    /// settings can be started again. Moves faulted back to stopped - the log
    /// records the clear, the counters keep the history, and starting is the
    /// operator's decision, not this method's. A no-op (logged) in any other
    /// state.
    void clearFault() noexcept;

    // ------------------------------------------------------------- UI commands (014)
    /// Thin live controls: slider-drag values into the engine, no config write
    /// (the release event persists through updateSettings). Mutes are runtime
    /// state per task 006's SPEC and never persisted; gains converge with the
    /// settings file at the next updateSettings.
    void setGainsLive(float inputDb, float outputDb) noexcept;
    void setMutesLive(bool inputMuted, bool outputMuted) noexcept;
    void setJitterLive(int jitterMs) noexcept;

    /// Validates the candidate, and only when it passes: stores it, applies what
    /// can be applied without a restart (gains, jitter pre-roll, log level) and
    /// persists it. `note` says what happened, including which changes take
    /// effect on the next Start (device, sample rate, buffer, channels,
    /// languages, NDI) or on the next application launch (log file on/off - the
    /// sinks are the composition root's startup act, recovery policy - mounted
    /// by Main.cpp at startup). An invalid candidate changes nothing in memory
    /// and nothing on disk - the 003 contract, not re-decided here.
    bool updateSettings(const AppConfig& candidate, std::string& note);

    // ---------------------------------------------------------- credentials (015)
    /// The credential store seam (task 015), same pattern as the device lister:
    /// the composition root installs the production store (Windows secure
    /// storage with the development-environment fallback), and this controller
    /// exposes the operator's actions on it. The secret value travels only
    /// UI-field -> store: it never enters settings, notes, statuses or logs
    /// (AGENTS.md 10, the task's FAIL criterion). Without an installed store the
    /// Null store answers honestly: nothing is ever "stored" here.
    void setSecretStore(security::ISecretStore& store) noexcept;

    /// True when an API-key identifier exists in the store. Presence only -
    /// names are read, values never are (identifiers(), not load()).
    bool hasApiSecret() const;

    /// Store label for the UI ("Windows Credential Manager / fallback: ...").
    std::string secretStoreName() const;

    /// Operator actions. `note` reports the outcome in operator words and is
    /// guaranteed to contain no trace of the secret itself.
    bool storeApiSecret(std::string_view secret, std::string& note);
    void removeApiSecret(std::string& note);

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

    /// The device side as the read-only contract, for task 018's accounting only
    /// (driver-reported latencies live in capabilities()). Null before any
    /// backend exists - the accounting handles the null honestly. This is a
    /// display read, never an operation: the UI may not open, start or stop
    /// anything through it.
    const audio::IAudioBackend* audioBackend() const noexcept { return audioBackend_.get(); }

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

    /// The version string printed into the diagnostics export header. Main.cpp
    /// (the JUCE shell that owns the build metadata) sets it; the honest default
    /// is "unknown" - the core does not invent build numbers (AGENTS.md 19).
    void setApplicationVersion(std::string version);

    /// Default export location: <settings folder>/diagnostics.
    std::filesystem::path diagnosticsDirectory() const;

    // ------------------------------------------------------------- diagnostics (017)
    /// Assembles the structured report ("what is this system doing" - counters,
    /// live geometry, meter views, session/NDI/security states, the settings
    /// (non-secret by 003's construction) and the event ring) and writes it
    /// atomically as a timestamped file in `directory` (empty = the default
    /// location). Assembled and written on the calling thread (UI/export), never
    /// from audio.
    ///
    /// Secrets: the report carries credential PRESENCE and the store's name, not
    /// values; as defense in depth the renderer re-checks every key against the
    /// Config module's own "looks like a secret" predicate, redacts matches and
    /// counts them into the note. Returns false with the reason - no silent
    /// second path. The export event itself is recorded after the file is
    /// written: the next export will contain it, this one cannot contain its own
    /// completion.
    bool exportDiagnostics(const std::filesystem::path& directory,
                           std::filesystem::path& written,
                           std::string& note);

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

    /// Text events cross the seam as the contract's partial/final string pair and
    /// become typed the moment they arrive: the pipeline stamps sequence and
    /// arrival time, owns the bounded history the UI reads, and its listener is
    /// what publishes subtitles to NDI. The sink methods here do nothing else -
    /// text never touches the audio path and audio never waits for text.
    void onPartialText(std::string_view text) override;
    void onFinalText(std::string_view text) override;
    void onSessionStateChanged(translation::SessionState state) override;

    /// Records the failure and keeps everything else running: a translation
    /// error must never stop the audio path (AGENTS.md 12), and deciding to
    /// reconnect or reopen is task 010's, not this callback's.
    void onTranslationError(const translation::TranslationError& error) override;

    /// The UI-facing text model (task 013): bounded history, the open line, and
    /// the counters that say what the pipeline ignored and why. Read from any
    /// thread.
    const translation::TextPipeline& textPipeline() const noexcept { return textPipeline_; }

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
    void publishToNdi(const translation::TranslationTextEvent& event);
    void fault(std::string reason);

    DiagnosticsManager diagnostics_;
    ConfigManager config_;
    AudioEngine engine_{ &diagnostics_ };

    std::unique_ptr<audio::IAudioBackend> audioBackend_;

    /// The typed text model of task 013: fed by the sink, read by the UI,
    /// listened to by the NDI publisher. Application-level like the counters -
    /// a session restart does not erase the operator's history.
    ///
    /// Declaration order is load-bearing, not stylistic: destruction runs in
    /// reverse, and ~OpenAIRealtimeBackend() closes its session - which now
    /// flushes an open subtitle line through this sink (rule 5 allows it
    /// before the close returns). The pipeline and the NDI output therefore
    /// must outlive the backend that writes through them.
    translation::TextPipeline textPipeline_;
    std::unique_ptr<ndi::INdiOutput> ndiOutput_;
    std::unique_ptr<translation::ITranslationBackend> translationBackend_;

    /// Lives exactly as long as an open session (startStreaming/stopStreaming);
    /// it holds pointers into the engine's pipeline, so it must be gone before
    /// the engine deactivates - stop() runs session teardown before audio teardown.
    std::unique_ptr<TranslationStreamer> streamer_;

    AudioBackendFactory audioBackendFactory_;
    /// Device enumeration seam (014): installed by the composition root, cached
    /// result read by the UI through devices().
    DeviceLister deviceLister_;
    std::vector<asio::DeviceEntry> devices_;

    /// Credentials seam (task 015): the default Null store makes "no
    /// credentials" an explicit, testable state instead of a missing feature;
    /// the composition root installs the production chain. The store outlives
    /// the controller by declaration order in Main.cpp.
    security::NullSecretStore nullSecrets_;
    security::ISecretStore* secrets_ = &nullSecrets_;
    /// True once setAudioBackend() was called. Distinguishes "the tests or developer
    /// mode chose this backend" from "this is the default null device", so a device
    /// configured in settings is never quietly served by the null backend.
    bool audioBackendOverridden_ = false;

    ApplicationState state_ = ApplicationState::stopped;
    std::string faultReason_;
    std::string lastAudioError_;
    /// Build metadata for the export header; only the composition root knows it.
    std::string appVersion_ = "unknown";
    // (ndiSequence_ retired by task 013: subtitle frames carry the typed event's
    // own pipeline sequence now - one monotonic identity for text, not a
    // per-publisher counter.)

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
