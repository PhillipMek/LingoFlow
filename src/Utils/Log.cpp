#include "Utils/Log.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>

namespace liveai {
namespace {

struct LogState
{
    std::mutex mutex;
    LogConfig config;
    std::uint64_t lines = 0;
    std::ofstream file;
};

LogState& state()
{
    static LogState s;
    return s;
}

std::string upperName(LogLevel level)
{
    auto name = log::nameOf(level);
    std::string out(name);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return out;
}

std::string currentTimestamp()
{
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    const std::time_t tt = system_clock::to_time_t(now);

    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif

    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << '.' << std::setfill('0') << std::setw(3) << ms.count();
    return oss.str();
}

bool openFile(LogState& s)
{
    if (s.config.filePath.empty())
        return false;

    if (s.file.is_open())
        s.file.close();

    std::error_code ec;
    const std::filesystem::path path(s.config.filePath);
    if (path.has_parent_path())
        std::filesystem::create_directories(path.parent_path(), ec);

    s.file.open(s.config.filePath, std::ios::out | std::ios::app);
    return s.file.is_open();
}

} // namespace

namespace log {

void configure(const LogConfig& config)
{
    auto& s = state();
    std::scoped_lock lock(s.mutex);
    s.config = config;
    openFile(s);
}

LogConfig config()
{
    auto& s = state();
    std::scoped_lock lock(s.mutex);
    return s.config;
}

std::string_view nameOf(LogLevel level)
{
    switch (level)
    {
        case LogLevel::trace:    return "trace";
        case LogLevel::debug:    return "debug";
        case LogLevel::info:     return "info";
        case LogLevel::warning:  return "warning";
        case LogLevel::error:    return "error";
        case LogLevel::critical: return "critical";
        case LogLevel::off:      return "off";
    }
    return "off";
}

LogLevel levelFromName(std::string_view name)
{
    std::string lower(name);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (lower == "trace")    return LogLevel::trace;
    if (lower == "debug")    return LogLevel::debug;
    if (lower == "info")     return LogLevel::info;
    if (lower == "warning")  return LogLevel::warning;
    if (lower == "error")    return LogLevel::error;
    if (lower == "critical") return LogLevel::critical;
    return LogLevel::off;
}

const std::vector<LogLevel>& allLevels()
{
    static const std::vector<LogLevel> levels {
        LogLevel::trace, LogLevel::debug, LogLevel::info, LogLevel::warning,
        LogLevel::error, LogLevel::critical, LogLevel::off
    };
    return levels;
}

bool enabled(LogLevel level)
{
    auto& s = state();
    std::scoped_lock lock(s.mutex);
    return level >= s.config.level && s.config.level != LogLevel::off;
}

std::string formatLine(LogLevel level,
                       std::string_view component,
                       std::string_view message,
                       std::string_view timestamp)
{
    if (component.empty())
        component = "core";

    std::string out;
    out.reserve(timestamp.size() + component.size() + message.size() + 24);
    out.append(timestamp);
    out.append(" [");
    out.append(upperName(level));
    out.append("] [");
    out.append(component);
    out.append("] ");
    out.append(message);
    return out;
}

void write(LogLevel level, std::string_view component, std::string_view message)
{
    auto& s = state();
    std::scoped_lock lock(s.mutex);

    if (level < s.config.level || s.config.level == LogLevel::off)
        return;

    const auto line = formatLine(level, component, message, currentTimestamp());

    if (s.config.writeConsole)
    {
        if (level >= LogLevel::error)
            std::cerr << line << '\n';
        else
            std::cout << line << '\n';
    }

    if (s.file.is_open())
    {
        s.file << line << '\n';
        s.file.flush();
    }

    ++s.lines;
}

std::uint64_t linesWritten()
{
    auto& s = state();
    std::scoped_lock lock(s.mutex);
    return s.lines;
}

void resetForTests()
{
    auto& s = state();
    std::scoped_lock lock(s.mutex);
    if (s.file.is_open())
        s.file.close();
    s.config = LogConfig{};
    s.lines = 0;
}

} // namespace log
} // namespace liveai
