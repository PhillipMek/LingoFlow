#include <catch2/catch_test_macros.hpp>

#include "Config/ConfigManager.h"

using liveai::AppConfig;
using liveai::ConfigManager;
using liveai::IConfigListener;

namespace {

class RecordingListener final : public IConfigListener
{
public:
    void onConfigChanged(const AppConfig& updated) override
    {
        ++calls;
        lastSampleRate = updated.sampleRate;
    }

    int calls = 0;
    int lastSampleRate = 0;
};

} // namespace

TEST_CASE("ConfigManager: defaults are valid", "[config]")
{
    ConfigManager config;
    std::string error;

    CHECK(ConfigManager::validate(config.current(), error));
    CHECK(error.empty());
    CHECK(config.current().sampleRate == 48000);
    CHECK(config.current().bufferFrames == 480);
    CHECK(config.current().inputLanguage == "en");
    CHECK(config.current().outputLanguage == "ru");
    CHECK_FALSE(config.current().ndiEnabled);
}

TEST_CASE("ConfigManager: default interpreter instructions carry the SPEC rules", "[config]")
{
    ConfigManager config;
    const auto& instructions = config.current().interpreterInstructions;

    INFO(instructions);
    CHECK(instructions.find("meaning") != std::string::npos);
    CHECK(instructions.find("names") != std::string::npos);
    CHECK(instructions.find("summarize") != std::string::npos);
    CHECK(instructions.find("latency") != std::string::npos);
}

TEST_CASE("ConfigManager: rejects out-of-range values and leaves settings untouched", "[config]")
{
    ConfigManager config;
    std::string error;

    auto bad = config.current();
    bad.sampleRate = 12345;
    CHECK_FALSE(config.update(bad, error));
    CHECK_FALSE(error.empty());
    CHECK(config.current().sampleRate == 48000);

    bad = config.current();
    bad.sampleRate = 48000;
    bad.bufferFrames = 8;                       // below the minimum
    CHECK_FALSE(config.update(bad, error));
    CHECK(error.find("buffer size") != std::string::npos);

    bad.bufferFrames = 480;
    bad.inputGainDb = 40.0f;                    // above the maximum
    CHECK_FALSE(config.update(bad, error));
    CHECK(error.find("gain") != std::string::npos);

    bad.inputGainDb = 0.0f;
    bad.outputLanguage = bad.inputLanguage;     // same language pair
    CHECK_FALSE(config.update(bad, error));
    CHECK(error.find("differ") != std::string::npos);

    bad.outputLanguage = "ru";
    bad.jitterBufferMs = 5000;                  // absurd jitter target
    CHECK_FALSE(config.update(bad, error));
    CHECK(error.find("jitter") != std::string::npos);
}

TEST_CASE("ConfigManager: NDI requires a stream name when enabled", "[config]")
{
    ConfigManager config;
    std::string error;

    auto candidate = config.current();
    candidate.ndiEnabled = true;
    candidate.ndiStreamName.clear();

    CHECK_FALSE(config.update(candidate, error));
    CHECK(error.find("NDI") != std::string::npos);

    candidate.ndiStreamName = "LiveAI EN->RU";
    CHECK(config.update(candidate, error));
    CHECK(config.current().ndiEnabled);
}

TEST_CASE("ConfigManager: listeners are notified only after a successful update", "[config]")
{
    ConfigManager config;
    RecordingListener listener;
    config.addListener(listener);
    std::string error;

    auto bad = config.current();
    bad.bufferFrames = 99999;
    CHECK_FALSE(config.update(bad, error));
    CHECK(listener.calls == 0);

    auto good = config.current();
    good.bufferFrames = 960;
    good.sampleRate = 96000;
    CHECK(config.update(good, error));
    CHECK(listener.calls == 1);
    CHECK(listener.lastSampleRate == 96000);
    CHECK(config.current().bufferFrames == 960);
}
