#include "Config/ConfigStore.h"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string_view>
#include <system_error>

namespace liveai {
namespace config {
namespace {

std::string readWholeFile(const std::filesystem::path& path, bool& ok)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        ok = false;
        return {};
    }

    std::ostringstream buffer;
    buffer << in.rdbuf();
    ok = static_cast<bool>(in) || in.eof();
    return buffer.str();
}

std::int64_t unixTimestampNow()
{
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

/// Portable environment lookup. MSVC deprecates std::getenv (C4996), so it uses
/// _dupenv_s there; no warnings are disabled anywhere.
std::string environmentVariable(const char* name)
{
#if defined(_WIN32)
    char* buffer = nullptr;
    std::size_t size = 0;

    if (_dupenv_s(&buffer, &size, name) != 0 || buffer == nullptr)
        return {};

    std::string value(buffer);
    std::free(buffer);
    return value;
#else
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string();
#endif
}

} // namespace

std::string_view nameOf(LoadOutcome outcome) noexcept
{
    switch (outcome)
    {
        case LoadOutcome::fileAbsent:          return "file-absent";
        case LoadOutcome::loaded:              return "loaded";
        case LoadOutcome::repaired:            return "repaired";
        case LoadOutcome::invalidJson:         return "invalid-json";
        case LoadOutcome::restoredFromBackup:  return "restored-from-backup";
        case LoadOutcome::refusedFutureVersion:return "refused-future-version";
        case LoadOutcome::readFailed:          return "read-failed";
    }
    return "read-failed";
}

ConfigStore::ConfigStore(std::filesystem::path file)
    : file_(std::move(file))
{
}

std::filesystem::path ConfigStore::backupFile() const
{
    auto path = file_;
    path += ".bak";
    return path;
}

std::filesystem::path ConfigStore::temporaryFile() const
{
    auto path = file_;
    path += ".tmp";
    return path;
}

std::filesystem::path ConfigStore::quarantineUnreadable(const std::filesystem::path& source) const
{
    std::filesystem::path destination = source.string() + ".corrupt-" + std::to_string(unixTimestampNow());

    std::error_code ec;
    std::filesystem::rename(source, destination, ec);

    if (ec)
        return {};   // could not move it aside; the caller still keeps working with defaults

    return destination;
}

LoadResult ConfigStore::load() const
{
    LoadResult result;
    result.config = defaults();

    std::error_code ec;
    if (!std::filesystem::exists(file_, ec))
    {
        result.outcome = LoadOutcome::fileAbsent;
        result.message = "no configuration file at " + file_.string() + ", defaults are used";
        return result;
    }

    bool readOk = false;
    const std::string text = readWholeFile(file_, readOk);

    if (!readOk)
    {
        result.outcome = LoadOutcome::readFailed;
        result.quarantinedTo = quarantineUnreadable(file_);
        result.message = "configuration file could not be read";
        if (!result.quarantinedTo.empty())
            result.message += ", moved to " + result.quarantinedTo.string();
        return result;
    }

    const auto version = schemaVersionOf(text);

    if (version > kConfigSchemaVersion)
    {
        // A newer file is valid data we simply cannot understand yet: never rewrite
        // or move it, run with defaults and let the operator decide.
        result.outcome = LoadOutcome::refusedFutureVersion;
        result.message = "configuration schema version " + std::to_string(version)
                       + " is newer than this build supports (" + std::to_string(kConfigSchemaVersion)
                       + "), file left untouched, defaults are used";
        return result;
    }

    ConfigProblems problems;
    std::string error;
    AppConfig parsed;

    if (fromJsonText(text, parsed, problems, error))
    {
        if (version == 0)
        {
            problems.push_back(ConfigProblem{ "schemaVersion", "field was missing, assumed version 0 and migrated" });
            parsed.schemaVersion = kConfigSchemaVersion;
        }

        result.config = parsed;
        result.problems = problems;
        result.outcome = problems.empty() ? LoadOutcome::loaded : LoadOutcome::repaired;
        result.message = problems.empty()
                             ? "configuration loaded from " + file_.string()
                             : "configuration loaded with " + std::to_string(problems.size()) + " field problem(s) repaired";
        return result;
    }

    // Main file is unusable: quarantine it and try the last known good copy.
    result.outcome = LoadOutcome::invalidJson;
    result.quarantinedTo = quarantineUnreadable(file_);
    result.message = "configuration is not usable JSON: " + error;
    if (!result.quarantinedTo.empty())
        result.message += ", original moved to " + result.quarantinedTo.string();

    std::error_code backupEc;
    const auto backup = backupFile();

    if (std::filesystem::exists(backup, backupEc))
    {
        bool backupReadOk = false;
        const std::string backupText = readWholeFile(backup, backupReadOk);

        if (backupReadOk)
        {
            ConfigProblems backupProblems;
            std::string backupError;
            AppConfig backupConfig;

            if (fromJsonText(backupText, backupConfig, backupProblems, backupError))
            {
                result.config = backupConfig;
                result.problems = backupProblems;
                result.outcome = LoadOutcome::restoredFromBackup;
                result.message = "configuration restored from " + backup.string();
                return result;
            }
        }
    }

    result.message += ", defaults are used";
    return result;
}

bool ConfigStore::save(const AppConfig& settings, std::string& error)
{
    error.clear();

    if (const auto problems = validate(settings); !problems.empty())
    {
        for (const auto& problem : problems)
        {
            if (!error.empty())
                error += "; ";
            error += problem.field + ": " + problem.message;
        }
        return false;
    }

    const std::string text = toJsonText(settings);

    std::error_code ec;
    if (file_.has_parent_path())
        std::filesystem::create_directories(file_.parent_path(), ec);

    if (ec)
    {
        error = "cannot create configuration directory " + file_.parent_path().string() + ": " + ec.message();
        return false;
    }

    const auto temp = temporaryFile();

    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            error = "cannot write temporary configuration file " + temp.string();
            return false;
        }

        out << text;
        out.flush();

        if (!out)
        {
            error = "configuration write did not complete";
            std::filesystem::remove(temp, ec);
            return false;
        }
    }

    // Read-back verification: never replace the good file with something we cannot
    // parse ourselves.
    {
        bool readOk = false;
        const std::string written = readWholeFile(temp, readOk);

        ConfigProblems readBackProblems;
        std::string readBackError;
        AppConfig readBack;

        if (!readOk || !fromJsonText(written, readBack, readBackProblems, readBackError)
            || !validate(readBack).empty())
        {
            error = "temporary configuration file failed verification";
            std::filesystem::remove(temp, ec);
            return false;
        }
    }

    if (std::filesystem::exists(file_, ec))
    {
        std::error_code copyEc;
        std::filesystem::copy_file(file_, backupFile(), std::filesystem::copy_options::overwrite_existing, copyEc);

        if (copyEc)
        {
            // Refuse to replace the live file without the safety copy.
            error = "cannot update backup file: " + copyEc.message();
            std::filesystem::remove(temp, ec);
            return false;
        }
    }

    std::error_code renameEc;
    std::filesystem::rename(temp, file_, renameEc);

    if (renameEc)
    {
        error = "cannot replace " + file_.string() + ": " + renameEc.message();
        std::filesystem::remove(temp, ec);
        return false;
    }

    return true;
}

std::filesystem::path ConfigStore::defaultFile()
{
#ifdef _WIN32
    const std::string appData = environmentVariable("APPDATA");
    if (!appData.empty())
        return std::filesystem::path(appData) / applicationDirectoryName() / "config.json";
#endif

    const std::string profile = environmentVariable("USERPROFILE");
    if (!profile.empty())
        return std::filesystem::path(profile) / ".liveai" / "config.json";

    const std::string home = environmentVariable("HOME");
    if (!home.empty())
        return std::filesystem::path(home) / ".liveai" / "config.json";

    return std::filesystem::path(".liveai") / "config.json";
}

} // namespace config
} // namespace liveai
