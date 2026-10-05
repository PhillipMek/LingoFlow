#include "App/ApplicationController.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <sstream>

#include "App/UiModel.h"
#include "Audio/LevelMeter.h"
#include "Audio/Null/NullAudioBackend.h"
#include "Config/ConfigStore.h"
#include "Diagnostics/DiagnosticsExport.h"
#include "NDI/Null/NullNdiOutput.h"
#include "Translation/Null/NullTranslationBackend.h"
#include "Translation/LanguageRegistry.h"
#include "Utils/Log.h"

namespace liveai {
namespace {

constexpr std::string_view kComponent = "app";

/// The meter view as one export line: the product reads levels, not raw
/// fractions (the same conversion the UI bar uses).
std::string meterText(const audio::LevelMeter* meter)
{
    if (meter == nullptr)
        return "no pipeline";

    return std::format("peak {:.1f} dBFS, rms {:.1f} dBFS, {}",
                       audio::linearToDb(meter->peakLinear()),
                       audio::linearToDb(meter->rmsLinear()),
                       meter->signalPresent() ? "signal" : "no signal");
}

/// log::timestampNow() shaped into a filename-safe stamp: the export file and
/// the log lines sort together and name the same minute.
std::string fileStamp()
{
    std::string stamp = log::timestampNow();
    for (auto& c : stamp)
    {
        if (c == ':' || c == ' ')
            c = '-';
    }
    return stamp;
}

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
    , ndiOutput_(std::make_unique<ndi::NullNdiOutput>())
    , translationBackend_(std::make_unique<translation::NullTranslationBackend>())
{
    // The one route text takes (task 013): sink -> typed pipeline -> listener.
    // The listener runs on the ingesting (backend worker) thread while the
    // pipeline's lock is held; publishing to NDI is non-blocking by its own
    // contract, and this lambda adds nothing that could block. Audio never
    // passes through here, so text cannot stall it and vice versa.
    textPipeline_.setListener(
        [this](const translation::TranslationTextEvent& event)
        {
            publishToNdi(event);
        });
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

void ApplicationController::setDeveloperPlan(DeveloperPlan plan)
{
    if (state_ != ApplicationState::stopped)
    {
        // The plan decides what gets MOUNTED at start; swapping it under a
        // running pipeline would leave the running world and the plan telling
        // different stories, so the pipeline goes down first - the same
        // discipline as every other injection seam.
        log::warning(kComponent, "developer plan replaced while not stopped - stopping first");
        stop();
    }

    devPlan_ = std::move(plan);

    if (devPlan_.enabled)
        log::warning(kComponent, "developer plan mounted: " + devPlan_.badge);
}

bool ApplicationController::loopbackActive() const noexcept
{
    return loopback_ != nullptr && loopback_->running();
}

std::uint64_t ApplicationController::loopbackTransferredFrames() const noexcept
{
    return loopback_ != nullptr ? loopback_->transferredFrames() : 0;
}

void ApplicationController::setDeviceLister(DeviceLister lister)
{
    deviceLister_ = std::move(lister);
}

int ApplicationController::refreshDevices()
{
    if (deviceLister_ == nullptr)
    {
        log::info(kComponent, "device enumeration is not installed: the device list stays as it is");
        return static_cast<int>(devices_.size());
    }

    devices_ = deviceLister_();
    log::info(kComponent, "device scan found " + std::to_string(devices_.size()) + " device(s)");
    return static_cast<int>(devices_.size());
}

void ApplicationController::clearFault() noexcept
{
    if (state_ != ApplicationState::faulted)
    {
        log::debug(kComponent, "clearFault() ignored: state is " + std::string(nameOf(state_)));
        return;
    }

    // The fault is history (counters keep it, the log said it when it happened);
    // clearing is the operator saying "I fixed the cause, let me try again".
    // What the retry does or does not achieve is the next start()'s truth to tell.
    log::info(kComponent, "fault cleared by operator: '" + faultReason_ + "' - ready to start again");
    diagnostics_.noteEvent("app", "fault cleared by operator: '" + faultReason_ + "'");
    faultReason_.clear();
    lastAudioError_.clear();
    state_ = ApplicationState::stopped;
}

void ApplicationController::setGainsLive(float inputDb, float outputDb) noexcept
{
    engine_.setInputGainDb(inputDb);
    engine_.setOutputGainDb(outputDb);
}

void ApplicationController::setMutesLive(bool inputMuted, bool outputMuted) noexcept
{
    // Runtime-only by task 006's design: mute is a live hand on the fader, not a
    // setting the show starts with. Nothing is persisted here on purpose.
    engine_.setInputMuted(inputMuted);
    engine_.setOutputMuted(outputMuted);
}

void ApplicationController::setJitterLive(int jitterMs) noexcept
{
    engine_.setJitterBufferMs(jitterMs);
}

bool ApplicationController::updateSettings(const AppConfig& candidate, std::string& note)
{
    const auto previous = config_.current();
    std::string error;

    if (!config_.update(candidate, error))
    {
        // ConfigManager::update refuses atomically: nothing changed in memory.
        note = "settings refused: " + error;
        log::warning(kComponent, note);
        return false;
    }

    // The accepted candidate is now current: apply what applies live, so a slider
    // release and an on-screen readout never disagree with what the callback is
    // doing. Device/rate/buffer/channel/languages/NDI need the session or the
    // pipeline to be rebuilt - that is a Stop/Start away, and the note says so.
    const auto& cfg = config_.current();
    engine_.setInputGainDb(cfg.audio.inputGainDb);
    engine_.setOutputGainDb(cfg.audio.outputGainDb);
    engine_.setJitterBufferMs(cfg.translation.jitterBufferMs);

    // Task 015: the log level is a mid-show control - the venue run-sheet asks
    // for debug lines while everything is already running - and reconfiguring
    // sinks is a documented non-realtime act (Utils/Log). Only the level is
    // taken from settings here; which sinks exist at all is the composition
    // root's startup decision, and the note below names what waits for a
    // restart instead of pretending this applies more than it does.
    {
        auto logging = log::config();
        logging.level = log::levelFromName(cfg.diagnostics.logLevel);
        log::configure(logging);
    }

    std::string saveError;
    if (saveSettings(saveError))
        note = "settings saved. Gain, jitter and the log level are live. Device, sample rate, "
               "buffer, channels, languages, instructions and NDI take effect on Stop + Start. "
               "Recovery policy (backoff, session age) and the log-file sink take effect when "
               "the application restarts.";
    else
        note = "settings are active for this run but were NOT saved: " + saveError;

    // Task 019: developer mode is a MOUNTING decision - backends are chosen
    // when the composition root builds the world, and the plan snapshot the
    // running app holds does not change under its feet. Say it, do not hint.
    if (candidate.developer != previous.developer)
        note += " Developer-mode edits take effect when the application restarts.";

    log::info(kComponent, "operator settings update: " + note);
    diagnostics_.noteEvent("settings", note);
    return true;
}

void ApplicationController::setSecretStore(security::ISecretStore& store) noexcept
{
    // No stop-guard needed: this pointer only serves the operator's credential
    // actions. The backend holds its own reference from the composition root,
    // installed before start() - swapping what the UI writes to cannot yank
    // the ground from a running session, and if a root ever did this mid-run,
    // the log line below is the visible fact.
    secrets_ = &store;
    log::info(kComponent, "credential store installed: " + std::string(store.name()));
}

bool ApplicationController::hasApiSecret() const
{
    // Identifiers only: a presence check must not copy a secret across a thread.
    const auto ids = secrets_->identifiers();
    const std::string apiKey(security::kOpenAiApiKey);
    return std::find(ids.begin(), ids.end(), apiKey) != ids.end();
}

std::string ApplicationController::secretStoreName() const
{
    return std::string(secrets_->name());
}

bool ApplicationController::storeApiSecret(std::string_view secret, std::string& note)
{
    if (secret.empty())
    {
        // "Save" of an empty field is almost certainly not what was meant;
        // deleting has its own button and its own honest wording.
        note = "an empty key was not stored - use Remove if the intent is to delete it";
        log::warning(kComponent, note);
        return false;
    }

    if (const auto status = secrets_->store(security::kOpenAiApiKey, secret);
        status == security::SecretStatus::stored)
    {
        note = "API key stored in " + secretStoreName() + ". The value is not in the settings "
               "file, not in the log, and not kept on screen; sessions read it at Start.";
        log::info(kComponent, "translation: the operator stored the API key in " + secretStoreName()
                                  + " (value never logged)");
        diagnostics_.noteEvent("security", "operator stored the API key (value not logged)");
        return true;
    }
    else
    {
        note = "the credential store refused the key: " + std::string(security::nameOf(status))
               + " (" + secretStoreName() + ")";
        log::warning(kComponent, note);
        diagnostics_.noteEvent("security", note);
        return false;
    }
}

void ApplicationController::removeApiSecret(std::string& note)
{
    switch (const auto status = secrets_->remove(security::kOpenAiApiKey); status)
    {
        case security::SecretStatus::found:
            note = "API key removed from " + secretStoreName() + ".";
            log::info(kComponent, "translation: the operator removed the stored API key");
            diagnostics_.noteEvent("security", "operator removed the stored API key");
            break;

        case security::SecretStatus::notFound:
            // Said, not swallowed: clicking Remove with nothing stored is a
            // fact the operator is entitled to hear back.
            note = "there was no stored API key to remove (" + secretStoreName() + ").";
            log::info(kComponent, "translation: removal requested, nothing was stored");
            break;

        default:
            note = "the credential store refused the removal: "
                   + std::string(security::nameOf(status)) + " (" + secretStoreName() + ")";
            log::warning(kComponent, note);
            break;
    }
}

void ApplicationController::setApplicationVersion(std::string version)
{
    appVersion_ = std::move(version);
}

std::filesystem::path ApplicationController::diagnosticsDirectory() const
{
    return config::ConfigStore::defaultFile().parent_path() / "diagnostics";
}

bool ApplicationController::exportDiagnostics(const std::filesystem::path& directory,
                                              std::filesystem::path& written,
                                              std::string& note)
{
    using diagnostics::ExportSection;

    const AppStatus status = this->status();
    const auto diag = diagnostics_.snapshot();
    const auto& cfg = config_.current();
    AudioEngine& engine = engine_;

    std::vector<ExportSection> sections;

    sections.push_back({ "app", {
        { "state", std::string(nameOf(status.application)) },
        { "audio_state", std::string(audio::nameOf(status.audio)) },
        { "session_state", std::string(translation::nameOf(status.session)) },
        { "ndi_state", std::string(ndi::nameOf(status.ndi)) },
        { "audio_backend", status.audioBackendName },
        { "detail", status.detail },
    } });

    const LatencyEstimate latency = estimateBufferDelay(engine, cfg);

    std::vector<std::pair<std::string, std::string>> audioRows;
    audioRows.emplace_back("sample_rate", std::to_string(engine.sampleRate()));
    audioRows.emplace_back("buffer_frames", std::to_string(engine.bufferFrames()));
    audioRows.emplace_back("audio_blocks", std::to_string(diag.audioBlocks));
    audioRows.emplace_back("audio_frames", std::to_string(diag.audioFrames));
    audioRows.emplace_back("underruns", std::to_string(diag.underruns));
    audioRows.emplace_back("overruns", std::to_string(diag.overruns));
    audioRows.emplace_back("input_ring_dropped_frames", std::to_string(engine.inputRingDroppedFrames()));
    audioRows.emplace_back("output_silence_frames", std::to_string(engine.outputSilenceFrames()));
    audioRows.emplace_back("jitter_fill_frames", std::to_string(engine.jitterFillFrames()));
    audioRows.emplace_back("clip_frames_in", std::to_string(engine.inputClippedFrames()));
    audioRows.emplace_back("clip_frames_input_gain", std::to_string(engine.inputGainClippedFrames()));
    audioRows.emplace_back("clip_frames_output_gain", std::to_string(engine.outputGainClippedFrames()));
    audioRows.emplace_back("gain_requests_clamped", std::to_string(engine.gainRequestsClamped()));
    audioRows.emplace_back("gain_requests_rejected", std::to_string(engine.gainRequestsRejected()));
    audioRows.emplace_back("nonfinite_input_frames", std::to_string(engine.nonFiniteInputFrames()));
    audioRows.emplace_back("malformed_callbacks", std::to_string(engine.malformedCallbacks()));
    audioRows.emplace_back("oversized_callbacks", std::to_string(engine.oversizedCallbacks()));
    audioRows.emplace_back("input_level", meterText(engine.inputMeter(0)));
    audioRows.emplace_back("output_level", meterText(engine.outputMeter(0)));
    audioRows.emplace_back("input_gain_db", std::format("{:.1f}", engine.inputGainDb()));
    audioRows.emplace_back("output_gain_db", std::format("{:.1f}", engine.outputGainDb()));
    audioRows.emplace_back("applied_input_gain_db", std::format("{:.1f}", engine.appliedInputGainDb()));
    audioRows.emplace_back("applied_output_gain_db", std::format("{:.1f}", engine.appliedOutputGainDb()));
    audioRows.emplace_back("input_muted", engine.inputMuted() ? "yes" : "no");
    audioRows.emplace_back("output_muted", engine.outputMuted() ? "yes" : "no");
    audioRows.emplace_back("buffer_block_ms", std::to_string(latency.blockMs));
    audioRows.emplace_back("pipeline_buffer_delay_ms", std::to_string(latency.totalMs));
    audioRows.emplace_back("pipeline_buffer_delay_source", latency.source);
    audioRows.emplace_back("pipeline_buffer_delay_note",
                           "buffer arithmetic only; translation and mouth-to-ear latency are "
                           "measured in task 018 - this number is not a measurement");
    sections.push_back({ "audio", std::move(audioRows) });

    // [latency]: the 018 accounting, row by row, with its kinds and limitations -
    // the same function the screen draws, so the file and the UI cannot disagree.
    std::vector<std::pair<std::string, std::string>> latencyRows;
    for (const auto& row : latencyAccounting(engine, audioBackend_.get(), diag, cfg))
    {
        std::string key = row.component;
        std::replace(key.begin(), key.end(), ' ', '_');
        latencyRows.emplace_back(std::move(key), row.value + " | kind: " + row.kind);
    }
    latencyRows.emplace_back("limitations",
                             "network and model are one combined live-computed backlog (no "
                             "provider-side timestamp to split them); unreported driver latencies "
                             "count as 0.0 in the total; the total is an accounting sum, NOT a "
                             "mouth-to-ear measurement - see docs/latency-budget.md");
    sections.push_back({ "latency", std::move(latencyRows) });

    // [developer]: whether this run is a developer run, in the file itself -
    // a venue report that cannot answer "was that a mock?" is a report that
    // invites exactly the confusion task 019 exists to prevent.
    std::vector<std::pair<std::string, std::string>> developerRows;
    developerRows.emplace_back("mode", devPlan_.enabled ? "DEVELOPER" : "production");
    developerRows.emplace_back("audio_source",
                               devPlan_.useWavSource ? "wav file (simulated device)"
                               : devPlan_.useToneSource ? "test tone (simulated device)"
                                                        : std::string("configured device"));
    developerRows.emplace_back("wav_input", devPlan_.wavInputPath.empty() ? "-" : devPlan_.wavInputPath);
    developerRows.emplace_back("recording", devPlan_.wavOutputPath.empty() ? "-" : devPlan_.wavOutputPath);
    developerRows.emplace_back("translation", devPlan_.mockTranslation
                                                   ? "MOCK ECHO - no provider session was opened"
                                                   : "configured provider chain");
    developerRows.emplace_back("mock_latency_ms", std::to_string(devPlan_.mockLatencyMs));
    developerRows.emplace_back("loopback",
                               devPlan_.loopback ? (loopbackActive()
                                                        ? "ON - capture goes to the output, "
                                                          "translation NOT fed ("
                                                          + std::to_string(loopbackTransferredFrames())
                                                          + " frames moved)"
                                                        : "requested but not running")
                                                 : "off");
    developerRows.emplace_back("notes",
                               devPlan_.notes.empty()
                                   ? "-"
                                   : [this]
                                     {
                                         std::string joined;
                                         for (const auto& note : devPlan_.notes)
                                         {
                                             if (!joined.empty())
                                                 joined += "; ";
                                             joined += note;
                                         }
                                         return joined;
                                     }());
    developerRows.emplace_back("limitations",
                               "the developer plan is a snapshot taken when the application "
                               "started; settings edits restart it, nothing re-mounts mid-run. "
                               "A production export has mode=production and no mock path is "
                               "active in it");
    sections.push_back({ "developer", std::move(developerRows) });

    std::vector<std::pair<std::string, std::string>> translationRows;
    translationRows.emplace_back("languages", cfg.translation.inputLanguage + "->"
                                             + cfg.translation.outputLanguage);
    translationRows.emplace_back("model_hint", cfg.translation.modelHint.empty()
                                                           ? "(backend default)"
                                                           : cfg.translation.modelHint);
    translationRows.emplace_back("instructions", cfg.translation.instructions);
    // The paired truth (code review P1, 2026-10-05): a venue reading the file
    // must know the text was never sent, not reconstruct it from a log line.
    translationRows.emplace_back("instructions_effect",
                                 cfg.translation.instructions.empty()
                                     ? std::string("not set")
                                     : std::string("ignored - the current model accepts no prompting (docs section 12.1)"));
    translationRows.emplace_back("jitter_buffer_ms", std::to_string(cfg.translation.jitterBufferMs));
    translationRows.emplace_back("reconnect_enabled", cfg.translation.reconnectEnabled ? "yes" : "no");
    translationRows.emplace_back("reconnect_initial_backoff_ms",
                                 std::to_string(cfg.translation.reconnectInitialBackoffMs));
    translationRows.emplace_back("reconnect_max_backoff_ms",
                                 std::to_string(cfg.translation.reconnectMaxBackoffMs));
    translationRows.emplace_back("session_max_age_seconds",
                                 std::to_string(cfg.translation.sessionMaxAgeSeconds));
    translationRows.emplace_back("expiry_safety_margin_seconds",
                                 std::to_string(cfg.translation.expirySafetyMarginSeconds));
    translationRows.emplace_back("capture_submitted_frames",
                                 std::to_string(diag.translationSubmittedFrames));
    translationRows.emplace_back("capture_gap_refused_frames", std::to_string(diag.translationGapFrames));
    translationRows.emplace_back("translated_audio_frames", std::to_string(diag.translatedAudioFrames));
    {
        // Live provider fact (protocol docs section 4bis): when the server
        // announced a concrete expiry, the export carries its remaining time at
        // export instant; when it did not, the export says "not announced" in
        // so many words - a reader never has to distinguish absent from zero.
        long long remainingMs = 0;
        translationRows.emplace_back("server_session_expiry",
                                     translationBackend_ != nullptr && translationBackend_->serverSessionExpiryRemainingMs(remainingMs)
                                         ? std::to_string(remainingMs) + " ms remaining"
                                         : std::string("not announced"));
    }
    translationRows.emplace_back("rejected_audio_frames", std::to_string(diag.rejectedAudioFrames));
    translationRows.emplace_back("dropped_audio_frames",
                                 std::to_string(diag.translatedAudioDroppedFrames));
    translationRows.emplace_back("text_partial_events", std::to_string(diag.partialTextEvents));
    translationRows.emplace_back("text_final_events", std::to_string(diag.finalTextEvents));
    translationRows.emplace_back("text_line_evictions", std::to_string(textPipeline_.evictedLines()));
    translationRows.emplace_back("text_duplicate_finals_ignored",
                                 std::to_string(textPipeline_.ignoredDuplicates()));
    translationRows.emplace_back("reconnects", std::to_string(diag.reconnects));
    translationRows.emplace_back("translation_errors", std::to_string(diag.translationErrors));
    translationRows.emplace_back("translation_fatal_errors", std::to_string(diag.translationFatalErrors));
    sections.push_back({ "translation", std::move(translationRows) });

    sections.push_back({ "ndi", {
        { "enabled", cfg.ndi.enabled ? "yes" : "no" },
        { "stream_name", cfg.ndi.streamName },
        { "published_frames", ndiOutput_ != nullptr ? std::to_string(ndiOutput_->publishedFrames())
                                                     : std::string("0") },
        // Task 016 contract, review P1 counters: "errors" is what the PRODUCER
        // was told (not started / shutting down); dropped_frames and
        // transport_errors are what happened after acceptance, on the dispatch
        // side - a post-mortem must be able to tell "we chose to drop stale
        // captions" apart from "the transport refused".
        { "dropped_frames", ndiOutput_ != nullptr ? std::to_string(ndiOutput_->droppedFrames())
                                                   : std::string("0") },
        { "transport_errors", ndiOutput_ != nullptr ? std::to_string(ndiOutput_->publishErrors())
                                                      : std::string("0") },
        { "errors", std::to_string(diag.ndiErrors) },
    } });

    // Security: presence and the store's name. The keys are deliberately not
    // secret-SHAPED words ("key_present", not "api_key_present") - the redactor
    // guards values by key shape and does not know semantics; a present-but-
    // redacted fact would be worse than a renamed one, and this choice was
    // caught live, by this task's own test. There is no value under any key:
    // credential values never enter settings, logs or exports (AGENTS.md 10).
    sections.push_back({ "security", {
        { "store_backend", secretStoreName() },
        { "key_present", hasApiSecret() ? "yes" : "no" },
        { "rule", "credential values never enter settings, logs or exports; presence is the only fact shown" },
    } });

    std::vector<std::pair<std::string, std::string>> settingRows;
    settingRows.emplace_back("schema_version", std::to_string(cfg.schemaVersion));
    settingRows.emplace_back("input_device_id", cfg.audio.inputDeviceId);
    settingRows.emplace_back("output_device_id", cfg.audio.outputDeviceId);
    settingRows.emplace_back("input_channel", std::to_string(cfg.audio.inputChannel));
    settingRows.emplace_back("output_channel", std::to_string(cfg.audio.outputChannel));
    settingRows.emplace_back("log_level", cfg.diagnostics.logLevel);
    settingRows.emplace_back("write_log_file", cfg.diagnostics.writeLogFile ? "yes" : "no");
    if (!diag.lastErrorSubsystem.empty())
    {
        settingRows.emplace_back("last_error_subsystem", diag.lastErrorSubsystem);
        settingRows.emplace_back("last_error_message", diag.lastErrorMessage);
    }
    sections.push_back({ "settings", std::move(settingRows) });

    const auto events = diagnostics_.events();
    const auto rendered = diagnostics::renderExport(appVersion_, sections, events,
                                                    diagnostics_.evictedEvents(),
                                                    &config::isSecretFieldName);

    const auto targetDir = directory.empty() ? diagnosticsDirectory() : directory;

    // Two exports in the same second (an excited double-click) must produce two
    // receipts, not silently overwrite the first one's evidence.
    const std::string base = "lingoflow-diag-" + fileStamp();
    std::filesystem::path file = targetDir / (base + ".txt");
    for (int suffix = 2; std::filesystem::exists(file); ++suffix)
        file = targetDir / (base + "-" + std::to_string(suffix) + ".txt");

    std::string error;
    if (!diagnostics::writeExportFile(file, rendered.text, error))
    {
        note = "diagnostics export failed: " + error;
        log::warning(kComponent, note);
        return false;
    }

    written = file;

    note = "diagnostics written: " + file.string();
    if (rendered.redactions > 0)
        note += " - " + std::to_string(rendered.redactions) + " secret-shaped value(s) were redacted";

    // Said where it belongs: a redaction happened only if a gatherer slipped, and
    // the operator must hear about that from the same action that produced it.
    if (rendered.redactions > 0)
        log::error(kComponent, note);
    else
        log::info(kComponent, note);

    // The event is appended AFTER the write: an export cannot contain news of its
    // own completion; the next one will.
    diagnostics_.noteEvent("diagnostics", "export written to " + file.string());

    return true;
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

        // The operator's "why is the translator off" question gets the same
        // answer here as a mid-run failure: this is the freshest translation
        // truth, and detail is where it lives. (A refused session is still not
        // fatal - audio is running and the state says so.)
        {
            const std::lock_guard lock(subsystemMutex_);
            lastTranslationError_ = "translation session not started: " + error;
        }
    }

    if (!startNdi(error))
    {
        log::warning(kComponent, "NDI output not started: " + error);
        diagnostics_.noteError("ndi", error);
    }

    state_ = ApplicationState::running;
    log::info(kComponent, "running");
    diagnostics_.noteEvent("app", "running (audio backend '" + status().audioBackendName + "')");
    return true;
}

void ApplicationController::stop()
{
    if (state_ == ApplicationState::stopped)
        return;

    state_ = ApplicationState::stopping;
    log::info(kComponent, "stopping");

    // Session first, NDI after: closeSession() may flush the trailing translated
    // line through the sink (task 013), and that text still has an audience only
    // while the subtitle transport is up. Audio goes last, as before - the
    // session teardown joins the streaming worker before the device dies.
    stopSession();
    stopLoopback();
    stopNdi();
    stopAudio();

    faultReason_.clear();
    state_ = ApplicationState::stopped;
    log::info(kComponent, "stopped");
    diagnostics_.noteEvent("app", "stopped");
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

    const bool devSource = devPlan_.useWavSource || devPlan_.useToneSource;

    // A planned simulated source must actually BE the injected backend: the
    // alternative - "planned wav, opened the null device quietly" - is exactly
    // the leaked-mock (or leaked-silence) failure this task must not ship.
    if (devSource && !audioBackendOverridden_)
    {
        error = "the developer plan asks for a simulated audio source, but the "
                "composition root installed no simulated device";
        log::error(kComponent, error);
        return false;
    }

    if (!deviceId.empty())
    {
        if (devSource)
        {
            // The configured device loses to the developer plan visibly, never
            // silently: an operator who sees the tone in the meters and had
            // left a SoundGrid device in settings gets the whole sentence in
            // the log, and the badge on the screen says it too.
            log::info(kComponent, "developer mode runs the simulated source '"
                                      + std::string(audioBackend_->name())
                                      + "': the configured device '" + deviceId
                                      + "' was NOT opened (developer plan)");
        }
        else if (audioBackendFactory_ != nullptr)
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

    // Developer loopback (task 019) claims the input rings before any streaming
    // worker is even considered; startSession() -> startStreaming() checks for
    // it and keeps the translator unfed while the room's own audio plays.
    if (devPlan_.loopback)
        startLoopbackIfNeeded();

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

    // Two consumers cannot own one ring: while the developer loopback plays
    // the capture to the room, the translator is deliberately NOT fed, and
    // that fact goes to the log, the event ring and the badge - it is never
    // a silent half-state (task 005's exclusivity, made visible by 019).
    if (loopbackActive())
    {
        log::info(kComponent,
                  "translation session is open but the developer loopback owns the capture: "
                  "the translator is not being fed");
        diagnostics_.noteEvent("translation", "not streaming: developer loopback owns the input");
        return;
    }

    streamer_ = std::make_unique<TranslationStreamer>(engine_, *translationBackend_, &diagnostics_);

    std::string error;

    if (streamer_->start(error))
    {
        log::info(kComponent,
                  "translation streaming started: device capture at "
                      + std::to_string(engine_.sampleRate()) + " Hz feeds the session");
        diagnostics_.noteEvent("translation", "streaming started");
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
    diagnostics_.noteEvent("translation",
                           "streaming stopped: " + std::to_string(streamer_->submittedFrames())
                               + " submitted, " + std::to_string(streamer_->gapRefusedFrames())
                               + " gap-refused");

    streamer_->stop();
    streamer_.reset();
}

void ApplicationController::startLoopbackIfNeeded()
{
    loopback_ = std::make_unique<audio::AudioLoopback>(engine_);

    std::string error;

    if (!loopback_->start(error))
    {
        // The plan asked for loopback and the worker refused: say it loudly and
        // fall back to the normal streaming (which startStreaming will then
        // happily do, loopbackActive() being false). Audio itself runs either
        // way - loopback is a developer convenience, never an audio-path must.
        log::error(kComponent, "developer loopback refused to start: " + error
                               + " - capture streaming will feed the translator instead");
        diagnostics_.noteError("developer", "loopback not started: " + error);
        loopback_.reset();
        return;
    }

    log::warning(kComponent,
                 "DEVELOPER LOOPBACK ON: capture is routed to the output - the audience hears "
                 "the input, NOT a translation, and the translator is not fed");
    diagnostics_.noteEvent("developer", "loopback running (capture -> output; translation not fed)");
}

void ApplicationController::stopLoopback() noexcept
{
    if (loopback_ == nullptr)
        return;

    log::info(kComponent, "developer loopback stopped after moving "
                              + std::to_string(loopback_->transferredFrames())
                              + " frames from capture to output");
    diagnostics_.noteEvent("developer",
                           "loopback stopped: " + std::to_string(loopback_->transferredFrames())
                               + " frames transferred");

    loopback_->stop();
    loopback_.reset();
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

    diagnostics_.noteEvent("ndi", "started as '" + config_.current().ndi.streamName + "'");

    error.clear();
    return true;
}

void ApplicationController::stopNdi() noexcept
{
    if (ndiOutput_ != nullptr)
        ndiOutput_->stop();
}

void ApplicationController::publishToNdi(const translation::TranslationTextEvent& event)
{
    if (ndiOutput_ == nullptr)
        return;

    if (ndiOutput_->state() == ndi::OutputState::disabled)
        return;   // feature is off: dropping subtitles is expected, not an error

    ndi::SubtitleFrame frame;
    frame.text = event.text;
    frame.final = event.kind == translation::TextKind::final;
    frame.sequence = static_cast<long long>(event.sequence);   // the pipeline's own
                                                               // monotonic identity

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

    // The event ring narrates transitions; the audio/translation noteError calls
    // already recorded the technical cause. This line is the moment the show
    // stopped, which is what a replay asks first.
    diagnostics_.noteEvent("app", "FAULTED: " + faultReason_);
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
    textPipeline_.ingestPartial(text);
}

void ApplicationController::onFinalText(std::string_view text)
{
    diagnostics_.countFinalTextEvent();
    textPipeline_.ingestFinal(text);
}

void ApplicationController::onSessionStateChanged(translation::SessionState state)
{
    diagnostics_.noteEvent("translation", std::string("session: ")
                                              + std::string(translation::nameOf(state)));
    log::info(kComponent, std::string("translation session: ").append(nameOf(state)));

    // A fresh (re)connected session deserves a fresh warning: if the new backend
    // behaves differently the operator must hear about it again, once.
    if (state == translation::SessionState::connected)
    {
        warnedRateMismatch_.store(false, std::memory_order_relaxed);
        warnedNoBuffer_.store(false, std::memory_order_relaxed);
        warnedBadBlock_.store(false, std::memory_order_relaxed);

        // A session that came up clean makes earlier translation refusals
        // history: detail shows the freshest truth, not old grudges. (The log
        // and the counters keep the full story.)
        const std::lock_guard lock(subsystemMutex_);
        lastTranslationError_.clear();
    }
    else if (state == translation::SessionState::reconnecting
             || state == translation::SessionState::faulted
             || state == translation::SessionState::closed)
    {
        // Session boundaries close the open subtitle line: the words the
        // translator got before the line died are history, not a draft that a
        // reopened session will silently replace. Idempotent, and the pipeline's
        // duplicate guard lets a backend's own close-flush through without
        // stamping the same line twice.
        textPipeline_.closeOpenLine();
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
