#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>

#include "Utils/Log.h"

using liveai::LogConfig;
using liveai::LogLevel;

namespace {

LogConfig silentFileConfig(const std::filesystem::path& file, LogLevel level = LogLevel::info)
{
    LogConfig cfg;
    cfg.level = level;
    cfg.writeConsole = false;
    cfg.filePath = file.string();
    return cfg;
}

} // namespace

TEST_CASE("Log: level names round-trip", "[log]")
{
    liveai::log::resetForTests();

    CHECK(liveai::log::nameOf(LogLevel::warning) == "warning");
    CHECK(liveai::log::levelFromName("warning") == LogLevel::warning);
    CHECK(liveai::log::levelFromName("ERROR") == LogLevel::error);
    CHECK(liveai::log::levelFromName("nonsense") == LogLevel::off);
}

TEST_CASE("Log: formatLine is stable and includes level, component, message", "[log]")
{
    liveai::log::resetForTests();

    const auto line = liveai::log::formatLine(LogLevel::info, "audio", "engine started", "2026-10-01 12:00:00.500");

    CHECK(line == "2026-10-01 12:00:00.500 [INFO] [audio] engine started");

    // Empty component falls back to "core" so records stay parseable.
    CHECK(liveai::log::formatLine(LogLevel::error, "", "boom", "T") == "T [ERROR] [core] boom");
}

TEST_CASE("Log: level filter suppresses quieter records", "[log]")
{
    liveai::log::resetForTests();

    const auto file = std::filesystem::temp_directory_path() / "liveai_log_filter_test.log";
    std::error_code removeEc;
    std::filesystem::remove(file, removeEc);

    liveai::log::configure(silentFileConfig(file, LogLevel::warning));

    CHECK_FALSE(liveai::log::enabled(LogLevel::info));
    CHECK(liveai::log::enabled(LogLevel::error));

    liveai::log::info("test", "must not be written");
    liveai::log::warning("test", "must be written");

    CHECK(liveai::log::linesWritten() == 1);

    std::ifstream in(file);
    const std::string contents{ std::istreambuf_iterator<char>{ in }, std::istreambuf_iterator<char>{} };
    in.close();
    CHECK(contents.find("must be written") != std::string::npos);
    CHECK(contents.find("must not be written") == std::string::npos);

    // The logger owns the file handle; closing it first is what makes the
    // removal possible on Windows.
    liveai::log::resetForTests();
    std::error_code ec;
    std::filesystem::remove(file, ec);
    CHECK_FALSE(static_cast<bool>(ec));
}

TEST_CASE("Log: logging disabled entirely emits nothing", "[log]")
{
    liveai::log::resetForTests();

    LogConfig cfg;
    cfg.level = LogLevel::off;
    cfg.writeConsole = false;
    liveai::log::configure(cfg);

    liveai::log::critical("test", "ignored");
    CHECK(liveai::log::linesWritten() == 0);

    liveai::log::resetForTests();
}
