#include "App/ApplicationController.h"

#include <format>

#include "Audio/Null/NullAudioBackend.h"
#include "NDI/Null/NullNdiOutput.h"
#include "Translation/Null/NullTranslationBackend.h"
#include "Translation/LanguageRegistry.h"
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

    audioBackendOverridden_ = backend != nullptr;
    audioBackend_ = std::move(backend);
}

void ApplicationController::setAudioBackendFactory(AudioBackendFactory factory)
{
    if (state_ != ApplicationState::stopped)
    {
        log::warning(kComponent, "audio backend factory replaced while not stopped - stopping first");
        stop();
    }

    audioBackendFactory_ = std::move(factory);
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

    {
        // A new run has a new story to tell; the previous run's failure must not
        // haunt the status line of the next one.
        std::lock_guard lock(subsystemMutex_);
        lastTranslationError_.clear();
    }

    warnedRateMismatch_.store(false, std::memory_order_relaxed);
    warnedNoBuffer_.store(false, std::memory_order_relaxed);
    warnedBadBlock_.store(false, std::memory_order_relaxed);

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

    // SPEC "ASIO Device Selection": a configured device is opened or the application
    // reports that it is unavailable - it is never quietly swapped for another one.
    const std::string deviceId = cfg.audio.inputDeviceId.empty() ? cfg.audio.outputDeviceId
                                                                 : cfg.audio.inputDeviceId;

    audio::DeviceRequest request;
    request.deviceId = deviceId;
    request.sampleRate = cfg.audio.sampleRate;
    request.bufferFrames = cfg.audio.bufferFrames;
    request.inputChannel = cfg.audio.inputChannel;
    request.outputChannel = cfg.audio.outputChannel;

    // SPEC "Output Jitter Buffer": the operator's pre-roll, applied before the device
    // starts so the buffers are allocated at the right size.
    engine_.setJitterBufferMs(cfg.translation.jitterBufferMs);

    // SPEC "Input Gain" / "Output Gain": applied before the device starts, so the first
    // block already plays the configured level instead of gliding into it.
    // Mute is not restored from anything on purpose. It is live stage state, and a room
    // that comes back muted after a restart is a fault nobody asked for; both sides start
    // unmuted and the operator decides (SPEC "Start Sequence").
    engine_.setInputGainDb(cfg.audio.inputGainDb);
    engine_.setOutputGainDb(cfg.audio.outputGainDb);

    if (!deviceId.empty())
    {
        if (audioBackendFactory_ != nullptr)
        {
            std::string factoryError;
            auto built = audioBackendFactory_(request, factoryError);

            if (built == nullptr)
            {
                error = "could not create an audio backend for the selected device '" + deviceId + "'";

                if (!factoryError.empty())
                    error += ": " + factoryError;

                log::error(kComponent, error);
                return false;
            }

            audioBackend_ = std::move(built);
            log::info(kComponent, "opening the audio device selected in settings: '" + deviceId + "'");
        }
        else if (!audioBackendOverridden_)
        {
            error = "settings select the audio device '" + deviceId
                  + "', but this build has no audio backend factory";
            log::error(kComponent, error);
            return false;
        }
    }
    else if (!audioBackendOverridden_)
    {
        log::warning(kComponent,
                     "no audio device selected in settings: running on the null backend, no live audio");
    }

    if (!engine_.activate(*audioBackend_, request, error))
        return false;

    // One line in the log saying which levels the room is now running at. The operator
    // needs to be able to tell "my settings were applied" from "the defaults were used",
    // and a start-up log is where that question gets asked.
    log::info(kComponent,
              "audio gains in effect: input " + std::format("{:+.1f}", engine_.inputGainDb())
            + " dB, output " + std::format("{:+.1f}", engine_.outputGainDb())
            + " dB, glide " + std::to_string(engine_.gainRampMs()) + " ms");

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

    const AppConfig& cfg = config_.current();

    translation::SessionRequest request;
    request.pair.input = cfg.translation.inputLanguage;
    request.pair.output = cfg.translation.outputLanguage;
    request.instructions = cfg.translation.instructions;

    // Task 007: the rates and the model travel with the request, so the backend
    // knows at what rate audio is coming and at what rate the answer must
    // arrive. An empty model means "backend default" - the set of legal values
    // is what task 008 establishes from the official documentation, not
    // something the application invents here.
    request.model = cfg.translation.modelHint;
    request.inputSampleRate = engine_.sampleRate();
    request.outputSampleRate = engine_.sampleRate();

    // Capability-driven start (task 011, AGENTS.md 9): the configured pair must
    // be one the product can actually deliver, checked against the versioned
    // manifest - the controller holds no list of its own. The backend re-checks
    // the same registry on its side; this gate is what stops a Null or mock
    // backend from being started with a pair the event cannot honor.
    const translation::PairCheck pair =
        translation::openAiManifest().checkPair(request.pair.input, request.pair.output);
    if (!pair)
    {
        error = "language pair not supported: " + pair.detail;
        return false;
    }

    const bool opened = translationBackend_->openSession(request, error);

    if (opened)
        startStreaming();

    return opened;
}

void ApplicationController::stopSession() noexcept
{
    // Streaming stops first: after closeSession() the backend guarantees no sink
    // callbacks and must see no further submits either, so the worker is joined
    // before the session closes - the same ordering the contract's rule 5 was
    // built for, applied on the capture side.
    stopStreaming();

    if (translationBackend_ != nullptr)
        translationBackend_->closeSession();
}

void ApplicationController::startStreaming()
{
    if (translationBackend_ == nullptr)
        return;

    streamer_ = std::make_unique<TranslationStreamer>(engine_, *translationBackend_, &diagnostics_);

    std::string error;

    if (streamer_->start(error))
    {
        log::info(kComponent,
                  "translation streaming started: device capture at "
                      + std::to_string(engine_.sampleRate()) + " Hz feeds the session");
    }
    else
    {
        // A session without capture is a half-machine: keep it (text plumbing and
        // recovery still make sense) but say plainly that the translator is not
        // being fed. This is never swallowed.
        log::warning(kComponent, "translation session is open but the capture is not streamed: " + error);
        diagnostics_.noteError("translation", "streaming not started: " + error);
        streamer_.reset();
    }
}

void ApplicationController::stopStreaming() noexcept
{
    if (streamer_ == nullptr)
        return;

    // The run's throughput, printed while it is still true: what the translator
    // got and what the gap policy dropped. The operator's "did we miss anything"
    // question is answered by these numbers (task 017 exports them).
    log::info(kComponent,
              "translation streaming stopped: " + std::to_string(streamer_->submittedFrames())
                  + " frames submitted, " + std::to_string(streamer_->gapRefusedFrames())
                  + " frames gap-refused");

    streamer_->stop();
    streamer_.reset();
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
    else
    {
        // The freshest translation failure, when nothing bigger is wrong: the
        // session can be faulted while the application runs fine, and the
        // operator still has to be able to ask "why".
        std::lock_guard lock(subsystemMutex_);
        s.detail = lastTranslationError_;
    }

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
    // Worker/network thread. This is the delivery side of the task 007 contract
    // and the reason the engine's jitter buffer exists (task 005): translated
    // audio becomes audible exactly here, through the one buffer the callback
    // reads, never anywhere else.
    if (samples == nullptr || frameCount <= 0)
    {
        diagnostics_.countRejectedAudioFrames(static_cast<std::uint64_t>(frameCount > 0 ? frameCount : 0));

        if (!warnedBadBlock_.exchange(true, std::memory_order_relaxed))
            log::warning(kComponent, "translated audio rejected: empty or null block");

        return;
    }

    const std::uint64_t frames = static_cast<std::uint64_t>(frameCount);

    // The device must be running to play anything, and channel 0 is the
    // translation output channel (SPEC "Output Channel"). No buffer means the
    // audio path is down - a fact for the counters, not a reason to crash or
    // to queue unbounded memory waiting for a device that may never return.
    audio::AudioJitterBuffer* jitter = engine_.outputJitter(0);

    if (jitter == nullptr)
    {
        diagnostics_.countRejectedAudioFrames(frames);

        if (!warnedNoBuffer_.exchange(true, std::memory_order_relaxed))
            log::warning(kComponent, "translated audio rejected: the audio path is not running");

        return;
    }

    // No resampler exists (and inventing a silent one is forbidden): a block at
    // the wrong speed would reach the audience. Reject, count, say it once.
    const int deviceRate = engine_.sampleRate();

    if (sampleRate != deviceRate)
    {
        diagnostics_.countRejectedAudioFrames(frames);

        if (!warnedRateMismatch_.exchange(true, std::memory_order_relaxed))
            log::warning(kComponent, "translated audio rejected: delivered at " + std::to_string(sampleRate)
                                         + " Hz, the device plays at " + std::to_string(deviceRate)
                                         + " Hz; further blocks of this kind are counted, not logged");

        return;
    }

    // SPSC: this thread is the buffer's only producer, the audio callback its
    // only consumer. write() never blocks and drops the newest frames when the
    // network outruns playback - counted below, never swallowed.
    const std::size_t written = jitter->write(samples, static_cast<std::size_t>(frameCount));

    diagnostics_.countTranslatedAudioFrames(frames);

    if (written != static_cast<std::size_t>(frameCount))
        diagnostics_.countTranslatedAudioDroppedFrames(static_cast<std::uint64_t>(frameCount)
                                                       - static_cast<std::uint64_t>(written));
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

    // A fresh (re)connected session deserves a fresh warning: if the new backend
    // behaves differently the operator must hear about it again, once.
    if (state == translation::SessionState::connected)
    {
        warnedRateMismatch_.store(false, std::memory_order_relaxed);
        warnedNoBuffer_.store(false, std::memory_order_relaxed);
        warnedBadBlock_.store(false, std::memory_order_relaxed);
    }
}

void ApplicationController::onTranslationError(const translation::TranslationError& error)
{
    noteTranslationError(error);

    // AGENTS.md 12: a translation failure never touches the audio path. Nothing
    // here stops the engine, closes the device or waits for the network - the
    // decision to retry or reopen belongs to task 010, not to this callback.
    if (error.fatal)
    {
        diagnostics_.noteError("translation",
                               std::string(nameOf(error.category)) + ": " + error.message);
    }
}

void ApplicationController::noteTranslationError(const translation::TranslationError& error)
{
    diagnostics_.countTranslationError(error.fatal);

    const std::string text = std::string(error.fatal ? "fatal " : "")
                            + std::string(nameOf(error.category)) + ": " + error.message;

    std::lock_guard lock(subsystemMutex_);

    if (lastTranslationError_ == text)
        log::debug(kComponent, "translation error (repeated): " + text);
    else
    {
        log::warning(kComponent, "translation error: " + text);
        lastTranslationError_ = text;
    }
}

} // namespace liveai
