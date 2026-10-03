//
// Task 011: the language registry and the versioned capability manifest.
//
// These tests pin the manifest against its normative source -
// docs/openai-realtime-protocol.md section 13 (the [R8] enumeration) and
// section 15 item 14.4 (the live-verified target codes) - because a capability
// manifest nobody checks against its source is exactly the "invented API"
// failure mode AGENTS.md 19 forbids. The PASS criteria of the task map to
// sections below: EN<->RU works (both directions), unsupported pairs are
// rejected, and the checks prove the lists are consistent and complete without
// copying them anywhere else in the product.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cctype>
#include <set>
#include <string>
#include <vector>

#include "Translation/LanguageRegistry.h"

using namespace liveai;
using translation::LanguageRegistry;
using translation::PairCheck;
using translation::TranslationCapabilities;

namespace {

bool isLowerAlphaCode(const std::string& code)
{
    if (code.size() < 2 || code.size() > 3)
        return false;
    return std::all_of(code.begin(), code.end(),
                       [](unsigned char c) { return std::isalpha(c) && std::islower(c); });
}

} // namespace

TEST_CASE("LanguageRegistry: the manifest is versioned and carries its provenance",
          "[translation][languages]")
{
    const LanguageRegistry& registry = translation::openAiManifest();

    CHECK(registry.capabilities().manifestVersion == 1);

    // A manifest without provenance is a rumor (task header): where the data
    // came from, when it was taken, and what was live-verified must be stated.
    const std::string& source = registry.capabilities().manifestSource;
    CHECK_FALSE(source.empty());
    CHECK(source.find("2026-10-02") != std::string::npos);
    CHECK(source.find("14.4") != std::string::npos); // the live target-code record

    // Single frozen instance: repeated calls hand back the same registry, so
    // controller and backend cannot drift apart by reading "the" manifest twice.
    CHECK(&registry == &translation::openAiManifest());
    CHECK(&registry.capabilities() == &translation::openAiManifest().capabilities());
}

TEST_CASE("LanguageRegistry: targets are the thirteen live-verified codes",
          "[translation][languages]")
{
    // The pin: section 15 item 14.4 recorded all thirteen accepted as ISO 639-1
    // against the real service on 2026-10-02. Order per docs section 13.
    const std::vector<std::string> verified = { "es", "pt", "fr", "ja", "ru", "zh", "de",
                                                "ko", "hi", "id", "vi", "it", "en" };

    const auto& targets = translation::openAiManifest().capabilities().targets;
    REQUIRE(targets.size() == verified.size());

    for (std::size_t i = 0; i < targets.size(); ++i)
    {
        CHECK(targets[i].code == verified[i]);
        CHECK_FALSE(targets[i].englishName.empty());
        CHECK(isLowerAlphaCode(targets[i].code));

        // Every target is also a listed source: the thirteen are inside the
        // 70+ auto-detected input set (docs section 13). Not a provider fact we
        // assume per-language - it is what the frozen [R8] enumeration shows.
        CHECK(translation::openAiManifest().isSource(targets[i].code));
    }

    // MVP pair from AGENTS.md 9 present on both sides.
    CHECK(translation::openAiManifest().isTarget("en"));
    CHECK(translation::openAiManifest().isTarget("ru"));
}

TEST_CASE("LanguageRegistry: sources are the frozen R8 enumeration", "[translation][languages]")
{
    const auto& sources = translation::openAiManifest().capabilities().sources;

    // "over 70" per [R8]; this fetch froze 74 entries on 2026-10-02. The count
    // is a fact ABOUT the manifest, checked so an edit cannot silently drop a
    // language.
    CHECK(sources.size() == 74);

    std::set<std::string> codes;
    for (const auto& lang : sources)
    {
        CHECK(isLowerAlphaCode(lang.code));
        CHECK_FALSE(lang.englishName.empty());
        CHECK(codes.insert(lang.code).second); // unique
    }

    // Spot-membership, both directions of the question:
    CHECK(translation::openAiManifest().isSource("en"));
    CHECK(translation::openAiManifest().isSource("ru"));
    CHECK(translation::openAiManifest().isSource("nl"));  // Dutch: a source...
    CHECK_FALSE(translation::openAiManifest().isTarget("nl")); // ...never a target
    CHECK(translation::openAiManifest().isSource("dz"));  // the two rare ISO-1 codes
    CHECK(translation::openAiManifest().isSource("fil")); // ISO 639-2 where 639-1 has none
    CHECK(translation::openAiManifest().isSource("haw"));
    CHECK(translation::openAiManifest().isSource("tl"));  // R8 lists Tagalog AND Filipino
    CHECK_FALSE(translation::openAiManifest().isSource("am")); // Amharic: real language,
                                                               // absent from the manifest
    CHECK_FALSE(translation::openAiManifest().isSource("xx"));  // not a language at all
}

TEST_CASE("LanguageRegistry: pair checks cover the MVP and reject what cannot work",
          "[translation][languages]")
{
    const LanguageRegistry& reg = translation::openAiManifest();

    // PASS criterion: English <-> Russian works in BOTH directions.
    CHECK(static_cast<bool>(reg.checkPair("en", "ru")));
    CHECK(static_cast<bool>(reg.checkPair("ru", "en")));

    // Non-MVP pairs inside the envelope are the manifest's business too: the
    // controller test (task 007 suite) runs de->ja, [R8] lists both roles.
    CHECK(static_cast<bool>(reg.checkPair("de", "ja")));
    CHECK(static_cast<bool>(reg.checkPair("nl", "ru"))); // a source in, a target out

    // ISO tags are case-insensitive; stored config may say "EN".
    CHECK(static_cast<bool>(reg.checkPair("EN", "ru")));
    CHECK(static_cast<bool>(reg.checkPair("ru", "EN")));

    // Unsupported combinations each fail with a complete operator-readable
    // sentence (AGENTS.md 19: no swallowed reasons).
    const PairCheck badTarget = reg.checkPair("en", "nl");
    CHECK_FALSE(static_cast<bool>(badTarget));
    CHECK(badTarget.detail.find("output language") != std::string::npos);
    CHECK(badTarget.detail.find("target") != std::string::npos);

    const PairCheck badSource = reg.checkPair("am", "ru");
    CHECK_FALSE(static_cast<bool>(badSource));
    CHECK(badSource.detail.find("source language") != std::string::npos);

    const PairCheck same = reg.checkPair("en", "EN"); // case-normalized equality
    CHECK_FALSE(static_cast<bool>(same));
    CHECK(same.detail.find("same") != std::string::npos);

    const PairCheck noTarget = reg.checkPair("en", "");
    CHECK_FALSE(static_cast<bool>(noTarget));
    CHECK(noTarget.detail.find("target language") != std::string::npos);

    const PairCheck noInput = reg.checkPair("", "ru");
    CHECK_FALSE(static_cast<bool>(noInput));
    CHECK(noInput.detail.find("input language") != std::string::npos);

    // Empty output outranks empty input: the target is the provider-facing knob.
    const PairCheck none = reg.checkPair("", "");
    CHECK_FALSE(static_cast<bool>(none));
    CHECK(none.detail.find("target language") != std::string::npos);
}

TEST_CASE("LanguageRegistry: lookups are case-insensitive and role-aware",
          "[translation][languages]")
{
    const LanguageRegistry& reg = translation::openAiManifest();

    const auto* russian = reg.find("ru");
    REQUIRE(russian != nullptr);
    CHECK(russian->englishName == "Russian");
    CHECK(reg.find("RU") == russian); // same entry, any case

    CHECK(reg.find("zz") == nullptr);
    CHECK(reg.find("") == nullptr);

    // Roles differ for the union member that is only ever an input.
    CHECK(reg.isSource("nl"));
    CHECK_FALSE(reg.isTarget("nl"));
    CHECK(reg.isSource("ja"));
    CHECK(reg.isTarget("ja"));
}

TEST_CASE("LanguageRegistry: a custom capability set behaves like the manifest",
          "[translation][languages]")
{
    // The registry type is generic capability data, not a hardcoded list in
    // disguise: a hand-built envelope drives the same checks. This is the seam
    // a dynamic provider manifest would flow through later (AGENTS.md 9).
    TranslationCapabilities tiny;
    tiny.manifestVersion = 42;
    tiny.manifestSource = "unit fixture";
    tiny.sources = { { "xx", "Testish" }, { "qq", "Quaintish" } };
    tiny.targets = { { "qq", "Quaintish" } };

    const LanguageRegistry reg(std::move(tiny));

    CHECK(reg.capabilities().manifestVersion == 42);
    CHECK(static_cast<bool>(reg.checkPair("xx", "qq")));
    CHECK_FALSE(static_cast<bool>(reg.checkPair("en", "ru"))); // the product pair
                                                               // means nothing here
    CHECK(reg.find("XX") != nullptr);
    CHECK_FALSE(reg.isTarget("xx"));
}
