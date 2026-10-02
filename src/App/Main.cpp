// LingoFlow - operator application.
//
// The UI talks to exactly one object (ApplicationController) and reads exactly
// one type (AppStatus). It has no knowledge of audio devices, wire protocols or
// subtitle transports (AGENTS.md 7).
//
// Command line:
//   --smoke   start the subsystems, log the resulting status, stop, quit without
//             creating a window. Used by the bootstrap verification and by CI; it
//             is not a substitute for an operator-visible check.
//
// Exit codes:
//   0   started (and, with --smoke, shut down) normally
//   2   a subsystem refused to start (e.g. the selected ASIO device is unavailable).
//       The reason is in the log and in the status detail.

#include <JuceHeader.h>

#include <filesystem>
#include <format>
#include <string>

#include "App/ApplicationController.h"
#include "Config/ConfigStore.h"
#include "Platform/Asio/JuceAsioBackend.h"
#include "Utils/Log.h"

namespace {

constexpr std::string_view kLogComponent = "app";

/// Exit codes of the operator application. 0 always means "the application did what it
/// was asked"; a non-zero code is a failure an operator or CI can branch on without
/// reading the log.
constexpr int kExitStartupFailure = 2;

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
/// types at all - only AppStatus and describeStatus().
juce::String statusText(const liveai::AppStatus& status)
{
    return juce::String(std::format("{} {}\n{}",
                                    JUCE_APPLICATION_NAME_STRING,
                                    JUCE_APPLICATION_VERSION_STRING,
                                    liveai::describeStatus(status)));
}

//==============================================================================
class StatusComponent final : public juce::Component, public juce::Timer
{
public:
    explicit StatusComponent(const liveai::ApplicationController& controller) : controller_(controller)
    {
        label_.setJustificationType(juce::Justification::centredLeft);
        label_.setColour(juce::Label::textColourId, juce::Colours::whitesmoke);
        addAndMakeVisible(label_);
        refresh();
        startTimer(500);
    }

    void timerCallback() override { refresh(); }

    void resized() override { label_.setBounds(getLocalBounds().reduced(18)); }

private:
    void refresh() { label_.setText(statusText(controller_.status()), juce::NotificationType::dontSendNotification); }

    const liveai::ApplicationController& controller_;
    juce::Label label_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StatusComponent)
};

class MainWindow final : public juce::DocumentWindow
{
public:
    explicit MainWindow(const liveai::ApplicationController& controller)
        : juce::DocumentWindow("LingoFlow",
                               juce::Colour(0xff232629u),
                               juce::DocumentWindow::closeButton)
    {
        setUsingNativeTitleBar(true);
        setContentOwned(new StatusComponent(controller), true);
        setResizable(true, true);
        setSize(520, 260);
        centreWithSize(getWidth(), getHeight());
        setVisible(true);
    }

    void closeButtonPressed() override
    {
        if (auto* app = juce::JUCEApplication::getInstance())
            app->systemRequestedQuit();
    }

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainWindow)
};

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

        window_ = std::make_unique<MainWindow>(controller_);
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
    liveai::ApplicationController controller_;
    std::unique_ptr<MainWindow> window_;
};

START_JUCE_APPLICATION(LingoFlowApplication)
