// Live AI Interpreter - operator application.
//
// The UI talks to exactly one object (ApplicationController) and reads exactly
// one type (AppStatus). It has no knowledge of audio devices, wire protocols or
// subtitle transports (AGENTS.md 7).
//
// Command line:
//   --smoke   start the subsystems, log the resulting status, stop, quit without
//             creating a window. Used by the bootstrap verification and by CI; it
//             is not a substitute for an operator-visible check.

#include <JuceHeader.h>

#include <format>
#include <string>

#include "App/ApplicationController.h"
#include "Utils/Log.h"

namespace {

constexpr std::string_view kLogComponent = "app";

juce::File logDirectory()
{
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("Live AI Interpreter")
        .getChildFile("logs");
}

liveai::LogConfig makeLogConfig(const liveai::AppConfig& settings, bool writeConsole)
{
    liveai::LogConfig cfg;
    cfg.level = liveai::log::levelFromName(settings.diagnostics.logLevel);
    cfg.writeConsole = writeConsole;
    cfg.filePath = settings.diagnostics.writeLogFile
                       ? logDirectory().getChildFile("liveai.log").getFullPathName().toStdString()
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
        : juce::DocumentWindow("Live AI Interpreter",
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
class LiveAIInterpreterApplication final : public juce::JUCEApplication
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
        if (!controller_.loadSettings(liveai::config::ConfigStore::defaultFile(), settingsNote))
            liveai::log::error(kLogComponent, "settings could not be read: " + settingsNote);

        // Settings decide the log level and whether a log file is written. In smoke
        // mode the file stays on, because that file is how the startup is observed.
        auto logging = controller_.config().current();
        if (smoke)
            logging.diagnostics.writeLogFile = true;
        liveai::log::configure(makeLogConfig(logging, /*writeConsole = */ false));

        if (!controller_.start())
        {
            liveai::log::error(kLogComponent, "application failed to start: " + controller_.status().detail);
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

START_JUCE_APPLICATION(LiveAIInterpreterApplication)
