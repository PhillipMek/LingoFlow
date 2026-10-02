// Live AI Interpreter - operator application.
//
// Bootstrap stage: JUCE shell, lifecycle and logging only. No audio backend is
// opened and no network access happens here (tasks 004+/009+ add those through
// IAudioBackend / ITranslationBackend).
//
// Command line:
//   --smoke   start, log, stop, quit without creating any window.
//             Used by CI and by the bootstrap verification; it must never be
//             used as a substitute for the operator-visible checks.

#include <JuceHeader.h>

#include <format>
#include <string>
#include <string_view>

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

juce::String toJuce(std::string_view text)
{
    return juce::String(std::string(text));
}

liveai::LogConfig makeLogConfig(bool writeConsole)
{
    liveai::LogConfig cfg;
    cfg.level = liveai::LogLevel::debug;
    cfg.writeConsole = writeConsole;
    cfg.filePath = logDirectory().getChildFile("liveai.log").getFullPathName().toStdString();
    return cfg;
}

//==============================================================================
class MainComponent final : public juce::Component
{
public:
    explicit MainComponent(const liveai::ApplicationController& controller)
    {
        status_.setText("Live AI Interpreter " + juce::String(JUCE_APPLICATION_VERSION_STRING)
                            + "\nApplication state: " + toJuce(liveai::nameOf(controller.state())),
                        juce::NotificationType::dontSendNotification);
        status_.setJustificationType(juce::Justification::centredLeft);
        status_.setColour(juce::Label::textColourId, juce::Colours::whitesmoke);
        addAndMakeVisible(status_);
    }

    void resized() override
    {
        status_.setBounds(getLocalBounds().reduced(18));
    }

private:
    juce::Label status_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
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
        setContentOwned(new MainComponent(controller), true);
        setResizable(true, true);
        setSize(460, 220);
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
        // source of truth for automated startup checks.
        liveai::log::configure(makeLogConfig(/*writeConsole = */ false));

        liveai::log::info(kLogComponent,
                          std::format("application {} initialised (juce {} on {})",
                                      getApplicationVersion().toStdString(),
                                      juce::SystemStats::getJUCEVersion().toStdString(),
                                      juce::SystemStats::getOperatingSystemName().toStdString()));

        if (!controller_.start())
        {
            liveai::log::error(kLogComponent, "application failed to start");
            quit();
            return;
        }

        if (smoke)
        {
            liveai::log::info(kLogComponent, "smoke mode: shutting down without UI");
            juce::MessageManager::callAsync([this] { quit(); });
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
