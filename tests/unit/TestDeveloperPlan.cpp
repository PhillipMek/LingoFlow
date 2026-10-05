#include <catch2/catch_test_macros.hpp>

#include <string>

#include "App/DeveloperMode.h"
#include "App/UiModel.h"
#include "Config/ConfigSchema.h"
#include "Utils/Log.h"

using namespace liveai;

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

} // namespace

TEST_CASE("Developer plan: a production configuration plans NOTHING", "[app][developer][isolation]")
{
    // The FAIL criterion of task 019 is "mock behaviour leaks into production".
    // This case is that criterion as executable: the shipped defaults produce a
    // plan whose every field says off and whose badge says nothing at all.
    const auto cfg = config::defaults();

    const auto plan = developerPlan(cfg);

    CHECK_FALSE(plan.enabled);
    CHECK_FALSE(plan.useWavSource);
    CHECK_FALSE(plan.useToneSource);
    CHECK_FALSE(plan.mockTranslation);
    CHECK_FALSE(plan.loopback);
    CHECK_FALSE(plan.captureStreamOff);
    CHECK(plan.wavInputPath.empty());
    CHECK(plan.wavOutputPath.empty());
    CHECK(plan.badge.empty());
    CHECK(plan.notes.empty());
}

TEST_CASE("Developer plan: with the switch off, leftover fields are inert", "[app][developer][isolation]")
{
    // A config file from a rehearsal whose `enabled` went back to false must
    // not resurrect the mock or - worst of all - the loopback. The mode is one
    // gate, and nothing else gets a vote.
    AppConfig cfg = config::defaults();
    cfg.developer.mockTranslation = true;
    cfg.developer.loopback = true;
    cfg.developer.audioSource = "tone";

    const auto plan = developerPlan(cfg);

    CHECK_FALSE(plan.enabled);
    CHECK_FALSE(plan.mockTranslation);
    CHECK_FALSE(plan.loopback);
    CHECK_FALSE(plan.useToneSource);
    CHECK(plan.badge.empty());
}

TEST_CASE("Developer plan: the --dev preset is useful offline and never routes the room to itself",
          "[app][developer]")
{
    const auto cfg = config::defaults();
    const auto plan = developerPlan(cfg, /*forcedByCommandLine = */ true);

    CHECK(plan.enabled);
    CHECK(plan.useToneSource);
    CHECK_FALSE(plan.useWavSource);
    CHECK(plan.mockTranslation);

    // The one thing a flag must never do: play the capture to the audience.
    CHECK_FALSE(plan.loopback);
    CHECK_FALSE(plan.captureStreamOff);

    CHECK(plan.badge.find("DEVELOPER MODE") != std::string::npos);
    CHECK(plan.badge.find("tone") != std::string::npos);
    CHECK(plan.badge.find("OpenAI was NOT called") != std::string::npos);

    // And the forcing is honest about itself: the file was not touched.
    bool saysSo = false;

    for (const auto& note : plan.notes)
        saysSo = saysSo || note.find("--dev preset") != std::string::npos;

    CHECK(saysSo);
}

TEST_CASE("Developer plan: a configured developer run carries every wish and names every part",
          "[app][developer]")
{
    AppConfig cfg = config::defaults();
    cfg.developer.enabled = true;
    cfg.developer.audioSource = "wav";
    cfg.developer.wavInputPath = "D:\\rehearsal\\input.wav";
    cfg.developer.wavOutputPath = "D:\\rehearsal\\record.wav";
    cfg.developer.mockTranslation = true;
    cfg.developer.mockLatencyMs = 800;
    cfg.developer.loopback = true;

    const auto plan = developerPlan(cfg);

    CHECK(plan.enabled);
    CHECK(plan.useWavSource);
    CHECK(plan.wavInputPath == "D:\\rehearsal\\input.wav");
    CHECK(plan.wavOutputPath == "D:\\rehearsal\\record.wav");
    CHECK(plan.mockTranslation);
    CHECK(plan.mockLatencyMs == 800);
    CHECK(plan.loopback);

    // Two consumers of one ring: the plan states the consequence, visibly.
    CHECK(plan.captureStreamOff);

    CHECK(plan.badge.find("DEVELOPER MODE") != std::string::npos);
    CHECK(plan.badge.find("WAV 'D:\\rehearsal\\input.wav'") != std::string::npos);
    CHECK(plan.badge.find("no device was opened") != std::string::npos);
    CHECK(plan.badge.find("recording output to 'D:\\rehearsal\\record.wav'") != std::string::npos);
    CHECK(plan.badge.find("mock echo +800 ms") != std::string::npos);
    CHECK(plan.badge.find("LOOPBACK") != std::string::npos);
    CHECK(plan.badge.find("NOT fed") != std::string::npos);
}

TEST_CASE("Developer plan: incoherent wishes degrade loudly, not silently", "[app][developer]")
{
    // "wav" naming no file is caught by the schema at load, but the plan is
    // the last authority before mounting, so it holds the line too - and says
    // out loud which story it ended up planning.
    AppConfig cfg = config::defaults();
    cfg.developer.enabled = true;
    cfg.developer.audioSource = "wav";
    cfg.developer.wavInputPath.clear();

    const auto plan = developerPlan(cfg);

    CHECK(plan.enabled);
    CHECK_FALSE(plan.useWavSource);
    CHECK(plan.badge.find("real device") != std::string::npos);
    REQUIRE_FALSE(plan.notes.empty());
    CHECK(plan.notes[0].find("falling back") != std::string::npos);
    CHECK(plan.badge.find(plan.notes[0]) != std::string::npos);   // the note rides the badge
}

TEST_CASE("Developer plan: the panel badge is the plan plus the worker's truth",
          "[app][developer][ui]")
{
    QuietLog quiet;

    ApplicationController controller;
    const auto panel = buildOperatorPanel(controller, {});
    CHECK(panel.developerBadge.empty());   // default plan = production = no badge

    AppConfig cfg = config::defaults();
    cfg.developer.enabled = true;
    cfg.developer.audioSource = "tone";
    cfg.developer.loopback = true;
    cfg.developer.mockTranslation = true;

    controller.setDeveloperPlan(developerPlan(cfg));

    const auto devPanel = buildOperatorPanel(controller, {});
    CHECK_FALSE(devPanel.developerBadge.empty());
    CHECK(devPanel.developerBadge.find("DEVELOPER MODE") != std::string::npos);

    // The plan asked for loopback; no pipeline ever started, so the badge must
    // say NOT running rather than repeating the wish as if it were the fact.
    CHECK(devPanel.developerBadge.find("loopback NOT running") != std::string::npos);
}
