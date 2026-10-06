#pragma once
//
// LanguageRegistry - the capability-driven language configuration.
//
// the project rules requires these three types and forbids hardcoded language lists
// scattered through the product. This file is that list's only home: the
// controller's start, the backend's open and (from 014) the UI dropdowns all
// read the SAME registry. When the provider cannot tell us its capabilities
// dynamically, the product uses a versioned manifest - and it cannot:
// `GET /v1/models` with this key answers `403 Missing scopes: api.model.read`
// (docs/openai-realtime-protocol.md section 15), so `openAiManifest()` below
// is the primary capability source, not a fallback.
//
// Provenance of the frozen data (docs section 13, live-verified section 15):
//   * targets: the 13 output languages, codes live-verified against the real
//     service (`es pt fr ja ru zh de ko hi id vi it en`, section 15 item 14.4);
//     ISO 639-1 two-letter codes only - that is what the service accepts and
//     what its own error messages disclose.
//   * sources: the 70+ input languages, enumerated by name in [R8] (fetched
//     2026-10-02), mapped to ISO 639-1 where a two-letter code exists. Two
//     listed names have no ISO 639-1 code (Filipino, Hawaiian); their ISO 639-2
//     codes (`fil`, `haw`) are carried instead. The source set never crosses
//     the wire - the provider auto-detects spoken language and `session.update`
//     carries only the TARGET (docs section 5) - so these codes are the
//     product's own identifiers for declaring and validating what the operator
//     expects to hear; nothing here depends on the service accepting them as
//     input parameters.
//
// Boundaries: this is protocol-free data - no event names, no
// session fields. Config may not include it (module boundary: shape validation
// of a stored tag string vs. supportability of a pair are different questions;
// the pair gate lives where sessions are opened: App and Network).

#include <string>
#include <string_view>
#include <vector>

namespace liveai {
namespace translation {

/// One language as the product knows it. `code` is the identifier that flows
/// into SessionRequest and (for targets only) the provider session; names are
/// for humans - the UI renders these, never its own list. Display
/// metadata can grow here when 014 needs it (native names); it is not invented
/// ahead of a need.
struct LanguageDefinition
{
    std::string code;       ///< ISO 639-1 (or -2 where -1 has none), lowercase
    std::string englishName; ///< as named by the capability source
};

/// The capability envelope itself: which languages may come in, which may go
/// out, and which edition of the truth this is. Versioned (the project rules):
/// a manifest without provenance is a rumor.
struct TranslationCapabilities
{
    int manifestVersion = 0;     ///< bump on any content change
    std::string manifestSource;  ///< where these lists came from, and when
    std::vector<LanguageDefinition> sources; ///< input languages (auto-detected)
    std::vector<LanguageDefinition> targets; ///< output languages (session parameter)
};

/// Outcome of checking one (input -> output) pair. `detail` is a complete
/// operator-readable sentence explaining a refusal (the project rules: no swallowed
/// reasons); it contains no provider vocabulary, only product terms.
struct PairCheck
{
    bool ok = false;
    std::string detail;

    explicit operator bool() const noexcept { return ok; }
};

class LanguageRegistry
{
public:
    explicit LanguageRegistry(TranslationCapabilities capabilities);

    const TranslationCapabilities& capabilities() const noexcept { return capabilities_; }

    /// Definition by code (case-insensitive: ISO tags are conventionally case-
    /// insensitive, stored config may be "EN"), searching sources then targets.
    /// nullptr when unknown.
    const LanguageDefinition* find(std::string_view code) const noexcept;

    /// Role membership, codes case-normalized like find().
    bool isSource(std::string_view code) const noexcept;
    bool isTarget(std::string_view code) const noexcept;

    /// The pair question the product asks: can this session translate input
    /// into output at all? Rules, in message order: both languages present;
    /// not the same language; input recognized as a source; output a target.
    PairCheck checkPair(std::string_view input, std::string_view output) const;

private:
    TranslationCapabilities capabilities_;
};

/// The shipped, frozen OpenAI capability manifest (docs sections 13/15), built
/// once and read everywhere. The type carries no provider name on purpose -
/// the NAME of this function is the honest label of where the data is from.
const LanguageRegistry& openAiManifest();

} // namespace translation
} // namespace liveai
