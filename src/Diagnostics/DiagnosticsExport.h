#pragma once
//
// DiagnosticsExport - the structured snapshot: sections of
// key/value facts plus the event ring, rendered to one text file and written
// atomically.
//
// Design choices, each for a reason:
//   * key=value text, not JSON. The JSON parser belongs to Config alone (the
//     boundary audit); a report format invented here on std::string keeps that
//     boundary intact and stays diff-friendly for a human carrying files back
//     from a venue.
//   * The module takes data, it does not fetch data. Diagnostics must not know
//     the engine, the session or the credential store - the controller (App
//     layer) gathers and hands over, so this file stays unit-testable and no
//     secret can sneak in through a dependency the format does not control.
//   * "no secrets" is enforced three times: by structure (an API key never
//     enters AppConfig and the gatherer passes presence, not values - the project rules
//     10), by a redaction pass at render time (a value under a key that looks
//     like a credential never ships, whatever gathered it), and by a value-side
//     last net: the controller hands redactSecretValues the values
//     the credential store actually holds, and any occurrence of them - pasted
//     into "instructions", "model_hint", an NDI stream name, anywhere in the
//     text - is erased and counted. Defense in depth is not a claim of
//     cleverness; it is the task's FAIL criterion ("secret leakage") taken
//     seriously in code.
//   * Writing is tmp-file + rename. A half-written report is worse than no
//     report, and a venue operator must never wonder which lines survived.
//
// Everything here allocates and touches the filesystem: UI/worker threads
// only, by construction.

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "Diagnostics/DiagnosticsManager.h"

namespace liveai {
namespace diagnostics {

struct ExportSection
{
    std::string name;                                        ///< [audio], [translation], ...
    std::vector<std::pair<std::string, std::string>> values; ///< ordered: the file reads top-down

    friend constexpr bool operator==(const ExportSection&, const ExportSection&) = default;
};

/// True when a key name looks like it carries a credential. The definition is
/// owned by Config (isSecretFieldName) and arrives as a function pointer -
/// Diagnostics may not include Config (the boundary audit), so the rule is
/// injected rather than duplicated, and a second list of "secret words" never
/// exists in this product.
using SecretKeyPredicate = bool (*)(std::string_view keyName);

/// The rendered report plus what the redaction pass removed. Returning the
/// count instead of hiding it in a global: a redaction the caller cannot see
/// would itself be a swallow.
struct RenderedExport
{
    std::string text;
    std::uint64_t redactions = 0;
};

/// The full report as one text block: header line (timestamp, eviction window
/// declared), then sections, then the event ring in order. `evictedEvents`
/// labels truncation honestly: a windowed history is not presented as complete.
/// Values under keys that `isSecretKey` flags are replaced by [redacted] and
/// counted - the export's defense in depth, wired to Config's single definition
/// by the calling controller.
RenderedExport renderExport(const std::string& appVersion,
                            const std::vector<ExportSection>& sections,
                            const std::vector<DiagnosticsManager::DiagnosticEvent>& events,
                            std::uint64_t evictedEvents,
                            SecretKeyPredicate isSecretKey);

/// The value-side last net. Free-form config fields ship their text
/// verbatim, so a credential an operator pasted into one of them travels to a
/// venue inside a receipt whose KEY name looks harmless - invisible to the
/// key-shaped redactor above. This pass erases every occurrence of a value the
/// credential store actually holds and counts them. Values shorter than
/// kMinRedactableValueLength are ignored: a short "secret" is a normal word's
/// substring, and redacting it would corrupt receipts without proving
/// anything. Being handed the values is the one place outside a session where
/// the product is allowed to hold a secret - and the only thing it does with
/// it is delete it. Pure text function: this module still does not know a
/// store exists; the controller reads the store and calls.
inline constexpr std::size_t kMinRedactableValueLength = 8;
std::uint64_t redactSecretValues(std::string& text, const std::vector<std::string>& secretValues);

/// Writes `text` to `file` atomically (same-directory tmp + rename). Parent
/// directories are created. On failure, returns false with the reason - no
/// silent fallback to another path.
bool writeExportFile(const std::filesystem::path& file, const std::string& text, std::string& error);

} // namespace diagnostics
} // namespace liveai
