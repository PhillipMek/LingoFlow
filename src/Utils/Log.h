#pragma once
//
// Logging skeleton.
//
// Rules that matter for this product:
//   * NEVER call any function here from the audio callback: write() allocates a
//     formatted string and takes a mutex (AGENTS.md section 5).
//   * NEVER log credentials, tokens or full configuration values
//     (AGENTS.md section 10). Diagnostics export must stay secret-free.
//   * The core library deliberately has no JUCE dependency so it stays
//     portable and unit-testable.
//
// Output goes to the console and/or a file; both are optional. Task 017
// (diagnostics) can attach additional sinks without changing call sites.

#include <cstdint>
#include <string>
#include <string_view>

namespace liveai {

enum class LogLevel
{
    trace = 0,
    debug,
    info,
    warning,
    error,
    critical,
    off
};

struct LogConfig
{
    LogLevel level = LogLevel::info;
    bool writeConsole = true;
    std::string filePath;   // empty = no file output
};

namespace log
{
    /// Must be called from a non-realtime thread. Reconfiguring replaces the
    /// current sinks; it is safe to call more than once.
    void configure(const LogConfig& config);

    /// Current effective configuration (thread-safe snapshot).
    LogConfig config();

    /// Level name in lower case, e.g. "info".
    std::string_view nameOf(LogLevel level);

    /// Parses "trace".."off", case-insensitively; returns off on unknown input.
    LogLevel levelFromName(std::string_view name);

    /// True if messages of the given level would be emitted.
    bool enabled(LogLevel level);

    /// Formats "<timestamp> [<LEVEL>] [<component>] <message>".
    /// Exposed for tests: the timestamp is passed in so output is deterministic.
    std::string formatLine(LogLevel level,
                           std::string_view component,
                           std::string_view message,
                           std::string_view timestamp);

    /// Emits one record to every enabled sink. Never call from the audio thread.
    void write(LogLevel level, std::string_view component, std::string_view message);

    /// Number of records emitted since startup (for diagnostics/tests).
    std::uint64_t linesWritten();

    /// Resets counters and closes file output. Intended for tests.
    void resetForTests();

    inline void trace(std::string_view component, std::string_view message)
    {
        write(LogLevel::trace, component, message);
    }
    inline void debug(std::string_view component, std::string_view message)
    {
        write(LogLevel::debug, component, message);
    }
    inline void info(std::string_view component, std::string_view message)
    {
        write(LogLevel::info, component, message);
    }
    inline void warning(std::string_view component, std::string_view message)
    {
        write(LogLevel::warning, component, message);
    }
    inline void error(std::string_view component, std::string_view message)
    {
        write(LogLevel::error, component, message);
    }
    inline void critical(std::string_view component, std::string_view message)
    {
        write(LogLevel::critical, component, message);
    }
} // namespace log
} // namespace liveai
