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
//
// The windowed run (task 014) opens the operator screen: status chips, device /
// language / format selectors, live meters and gain, jitter pre-roll, counters
// and the subtitle model. It starts the subsystems exactly like --smoke does; a
// start that fails exits with code 2 as documented below, and everything the
// operator changes afterwards goes through the controller - the UI itself owns
// no logic (App/UiModel is the tested half).
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
#include "App/OperatorWindow.h"
#include "Config/ConfigStore.h"
#include "Platform/Asio/AsioDiscovery.h"
#include "Platform/Asio/JuceAsioBackend.h"
#include "Utils/Log.h"

#ifdef LINGOFLOW_WITH_OPENAI_BACKEND
// Task 012: the production translation chain (the contract implementation of
// task 009 behind the recovery supervisor of task 010). Only App may include
// Network (docs/architecture.md); the core keeps speaking the contract.
#include <cstdlib>
#include <vector>

#include "Network/OpenAIRealtimeBackend.h"
#include "Security/ISecretStore.h"
#include "Translation/ReconnectSupervisor.h"
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
/// reports only its presence. Task 015 replaces this with Windows secure storage;
/// until then this is the documented development path, not a production store.
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

        // A windowed GUI process has no usable console, so the file sink is the
        // source of truth for automated startup checks. Start from the operator
        // defaults so the settings load itself gets logged.
        liveai::log::configure(makeLogConfig(liveai::config::defaults(), /*writeConsole = */ false));

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

#ifdef LINGOFLOW_WITH_OPENAI_BACKEND
        if (smoke)
            liveai::log::info(kLogComponent,
                              "smoke mode: the translation backend stays Null - a startup check must not "
                              "open network sockets or provider sessions");
        else
            installProductionTranslation();
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
    /// Mounts the production translation chain (task 012): the real backend of
    /// task 009 inside the recovery supervisor of task 010, its policy built from
    /// the operator's settings. From here up the application sees only the task
    /// 007 contract - the OpenAI names stop at this composition root.
    void installProductionTranslation()
    {
        const auto& cfg = controller_.config().current().translation;

        secretStore_ = std::make_unique<EnvironmentSecretStore>();

        // Presence only, never the value (AGENTS.md 10). Without a key the session
        // open will refuse - the controller already treats that as a recorded,
        // recoverable failure that leaves the audio path running (AGENTS.md 12).
        if (secretStore_->identifiers().empty())
            liveai::log::warning(kLogComponent,
                                 "translation: no OPENAI_API_KEY in the environment - sessions will refuse "
                                 "until a credential exists (development store; task 015 owns storage). "
                                 "Audio keeps running regardless.");
        else
            liveai::log::info(kLogComponent, "translation: OpenAI credential found (development store)");

        liveai::network::OpenAIRealtimeOptions options; // documented defaults, no invented overrides

        auto backend = std::make_unique<liveai::network::OpenAIRealtimeBackend>(*secretStore_, options);

        liveai::translation::ReconnectSupervisor::Policy policy;
        policy.enabled = cfg.reconnectEnabled;
        policy.initialBackoffMs = cfg.reconnectInitialBackoffMs;
        policy.maxBackoffMs = cfg.reconnectMaxBackoffMs;
        policy.sessionMaxAgeMs = cfg.sessionMaxAgeSeconds > 0 ? cfg.sessionMaxAgeSeconds * 1000 : 0;

        controller_.setTranslationBackend(
            std::make_unique<liveai::translation::ReconnectSupervisor>(std::move(backend), policy));

        liveai::log::info(kLogComponent,
                          std::string("translation: OpenAI backend mounted behind the reconnect supervisor (recovery ")
                              + (cfg.reconnectEnabled ? "on" : "pass-through") + ")");
    }

    /// Declared before the controller so it outlives the backend holding a
    /// reference to it.
    std::unique_ptr<liveai::security::ISecretStore> secretStore_;
#endif

    liveai::ApplicationController controller_;
    std::unique_ptr<liveai::OperatorWindow> window_;
};

START_JUCE_APPLICATION(LingoFlowApplication)
