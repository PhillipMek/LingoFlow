#include "App/ApplicationController.h"

#include "Audio/Null/NullAudioBackend.h"
#include "NDI/Null/NullNdiOutput.h"
#include "Translation/Null/NullTranslationBackend.h"
#include "Utils/Log.h"

namespace liveai {
namespace {

constexpr std::string_view kComponent = "app";

} // namespace

std::string_view nameOf(ApplicationState state) noexcept
{
    switch (state)
    {
        case ApplicationState::stopped:  return "stopped";
        case ApplicationState::starting: return "starting";
        case ApplicationState::running:  return "running";
        case ApplicationState::stopping: return "stopping";
        case ApplicationState::faulted:  return "faulted";
    }
    return "faulted";
}

std::string describeStatus(const AppStatus& status)
{
    std::string out;
    out += "Application: ";
    out += nameOf(status.application);
    out += "\nAudio backend: ";
    out += status.audioBackendName.empty() ? "none" : status.audioBackendName;
    out += " (";
    out += audio::nameOf(status.audio);
    out += ")\nTranslation session: ";
    out += translation::nameOf(status.session);
    out += "\nNDI subtitles: ";
    out += ndi::nameOf(status.ndi);
    out += "\nDetail: ";
    out += status.detail.empty() ? "ok" : status.detail;
    return out;
}

ApplicationController::ApplicationController()
    : audioBackend_(std::make_unique<audio::NullAudioBackend>())
    , translationBackend_(std::make_unique<translation::NullTranslationBackend>())
    , ndiOutput_(std::make_unique<ndi::NullNdiOutput>())
{
}

void ApplicationController::setAudioBackend(std::unique_ptr<audio::IAudioBackend> backend)
{
    if (state_ != ApplicationState::stopped)
    {
        log::warning(kComponent, "audio backend replaced while not stopped - stopping first");
        stop();
    }
    audioBackend_ = std::move(backend);
}

void ApplicationController::setTranslationBackend(std::unique_ptr<translation::ITranslationBackend> backend)
{
    if (state_ != ApplicationState::stopped)
    {
        log::warning(kComponent, "translation backend replaced while not stopped - stopping first");
        stop();
    }

    translationBackend_ = std::move(backend);

    if (translationBackend_ != nullptr)
        translationBackend_->setSink(*this);
}

void ApplicationController::setNdiOutput(std::unique_ptr<ndi::INdiOutput> output)
{
    if (state_ != ApplicationState::stopped)
    {
        log::warning(kComponent, "NDI output replaced while not stopped - stopping first");
        stop();
    }
    ndiOutput_ = std::move(output);
}

bool ApplicationController::start()
{
    if (state_ == ApplicationState::running || state_ == ApplicationState::starting)
    {
        log::warning(kComponent, std::string("start() ignored: already ").append(nameOf(state_)));
        return false;
    }

    if (state_ == ApplicationState::faulted)
    {
        log::error(kComponent, "start() refused from faulted state: " + faultReason_);
        return false;
    }

    state_ = ApplicationState::starting;
    log::info(kComponent, "starting");

    std::string error;
    if (!startAudio(error))
    {
        // Audio is the only subsystem whose failure stops the application:
        // without a device there is nothing to interpret.
        fault("audio: " + error);
        log::error(kComponent, "audio start failed: " + error);
        diagnostics_.noteError("audio", error);
        return false;
    }

    // Translation and NDI are best-effort at this level: a failure is recorded
    // and stays recoverable, it must not take the audio path down (AGENTS.md 12).
    if (!startSession(error))
    {
        log::warning(kComponent, "translation session not started: " + error);
        diagnostics_.noteError("translation", error);
    }

    if (!startNdi(error))
    {
        log::warning(kComponent, "NDI output not started: " + error);
        diagnostics_.noteError("ndi", error);
    }

    state_ = ApplicationState::running;
    log::info(kComponent, "running");
    return true;
}

void ApplicationController::stop()
{
    if (state_ == ApplicationState::stopped)
        return;

    state_ = ApplicationState::stopping;
    log::info(kComponent, "stopping");

    stopNdi();
    stopSession();
    stopAudio();

    faultReason_.clear();
    state_ = ApplicationState::stopped;
    log::info(kComponent, "stopped");
}

bool ApplicationController::startAudio(std::string& error)
{
    if (audioBackend_ == nullptr)
    {
        error = "no audio backend is configured";
        return false;
    }

    const auto& cfg = config_.current();

    // SPEC "ASIO Device Selection": one ASIO device serves input and output, and a
    // configured device must never be silently replaced by another one.
    if (!cfg.audio.inputDeviceId.empty() && !cfg.audio.outputDeviceId.empty()
        && cfg.audio.inputDeviceId != cfg.audio.outputDeviceId)
    {
        error = "ASIO uses a single device for input and output, but settings name two: '"
              + cfg.audio.inputDeviceId + "' and '" + cfg.audio.outputDeviceId + "'";
        return false;
    }

    // The device id is consumed by the ASIO backend, which task 005 wires in here.
    audio::DeviceRequest request;
    request.sampleRate = cfg.audio.sampleRate;
    request.bufferFrames = cfg.audio.bufferFrames;
    request.inputChannel = cfg.audio.inputChannel;
    request.outputChannel = cfg.audio.outputChannel;

    if (!engine_.activate(*audioBackend_, request, error))
        return false;

    lastAudioError_.clear();
    return true;
}

void ApplicationController::stopAudio() noexcept
{
    engine_.deactivate();
}

bool ApplicationController::startSession(std::string& error)
{
    if (translationBackend_ == nullptr)
    {
        error = "no translation backend is configured";
        return false;
    }

    translationBackend_->setSink(*this);

    translation::SessionRequest request;
    request.pair.input = config_.current().translation.inputLanguage;
    request.pair.output = config_.current().translation.outputLanguage;
    request.instructions = config_.current().translation.instructions;

    return translationBackend_->openSession(request, error);
}

void ApplicationController::stopSession() noexcept
{
    if (translationBackend_ != nullptr)
        translationBackend_->closeSession();
}

bool ApplicationController::startNdi(std::string& error)
{
    if (!config_.current().ndi.enabled)
        return true;   // disabled by settings: nothing to start, not a failure

    if (ndiOutput_ == nullptr)
    {
        error = "no NDI output is configured";
        return false;
    }

    if (!ndiOutput_->start(config_.current().ndi.streamName, error))
    {
        ndiOutput_->stop();
        return false;
    }

    error.clear();
    return true;
}

void ApplicationController::stopNdi() noexcept
{
    if (ndiOutput_ != nullptr)
        ndiOutput_->stop();
}

void ApplicationController::publishToNdi(std::string_view text, bool isFinal)
{
    if (ndiOutput_ == nullptr)
        return;

    if (ndiOutput_->state() == ndi::OutputState::disabled)
        return;   // feature is off: dropping subtitles is expected, not an error

    ndi::SubtitleFrame frame;
    frame.text.assign(text);
    frame.final = isFinal;
    frame.sequence = static_cast<long long>(ndiSequence_.fetch_add(1, std::memory_order_relaxed) + 1);

    std::string error;
    if (!ndiOutput_->publish(frame, error))
    {
        if (!error.empty())
        {
            // NDI never interrupts translation audio: count, record, continue.
            diagnostics_.countNdiError();
            diagnostics_.noteError("ndi", error);
        }
    }
}

void ApplicationController::fault(std::string reason)
{
    faultReason_ = std::move(reason);
    state_ = ApplicationState::faulted;
}

AppStatus ApplicationController::status() const
{
    AppStatus s;
    s.application = state_;
    s.audio = audioBackend_ != nullptr ? audioBackend_->state() : audio::BackendState::closed;
    s.session = sessionState();
    s.ndi = ndiOutput_ != nullptr ? ndiOutput_->state() : ndi::OutputState::disabled;
    s.audioBackendName = audioBackend_ != nullptr ? std::string(audioBackend_->name()) : std::string();

    if (!faultReason_.empty())
        s.detail = faultReason_;
    else if (!lastAudioError_.empty())
        s.detail = lastAudioError_;

    return s;
}

translation::SessionState ApplicationController::sessionState() const noexcept
{
    return translationBackend_ != nullptr ? translationBackend_->state() : translation::SessionState::closed;
}

bool ApplicationController::loadSettings(const std::filesystem::path& file, std::string& note,
                                         const std::filesystem::path& persistTo)
{
    config_.setStore(config::ConfigStore(file));

    if (!config_.load(note))
        return false;

    // Recovery details belong in the log: an operator must be able to see that the
    // file was repaired, not silently get defaults.
    if (!note.empty())
        log::info(kComponent, "settings: " + note);

    for (const auto& problem : config_.lastLoad().problems)
        log::warning(kComponent, "settings field " + problem.field + ": " + problem.message);

    if (!persistTo.empty() && persistTo != file)
    {
        // One-time move of the settings into the new location. The old file is left
        // exactly where it is: a rename must not delete a user's data.
        config_.setStore(config::ConfigStore(persistTo));

        std::string error;
        if (config_.save(error))
            log::info(kComponent, "settings migrated from '" + file.string() + "' to '" + persistTo.string() + "'");
        else
            log::warning(kComponent, "settings were read but could not be written to '" + persistTo.string()
                                         + "': " + error);
    }

    return true;
}

bool ApplicationController::saveSettings(std::string& error)
{
    return config_.save(error);
}

void ApplicationController::onTranslatedAudio(const float* samples, int frameCount, int sampleRate)
{
    // Worker/network thread. There is no playback queue yet: task 005 adds the
    // lock-free ring buffer and task 012 connects it to the engine. Silently
    // dropping the block keeps the boundary honest instead of pretending a path
    // exists.
    (void)sampleRate;
    log::debug(kComponent, "translated audio dropped (no buffer yet): " + std::to_string(frameCount)
                               + " frames from " + (samples != nullptr ? "valid pointer" : "null pointer"));
}

void ApplicationController::onPartialText(std::string_view text)
{
    diagnostics_.countPartialTextEvent();
    publishToNdi(text, false);
}

void ApplicationController::onFinalText(std::string_view text)
{
    diagnostics_.countFinalTextEvent();
    publishToNdi(text, true);
}

void ApplicationController::onSessionStateChanged(translation::SessionState state)
{
    log::info(kComponent, std::string("translation session: ").append(nameOf(state)));
}

} // namespace liveai
