#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "App/ApplicationController.h"
#include "App/DeveloperMode.h"
#include "App/UiModel.h"
#include "Audio/Dev/SimulatedDeviceBackend.h"
#include "Config/ConfigSchema.h"
#include "Translation/Mock/MockTranslationBackend.h"
#include "Utils/Log.h"
#include "Utils/WavFile.h"

using namespace liveai;
using namespace std::chrono_literals;

namespace {

struct QuietLog
{
    QuietLog()
    {
        LogConfig cfg;
        cfg.level = LogLevel::off;
        cfg.writeConsole = false;
        log::configure(cfg);
    }
    ~QuietLog() { log::resetForTests(); }
};

std::filesystem::path makeTempRoot(const char* name)
{
    const auto root = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    return root;
}

/// Poll a predicate until it holds or the deadline passes - the tests below
/// watch a live paced pipeline, and "gave up after N seconds, say so" is the
/// honest form of a timing assertion.
template <typename Predicate>
bool waitFor(Predicate ready, std::chrono::milliseconds limit = 8s)
{
    const auto deadline = std::chrono::steady_clock::now() + limit;

    while (std::chrono::steady_clock::now() < deadline)
    {
        if (ready())
            return true;

        std::this_thread::sleep_for(50ms);
    }

    return false;
}

/// A 48 kHz mono second of sine for the file-driven cases; built through the
/// product's own writer so a failure cannot hide in a hand-made fixture.
void writeTestWav(const std::filesystem::path& path)
{
    wavio::Mono16Writer writer;
    std::string error;
    REQUIRE(writer.open(path.string(), 48000, error));

    std::vector<float> block(480);

    for (int repetition = 0; repetition < 100; ++repetition)
    {
        for (int i = 0; i < 480; ++i)
        {
            const double t = (repetition * 480.0 + i) / 48000.0;
            block[i] = 0.5f * static_cast<float>(std::sin(2.0 * 3.14159265358979 * 440.0 * t));
        }

        writer.write(block.data(), block.size());
    }

    REQUIRE(writer.close(error));
}

float peakOf(const wavio::WavAudio& audio)
{
    float peak = 0.0f;

    for (const float sample : audio.samples)
        peak = std::max(peak, std::abs(sample));

    return peak;
}

} // namespace

TEST_CASE("Offline core: WAV in, mock echo, WAV out - no ASIO device, no API key, no network",
          "[integration][developer][offline]")
{
    QuietLog quiet;
    const auto root = makeTempRoot("lingoflow-019-offline");

    const auto configFile = root / "config.json";
    const auto inFile = root / "in.wav";
    const auto outFile = root / "out.wav";

    writeTestWav(inFile);

    AppConfig cfg = config::defaults();
    cfg.developer.enabled = true;
    cfg.developer.audioSource = "wav";
    cfg.developer.wavInputPath = inFile.string();
    cfg.developer.wavOutputPath = outFile.string();
    cfg.developer.mockTranslation = true;
    cfg.developer.mockLatencyMs = 40;

    // Write the settings so the controller loads a developer configuration the
    // same way Main.cpp will; the PLAN derived from it is the very object the
    // composition root mounts from.
    {
        std::ofstream out(configFile);
        out << config::toJsonText(cfg);
    }

    ApplicationController controller;

    std::string loadNote;
    REQUIRE(controller.loadSettings(configFile, loadNote));

    const DeveloperPlan plan = developerPlan(controller.config().current());
    REQUIRE(plan.enabled);
    REQUIRE(plan.useWavSource);
    REQUIRE(plan.mockTranslation);

    controller.setDeveloperPlan(plan);
    controller.setAudioBackend(std::make_unique<audio::WavFileAudioBackend>(
        plan.wavInputPath, plan.wavOutputPath));

    translation::MockTranslationBackend::Options options;
    options.latencyMs = plan.mockLatencyMs;
    controller.setTranslationBackend(
        std::make_unique<translation::MockTranslationBackend>(std::move(options)));

    REQUIRE(controller.start());

    CHECK(controller.engine().sampleRate() == 48000);   // the file's rate, as asked
    CHECK(controller.status().session == translation::SessionState::connected);
    CHECK(controller.translationStreamer() != nullptr); // the translator IS fed here

    const bool echoed = waitFor([&]
                                {
                                    // Past the 250 ms pre-roll AND into real
                                    // playback: ~700 ms of echoed audio. Anything
                                    // less and the recorder would legitimately
                                    // hold only pre-roll silence - which is the
                                    // engine working correctly, not a failure.
                                    return controller.diagnostics().snapshot().translatedAudioFrames > 33600;
                                });

    INFO("translatedAudioFrames=" << controller.diagnostics().snapshot().translatedAudioFrames);
    REQUIRE(echoed);

    // The badge and the export both say DEVELOPER while it runs.
    const auto panel = buildOperatorPanel(controller, {});
    CHECK(panel.developerBadge.find("DEVELOPER MODE") != std::string::npos);
    CHECK(panel.developerBadge.find("mock echo") != std::string::npos);

    controller.stop();

    // The receipt: what the simulated audience "heard" is on disk and audible -
    // the input went through rings, the streaming worker, the mock queue, the
    // sink checks, the jitter pre-roll, the output blocks and the recorder.
    wavio::WavAudio recorded;
    std::string error;
    REQUIRE(wavio::readMono(outFile.string(), recorded, error));
    CHECK(recorded.sampleRate == 48000);
    CHECK(recorded.samples.size() > 4800);
    CHECK(peakOf(recorded) > 0.2f);   // the echo of a 0.5-peak sine at unity gains
}

TEST_CASE("Offline core: loopback plays the capture and keeps the translator unfed, loudly",
          "[integration][developer][loopback]")
{
    QuietLog quiet;
    const auto root = makeTempRoot("lingoflow-019-loopback");

    const auto configFile = root / "config.json";

    AppConfig cfg = config::defaults();
    cfg.developer.enabled = true;
    cfg.developer.audioSource = "tone";
    cfg.developer.mockTranslation = true;
    cfg.developer.loopback = true;   // the settings path; --dev would not set this

    {
        std::ofstream out(configFile);
        out << config::toJsonText(cfg);
    }

    ApplicationController controller;

    std::string loadNote;
    REQUIRE(controller.loadSettings(configFile, loadNote));

    const DeveloperPlan plan = developerPlan(controller.config().current());
    REQUIRE(plan.loopback);

    controller.setDeveloperPlan(plan);
    controller.setAudioBackend(std::make_unique<audio::TestToneAudioBackend>(
        plan.toneFrequencyHz, plan.toneLevelDb));

    translation::MockTranslationBackend::Options options;
    options.latencyMs = 40;
    controller.setTranslationBackend(
        std::make_unique<translation::MockTranslationBackend>(std::move(options)));

    REQUIRE(controller.start());

    // The loopback worker owns the rings: it moves audio, and the streaming
    // worker deliberately does not exist (two consumers, one ring, no silent
    // arbitration - task 019 made the choice visible).
    const bool moving = waitFor([&]
                                {
                                    return controller.loopbackActive()
                                           && controller.loopbackTransferredFrames() > 0;
                                });

    REQUIRE(moving);
    CHECK(controller.translationStreamer() == nullptr);
    CHECK(controller.status().session == translation::SessionState::connected);

    const auto panel = buildOperatorPanel(controller, {});
    CHECK(panel.developerBadge.find("LOOPBACK") != std::string::npos);
    CHECK(panel.developerBadge.find("loopback worker RUNNING") != std::string::npos);

    controller.stop();
    CHECK_FALSE(controller.loopbackActive());
}

TEST_CASE("Offline core: the production controller says so in the export, field by field",
          "[integration][developer][isolation]")
{
    QuietLog quiet;
    const auto root = makeTempRoot("lingoflow-019-production");

    ApplicationController controller;

    std::string loadNote;
    REQUIRE(controller.loadSettings(root / "config.json", loadNote));

    // No plan was ever installed: that is the production story, and it needs
    // no special casing anywhere - DeveloperPlan{} simply says "nothing".
    REQUIRE(controller.start());

    std::filesystem::path written;
    std::string note;
    REQUIRE(controller.exportDiagnostics(root, written, note));

    std::ifstream in(written);
    const std::string report((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();   // Windows will not let the temp dir be reused while the file is held

    CHECK(report.find("[developer]") != std::string::npos);
    CHECK(report.find("mode=production") != std::string::npos);
    CHECK(report.find("translation=configured provider chain") != std::string::npos);
    CHECK(report.find("loopback=off") != std::string::npos);

    controller.stop();
}
