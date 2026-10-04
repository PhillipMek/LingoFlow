#include "Diagnostics/DiagnosticsExport.h"

#include <algorithm>
#include <fstream>
#include <random>
#include <sstream>
#include <string>

#include "Utils/Log.h"

namespace liveai {
namespace diagnostics {
namespace {

/// Newlines inside an exported value would fake a key=value line, so they are
/// flattened to spaces - a CRLF pair collapses to one space so Windows text
/// does not leave doubles behind. A format rule, not a hidden edit of the data
/// (the log keeps the original text).
std::string flatten(std::string_view value)
{
    std::string flat;
    flat.reserve(value.size());

    for (std::size_t i = 0; i < value.size(); ++i)
    {
        if (value[i] == '\r' && i + 1 < value.size() && value[i + 1] == '\n')
        {
            ++i;
            flat += ' ';
        }
        else if (value[i] == '\n' || value[i] == '\r')
        {
            flat += ' ';
        }
        else
        {
            flat += value[i];
        }
    }

    return flat;
}

} // namespace

RenderedExport renderExport(const std::string& appVersion,
                            const std::vector<ExportSection>& sections,
                            const std::vector<DiagnosticsManager::DiagnosticEvent>& events,
                            std::uint64_t evictedEvents,
                            SecretKeyPredicate isSecretKey)
{
    RenderedExport rendered;
    std::ostringstream out;

    out << "# LingoFlow diagnostics export\n";
    out << "generated_at=" << log::timestampNow() << "\n";
    out << "app_version=" << appVersion << "\n";
    out << "event_history=window\n";
    out << "events_in_window=" << events.size() << "\n";
    out << "events_evicted=" << evictedEvents << "\n";
    out << "# (the ring is bounded on purpose; a nonzero events_evicted means this "
           "file holds the recent window, not the full run)\n\n";

    for (const auto& section : sections)
    {
        out << "[" << section.name << "]\n";
        for (const auto& [key, value] : section.values)
        {
            // The value-side defense: a key that looks like a credential never
            // ships its value, whoever assembled the section. Defense in depth -
            // the gatherer's rule is "presence, not values"; this one does not
            // trust that rule and re-checks it, with Config's own definition
            // injected so no second list of secret words exists in the product.
            if (isSecretKey != nullptr && isSecretKey(key))
            {
                out << key << "=[redacted]\n";
                ++rendered.redactions;
                log::warning("app.diag",
                             "export redacted a secret-shaped key: " + std::string(key));
            }
            else
            {
                out << key << "=" << flatten(value) << "\n";
            }
        }
        out << "\n";
    }

    out << "[events]\n";
    for (const auto& event : events)
    {
        out << event.sequence << " " << event.timestamp << " [" << event.subsystem << "] "
            << flatten(event.message) << "\n";
    }

    rendered.text = out.str();
    return rendered;
}

bool writeExportFile(const std::filesystem::path& file, const std::string& text, std::string& error)
{
    std::error_code ec;
    if (file.has_parent_path())
        std::filesystem::create_directories(file.parent_path(), ec);   // best-effort; the open below is the real check

    // A unique tmp name in the same directory: the rename below needs the same
    // volume, and a fixed tmp name would let two exports race into one file.
    std::random_device entropy;
    std::filesystem::path tmpPath = file;
    tmpPath += "." + std::to_string(entropy()) + ".tmp";

    {
        std::ofstream stream(tmpPath, std::ios::out | std::ios::binary | std::ios::trunc);
        if (!stream.is_open())
        {
            error = "cannot open the export temp file " + tmpPath.string();
            return false;
        }

        stream.write(text.data(), static_cast<std::streamsize>(text.size()));
        stream.flush();
        if (!stream.good())
        {
            error = "writing the export temp file failed (disk full?)";
            stream.close();
            std::filesystem::remove(tmpPath, ec);
            return false;
        }
    }

    std::filesystem::rename(tmpPath, file, ec);
    if (ec)
    {
        error = "could not move the export into place: " + ec.message();
        std::filesystem::remove(tmpPath, ec);
        return false;
    }

    return true;
}

} // namespace diagnostics
} // namespace liveai
