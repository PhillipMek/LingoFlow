#include <catch2/catch_test_macros.hpp>

#include "App/ApplicationController.h"
#include "Utils/Log.h"

using liveai::ApplicationController;
using liveai::ApplicationState;

namespace {

struct QuietLog
{
    QuietLog()
    {
        liveai::LogConfig cfg;
        cfg.level = liveai::LogLevel::off;
        cfg.writeConsole = false;
        liveai::log::configure(cfg);
    }

    ~QuietLog() { liveai::log::resetForTests(); }
};

} // namespace

TEST_CASE("ApplicationController: state names are stable", "[app]")
{
    CHECK(liveai::nameOf(ApplicationState::stopped) == "stopped");
    CHECK(liveai::nameOf(ApplicationState::running) == "running");
    CHECK(liveai::nameOf(ApplicationState::faulted) == "faulted");
}

TEST_CASE("ApplicationController: start brings the app to running", "[app]")
{
    QuietLog quiet;
    ApplicationController controller;

    REQUIRE(controller.state() == ApplicationState::stopped);
    CHECK(controller.start());
    CHECK(controller.state() == ApplicationState::running);
    CHECK(controller.isRunning());
}

TEST_CASE("ApplicationController: repeated start is rejected, state unchanged", "[app]")
{
    QuietLog quiet;
    ApplicationController controller;

    REQUIRE(controller.start());
    CHECK_FALSE(controller.start());
    CHECK(controller.state() == ApplicationState::running);
}

TEST_CASE("ApplicationController: stop returns to stopped and is idempotent", "[app]")
{
    QuietLog quiet;
    ApplicationController controller;

    REQUIRE(controller.start());
    controller.stop();
    CHECK(controller.state() == ApplicationState::stopped);
    CHECK_FALSE(controller.isRunning());

    controller.stop();
    CHECK(controller.state() == ApplicationState::stopped);
}

TEST_CASE("ApplicationController: start->stop->start works", "[app]")
{
    QuietLog quiet;
    ApplicationController controller;

    REQUIRE(controller.start());
    controller.stop();
    CHECK(controller.start());
    CHECK(controller.state() == ApplicationState::running);
}
