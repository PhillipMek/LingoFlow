// LingoFlow - operator application.
//
// Task 012: the credential read below uses getenv and the registry; suppress the
// deprecation the same way lingoflow_openai_probe does, and take the Windows
// headers before JUCE pulls its own (both must precede every standard header).
#ifdef LINGOFLOW_WITH_OPENAI_BACKEND
#define _CRT_SECURE_NO_WARNINGS 1
#include <windows.h>
#endif

// The UI talks to exactly one object (ApplicationController) and reads exactly
// one type (AppStatus). It has no knowledge of audio devices, wire protocols or
// subtitle transports (AGENTS.md 7).
//
// Command line:
//   --smoke   start the subsystems, log the resulting status, stop, quit without
//             creating a window. Used by the bootstrap verification and by CI; it
//             is not a substitute for an operator-visible check. --smoke stays on
//             the Null translation backend: a startup check must not open network
//             sockets or provider sessions (deterministic, cost-free, offline-
//             capable), the windowed run is what mounts the real chain.
//   --dev     developer mode (task 019) regardless of the settings file: the test
//             tone becomes the "device", the mock echo becomes the translator,
//             no OpenAI session is opened and no ASIO device is touched. It does
//             NOT rewrite the settings file, and it never enables loopback - a
//             microphone routed to its own room's speakers is a settings-level,
//             two-intentional-clicks decision, never a flag side effect. Combined
//             with --smoke it is the offline proof that the core runs without
//             hardware and without an API key.
//
// The windowed run (task 014) opens the operator screen: status chips, device /
// language / format selectors, live meters and gain, jitter pre-roll, counters
// and the subtitle model. Task 015 adds the Settings dialog: the translation
// instructions, recovery policy, NDI and diagnostics fields, and the masked
// API-key entry that is stored in the Windows Credential Manager (the value
// never touches settings, logs or the screen after a successful store). It
// starts the subsystems exactly like --smoke does; a start that fails exits
// with code 2 as documented below, and everything the operator changes
// afterwards goes through the controller - the UI itself owns no logic
// (App/UiModel is the tested half).
//
// Exit codes:
//   0   started (and, with --smoke, shut down) normally
//   2   a subsystem refused to start (e.g. the selected ASIO device is unavailable).
//       The reason is in the log and in the status detail.

#include <JuceHeader.h>

#include <filesystem>
#include <format>
#include <optional>
#include <string>

#include "App/ApplicationController.h"
#include "App/DeveloperMode.h"
#include "App/OperatorWindow.h"
#include "Audio/Dev/SimulatedDeviceBackend.h"
#include "Config/ConfigStore.h"
#include "Platform/Asio/AsioDiscovery.h"
#include "Platform/Asio/JuceAsioBackend.h"
#include "Translation/Mock/MockTranslationBackend.h"
#include "Utils/Log.h"

#ifdef LINGOFLOW_WITH_OPENAI_BACKEND
// Task 012: the production translation chain (the contract implementation of
// task 009 behind the recovery supervisor of task 010). Only App may include
// Network (docs/architecture.md); the core keeps speaking the contract.
#include <cstdlib>
#include <vector>

#include "Network/OpenAIRealtimeBackend.h"
#include "Security/ChainedSecretStore.h"
#include "Security/ISecretStore.h"
#include "Security/WindowsCredentialStore.h"
#include "Translation/ReconnectSupervisor.h"
#endif

#ifdef LINGOFLOW_WITH_NDI
// Task 016: the production subtitle transport. App may include NDI (the module
// boundary forbids SDK types only inside the INdiOutput contract, and this
// header keeps them on its .cpp side). NdiDispatch is core-portable (no SDK
// types) and moves every transport call off the thread that feeds the jitter
// buffer (code review P1, 2026-10-05).
#include "NDI/NdiDispatch.h"
#include "NDI/Real/NdiTimedTextOutput.h"
#endif

namespace {

constexpr std::string_view kLogComponent = "app";

/// Exit codes of the operator application. 0 always means "the application did what it
/// was asked"; a non-zero code is a failure an operator or CI can branch on without
/// reading the log.
constexpr int kExitStartupFailure = 2;

#ifdef LINGOFLOW_WITH_OPENAI_BACKEND
/// Reads OPENAI_API_KEY from the process environment first, then from the
/// HKCU\\Environment user variable - the same order and the same sources
/// lingoflow_openai_probe uses. Returns the value only to its caller in memory;
/// nothing here logs it, writes it, or keeps it past the session attempt.
std::optional<std::string> readEnvironmentApiKey()
{
    if (const char* env = std::getenv("OPENAI_API_KEY"); env != nullptr && env[0] != '\0')
        return std::string(env);

    wchar_t buffer[4096];
    DWORD size = sizeof(buffer);

    if (RegGetValueW(HKEY_CURRENT_USER, L"Environment", L"OPENAI_API_KEY", RRF_RT_REG_SZ,
                     nullptr, buffer, &size) == ERROR_SUCCESS)
    {
        const int len = WideCharToMultiByte(CP_UTF8, 0, buffer, -1, nullptr, 0, nullptr, nullptr);

        if (len > 1)
        {
            std::string out(static_cast<std::size_t>(len - 1), '\0');
            WideCharToMultiByte(CP_UTF8, 0, buffer, -1, out.data(), len, nullptr, nullptr);
            return out;
        }
    }

    return std::nullopt;
}

/// Development credential store (AGENTS.md 10 allows an environment variable for
/// development). The value is never logged and never written anywhere - the app
/// reports only its presence. Since task 015 this is the documented FALLBACK:
/// production keys live in the Windows Credential Manager and are read first;
/// the environment stays a convenience for development, not a storage story.
class EnvironmentSecretStore final : public liveai::security::ISecretStore
{
public:
    std::string_view name() const noexcept override { return "Development environment"; }

    liveai::security::SecretStatus store(std::string_view, std::string_view) override
    {
        return liveai::security::SecretStatus::unavailable; // read-only dev store
    }

    std::optional<std::string> load(std::string_view identifier) override
    {
        if (identifier != liveai::security::kOpenAiApiKey)
            return std::nullopt;

        return readEnvironmentApiKey();
    }

    liveai::security::SecretStatus remove(std::string_view) override
    {
        return liveai::security::SecretStatus::unavailable;
    }

    std::vector<std::string> identifiers() const override
    {
        // Identifiers only, and only when a value actually exists - never the value.
        return readEnvironmentApiKey().has_value()
                   ? std::vector<std::string>{ std::string(liveai::security::kOpenAiApiKey) }
                   : std::vector<std::string>{};
    }
};
#endif // LINGOFLOW_WITH_OPENAI_BACKEND

juce::File logDirectory()
{
    // Same folder name as the settings, so the rename to LingoFlow moved both at once.
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile(liveai::config::ConfigStore::applicationDirectoryName().data())
        .getChildFile("logs");
}

liveai::LogConfig makeLogConfig(const liveai::AppConfig& settings, bool writeConsole)
{
    liveai::LogConfig cfg;
    cfg.level = liveai::log::levelFromName(settings.diagnostics.logLevel);
    cfg.writeConsole = writeConsole;
    cfg.filePath = settings.diagnostics.writeLogFile
                       ? logDirectory().getChildFile("lingoflow.log").getFullPathName().toStdString()
                       : std::string();
    return cfg;
}

/// Header line plus the controller-formatted status. Main.cpp uses no subsystem
/// types at all - only AppStatus and describeStatus() - except for the two seams
/// it exists to install: the audio backend factory and the device lister (both
/// platform adapters, both handed to the controller as functions).
juce::String statusText(const liveai::AppStatus& status)
{
    return juce::String(std::format("{} {}\n{}",
                                    JUCE_APPLICATION_NAME_STRING,
                                    JUCE_APPLICATION_VERSION_STRING,
                                    liveai::describeStatus(status)));
}

} // namespace

//==============================================================================
class LingoFlowApplication final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return JUCE_APPLICATION_NAME_STRING; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override { return false; }

    void initialise(const juce::String& commandLineParameters) override
    {
        const bool smoke = commandLineParameters.containsIgnoreCase("--smoke");
        const bool dev = commandLineParameters.containsIgnoreCase("--dev");

        // A windowed GUI process has no usable console, so the file sink is the
        // source of truth for automated startup checks. Start from the operator
        // defaults so the settings load itself gets logged.
        liveai::log::configure(makeLogConfig(liveai::config::defaults(), /*writeConsole = */ false));

        // The build metadata belongs to the JUCE shell; the diagnostics export
        // header asks the controller for it (task 017) - the core does not
        // invent version numbers.
        controller_.setApplicationVersion(juce::String(JUCE_APPLICATION_VERSION_STRING).toStdString());

        liveai::log::info(kLogComponent,
                          std::format("application {} initialised (juce {} on {})",
                                      getApplicationVersion().toStdString(),
                                      juce::SystemStats::getJUCEVersion().toStdString(),
                                      juce::SystemStats::getOperatingSystemName().toStdString()));

        std::string settingsNote;

        // A settings file written before the rename lives in %APPDATA%\Live AI Interpreter.
        // Read it if that is all there is, and persist it once into the LingoFlow folder;
        // the old copy is never deleted (AGENTS.md 12: recover, do not destroy).
        bool fromLegacy = false;
        const auto startupFile = liveai::config::ConfigStore::startupFile(fromLegacy);
        const auto migrateTo = fromLegacy ? liveai::config::ConfigStore::defaultFile()
                                          : std::filesystem::path{};

        if (fromLegacy)
            liveai::log::info(kLogComponent, "settings: reading the pre-rename location "
                                                 + startupFile.string());

        if (!controller_.loadSettings(startupFile, settingsNote, migrateTo))
            liveai::log::error(kLogComponent, "settings could not be read: " + settingsNote);

        // Settings decide the log level and whether a log file is written. In smoke
        // mode the file stays on, because that file is how the startup is observed.
        auto logging = controller_.config().current();
        if (smoke)
            logging.diagnostics.writeLogFile = true;
        liveai::log::configure(makeLogConfig(logging, /*writeConsole = */ false));

        // The platform adapter is created here, in the JUCE layer: the core only knows
        // that a device name from settings has to become an IAudioBackend, not how an
        // ASIO device is opened (AGENTS.md 7). Without this factory a configured device
        // is reported as an error rather than silently ignored.
        controller_.setAudioBackendFactory(
            [](const liveai::audio::DeviceRequest& request, std::string& factoryError)
                -> std::unique_ptr<liveai::audio::IAudioBackend>
            {
                (void)factoryError;
                return std::make_unique<liveai::platform::JuceAsioBackend>(request.deviceId);
            });

        // Task 014: the operator picks devices in the UI, so the enumeration seam
        // gets its platform adapter here - the same "the core knows a name can
        // become a backend, not how" logic as the factory above. The scan reads
        // the ASIO registry only; it never loads a driver.
        controller_.setDeviceLister([] { return liveai::platform::scanAsioDevices().devices; });

        // Task 019: one plan, computed here from the loaded settings (plus the
        // --dev forcing), handed to the controller as the mounted authority and
        // executed by the mounts below. Production defaults - no forcing, and a
        // settings file whose developer section is absent or off - produce a
        // plan that mounts NOTHING, so every line below is inert in a real run
        // unless somebody asked for it with hands on a switch.
        const liveai::DeveloperPlan plan =
            liveai::developerPlan(controller_.config().current(), dev);
        controller_.setDeveloperPlan(plan);

        if (plan.enabled)
        {
            liveai::log::warning(kLogComponent, "DEVELOPER MODE: " + plan.badge);

            for (const auto& note : plan.notes)
                liveai::log::warning(kLogComponent, "developer plan note: " + note);
        }

        if (plan.useWavSource)
        {
            controller_.setAudioBackend(std::make_unique<liveai::audio::WavFileAudioBackend>(
                plan.wavInputPath, plan.wavOutputPath));
            liveai::log::info(kLogComponent,
                              "developer mode: WAV file '" + plan.wavInputPath
                                  + "' mounted as the audio source - no ASIO device will be opened");
        }
        else if (plan.useToneSource)
        {
            controller_.setAudioBackend(std::make_unique<liveai::audio::TestToneAudioBackend>(
                plan.toneFrequencyHz, plan.toneLevelDb, plan.wavOutputPath));
            liveai::log::info(kLogComponent, "developer mode: test tone mounted as the audio source "
                                             "- no ASIO device will be opened");
        }

#ifdef LINGOFLOW_WITH_OPENAI_BACKEND
        // The credential store serves the Settings dialog and the real backend
        // alike; a windowed run installs it even when the mock replaces the
        // backend, because entering a key is never a developer-mode privilege.
        // Smoke skips it: reading stores is not something a startup check needs.
        if (!smoke)
            installCredentialStores();
#endif

        if (plan.mockTranslation)
        {
            // Mounted even in builds without the network module: the mock needs
            // no provider and no key - that is precisely its purpose.
            liveai::translation::MockTranslationBackend::Options options;
            options.latencyMs = plan.mockLatencyMs;

            controller_.setTranslationBackend(
                std::make_unique<liveai::translation::MockTranslationBackend>(std::move(options)));

            liveai::log::warning(kLogComponent,
                                 "developer mode: mock echo translation mounted - no OpenAI backend "
                                 "will be constructed and no provider session will be opened");
        }
#ifdef LINGOFLOW_WITH_OPENAI_BACKEND
        else if (smoke)
            liveai::log::info(kLogComponent,
                              "smoke mode: the translation backend stays Null - a startup check must not "
                              "open network sockets or provider sessions");
        else
            installProductionTranslation();
#endif

#ifdef LINGOFLOW_WITH_NDI
        if (smoke)
            liveai::log::info(kLogComponent,
                              "smoke mode: the NDI output stays Null - a startup check must not touch "
                              "the LAN discovery stack either");
        else
            installProductionNdi();
#endif

        if (!controller_.start())
        {
            liveai::log::error(kLogComponent, "application failed to start: " + controller_.status().detail);

            // A start that failed must not look like a successful run: --smoke is used
            // as evidence in CI and by the operator, so it reports the failure in the
            // exit code as well as in the log.
            setApplicationReturnValue(kExitStartupFailure);

            juce::MessageManager::callAsync([] { juce::JUCEApplicationBase::quit(); });
            return;
        }

        if (smoke)
        {
            liveai::log::info(kLogComponent, "smoke mode status: " + statusText(controller_.status()).toStdString());
            liveai::log::info(kLogComponent, "smoke mode: shutting down without UI");
            juce::MessageManager::callAsync([] { juce::JUCEApplicationBase::quit(); });
            return;
        }

        window_ = std::make_unique<liveai::OperatorWindow>(controller_);
    }

    void shutdown() override
    {
        window_.reset();
        controller_.stop();
        liveai::log::info(kLogComponent, "application shut down");
    }

    void systemRequestedQuit() override { quit(); }
    void anotherInstanceStarted(const juce::String&) override {}

    void suspended() override {}
    void resumed() override {}

private:
#ifdef LINGOFLOW_WITH_OPENAI_BACKEND
    /// Mounts the credential store chain (task 015): the Windows Credential
    /// Manager is the primary store - a key the operator enters in Settings is
    /// written there by the OS, encrypted and restart-proof - and the
    /// environment variable keeps the exact role AGENTS.md 10 gave it: a
    /// development path. The chain reads the store first (a stored key wins)
    /// and writes only to the store (the environment is not the product's to
    /// rewrite). Idempotent: whoever needs the store calls this first.
    void installCredentialStores()
    {
        if (secretStore_ != nullptr)
            return;

        windowsStore_ = std::make_unique<liveai::security::WindowsCredentialStore>();
        devStore_ = std::make_unique<EnvironmentSecretStore>();
        secretStore_ = std::make_unique<liveai::security::ChainedSecretStore>(*windowsStore_,
                                                                              *devStore_);

        // The UI's credential actions and the backend's session-start read use
        // this same object: one store, two users, and not one copy of the value
        // anywhere else in the process.
        controller_.setSecretStore(*secretStore_);

        // Presence only, never the value (AGENTS.md 10). Without a key the session
        // open will refuse - the controller already treats that as a recorded,
        // recoverable failure that leaves the audio path running (AGENTS.md 12).
        if (secretStore_->identifiers().empty())
            liveai::log::warning(kLogComponent,
                                 "translation: no API key found - enter it in Settings (it will be "
                                 "stored in the Windows Credential Manager) or set OPENAI_API_KEY for "
                                 "development; sessions will refuse until one exists. "
                                 "Audio keeps running regardless.");
        else
            liveai::log::info(kLogComponent,
                              "translation: OpenAI credential found ("
                                  + std::string(secretStore_->name()) + ")");
    }

    /// Mounts the production translation chain (task 012): the real backend of
    /// task 009 inside the recovery supervisor of task 010, its policy built from
    /// the operator's settings. From here up the application sees only the task
    /// 007 contract - the OpenAI names stop at this composition root.
    void installProductionTranslation()
    {
        installCredentialStores();

        const auto& cfg = controller_.config().current().translation;

        liveai::network::OpenAIRealtimeOptions options; // documented defaults, no invented overrides

        auto backend = std::make_unique<liveai::network::OpenAIRealtimeBackend>(*secretStore_, options);

        liveai::translation::ReconnectSupervisor::Policy policy;
        policy.enabled = cfg.reconnectEnabled;
        policy.initialBackoffMs = cfg.reconnectInitialBackoffMs;
        policy.maxBackoffMs = cfg.reconnectMaxBackoffMs;
        policy.sessionMaxAgeMs = cfg.sessionMaxAgeSeconds > 0 ? cfg.sessionMaxAgeSeconds * 1000 : 0;
        policy.expirySafetyMarginMs = cfg.expirySafetyMarginSeconds > 0 ? cfg.expirySafetyMarginSeconds * 1000 : 0;

        controller_.setTranslationBackend(
            std::make_unique<liveai::translation::ReconnectSupervisor>(std::move(backend), policy));

        liveai::log::info(kLogComponent,
                          std::string("translation: OpenAI backend mounted behind the reconnect supervisor (recovery ")
                              + (cfg.reconnectEnabled ? "on" : "pass-through") + ")");
    }

    /// Declared before the controller so they outlive the backend holding a
    /// reference to the chain. Order matters twice: the chain refers to both
    /// stores (declared first, destroyed last after it), and all three must
    /// outlive the controller's backends.
    std::unique_ptr<liveai::security::WindowsCredentialStore> windowsStore_;
    std::unique_ptr<EnvironmentSecretStore> devStore_;
    std::unique_ptr<liveai::security::ISecretStore> secretStore_;
#endif

#ifdef LINGOFLOW_WITH_NDI
    /// Mounts the production subtitle transport (task 016, SPEC 38 Mode A):
    /// caption snapshots as TTML1 metadata over NDI, through the runtime the
    /// machine provides - loaded dynamically per docs/licensing.md, the SDK's
    /// import library never linked. When the runtime is absent the controller's
    /// honest NDI-failure path takes over: the log and the operator chip say
    /// what is wrong, and audio and translation keep running (AGENTS.md 12).
    /// The transport itself is mounted behind NdiDispatch (code review P1):
    /// every SDK call runs on the dispatch worker, never on the OpenAI receiver
    /// thread that feeds the jitter buffer.
    void installProductionNdi()
    {
        controller_.setNdiOutput(std::make_unique<liveai::ndi::NdiDispatch>(
            std::make_unique<liveai::ndi::real::NdiTimedTextOutput>()));
        liveai::log::info(kLogComponent,
                          "NDI: timed-text output mounted behind the dispatch worker"
                          " (runtime-loaded, never linked; SDK off the receiver thread)");
    }
#endif

    liveai::ApplicationController controller_;
    std::unique_ptr<liveai::OperatorWindow> window_;
};

START_JUCE_APPLICATION(LingoFlowApplication)
