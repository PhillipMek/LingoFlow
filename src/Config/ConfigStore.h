#pragma once
//
// ConfigStore - reads and writes config.json safely.
//
// Guarantees (SPEC "Configuration", AGENTS.md 11):
//   * loading never throws and never deletes user data: an unusable file is
//     quarantined next to the original, the application continues with defaults or
//     with the last known good backup;
//   * saving is atomic: serialize to <file>.tmp, read it back and re-parse it, only
//     then copy the previous file to <file>.bak and replace the target;
//   * a configuration that fails validation is never written;
//   * a file produced by a newer schema version is refused, not downgraded;
//   * credentials are never part of what is written (see ConfigSchema).

#include <filesystem>
#include <string>

#include "Config/AppConfig.h"
#include "Config/ConfigSchema.h"

namespace liveai {
namespace config {

enum class LoadOutcome
{
    fileAbsent,             ///< no file yet: defaults are used, nothing was written
    loaded,                 ///< file parsed and accepted as-is
    repaired,               ///< file parsed with problems: defaults restored per field
    invalidJson,            ///< file was not usable JSON: quarantined, defaults used
    restoredFromBackup,     ///< main file unusable, <file>.bak restored
    refusedFutureVersion,   ///< schemaVersion newer than this build supports; file untouched
    readFailed,             ///< file exists but could not be read: quarantined, defaults used
};

std::string_view nameOf(LoadOutcome outcome) noexcept;

struct LoadResult
{
    AppConfig config;
    LoadOutcome outcome = LoadOutcome::fileAbsent;
    ConfigProblems problems;
    std::string message;
    std::filesystem::path quarantinedTo;   ///< empty unless the bad file was moved aside
};

class ConfigStore
{
public:
    explicit ConfigStore(std::filesystem::path file);

    const std::filesystem::path& file() const noexcept { return file_; }
    std::filesystem::path backupFile() const;
    std::filesystem::path temporaryFile() const;

    /// Never throws. `problems` are reported per field; the returned configuration
    /// is always usable.
    LoadResult load() const;

    /// Atomic save. Returns false with a message when the configuration is invalid
    /// or the filesystem refused the operation; the previous file stays intact in
    /// both cases.
    bool save(const AppConfig& settings, std::string& error);

    /// <user AppData>/Live AI Interpreter/config.json on Windows,
    /// ~/.liveai/config.json elsewhere. Never creates the directory here.
    static std::filesystem::path defaultFile();

    /// Application folder name used for the config location.
    static std::string_view applicationDirectoryName() noexcept { return "Live AI Interpreter"; }

private:
    std::filesystem::path quarantineUnreadable(const std::filesystem::path& source) const;

    std::filesystem::path file_;
};

} // namespace config
} // namespace liveai
