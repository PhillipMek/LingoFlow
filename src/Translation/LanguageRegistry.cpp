#include "Translation/LanguageRegistry.h"

#include <algorithm>
#include <cctype>

namespace liveai {
namespace translation {

namespace {

/// ISO 639-1, lowercase - the form the service accepts and discloses in its
/// own error messages (docs section 15, item 14.4), and the form config
/// defaults use. Where an [R8] name has no 639-1 code, its 639-2 code is
/// carried (fil, haw) and that is noted at the entry, not invented around.
///
/// Source list: the [R8] enumeration, in the order the cookbook publishes it,
/// fetched 2026-10-02 (docs/openai-realtime-protocol.md section 13; [R8] =
/// OpenAI cookbook "Build Live Translation Apps with gpt-realtime-translate").
/// R8 lists both Tagalog and Filipino - they are separate entries because the
/// capability source says so, not because we reconciled them.
const std::vector<LanguageDefinition>& sourceLanguages()
{
    static const std::vector<LanguageDefinition> kSources = {
        { "ar", "Arabic" },           { "af", "Afrikaans" },
        { "az", "Azerbaijani" },      { "be", "Belarusian" },
        { "bn", "Bengali" },          { "bs", "Bosnian" },
        { "bg", "Bulgarian" },        { "ca", "Catalan" },
        { "zh", "Chinese" },          { "hr", "Croatian" },
        { "cs", "Czech" },            { "da", "Danish" },
        { "nl", "Dutch" },            { "dz", "Dzongkha" },
        { "en", "English" },          { "eo", "Esperanto" },
        { "et", "Estonian" },         { "eu", "Basque" },
        { "fa", "Persian / Farsi" },  { "fi", "Finnish" },
        { "fil", "Filipino" },        // no ISO 639-1 code; 639-2 carries it
        { "fr", "French" },           { "gl", "Galician" },
        { "de", "German" },           { "el", "Greek" },
        { "gu", "Gujarati" },         { "ht", "Haitian Creole" },
        { "haw", "Hawaiian" },        // no ISO 639-1 code; 639-2 carries it
        { "he", "Hebrew" },           { "hi", "Hindi" },
        { "hu", "Hungarian" },        { "hy", "Armenian" },
        { "id", "Indonesian" },       { "it", "Italian" },
        { "ja", "Japanese" },         { "jv", "Javanese" },
        { "ka", "Georgian" },         { "kk", "Kazakh" },
        { "ko", "Korean" },           { "ku", "Kurdish" },
        { "la", "Latin" },            { "lv", "Latvian" },
        { "lt", "Lithuanian" },       { "mk", "Macedonian" },
        { "ms", "Malay" },            { "ml", "Malayalam" },
        { "mi", "Maori" },            { "mn", "Mongolian" },
        { "my", "Burmese / Myanmar" },{ "ne", "Nepali" },
        { "no", "Norwegian" },        { "nn", "Nynorsk" },
        { "pl", "Polish" },           { "pt", "Portuguese" },
        { "pa", "Punjabi" },          { "ro", "Romanian" },
        { "ru", "Russian" },          { "sr", "Serbian" },
        { "sn", "Shona" },            { "sk", "Slovak" },
        { "sl", "Slovenian" },        { "sq", "Albanian" },
        { "es", "Spanish" },          { "sw", "Swahili" },
        { "sv", "Swedish" },          { "tl", "Tagalog" },
        { "te", "Telugu" },           { "th", "Thai" },
        { "tr", "Turkish" },          { "uk", "Ukrainian" },
        { "uz", "Uzbek" },            { "vi", "Vietnamese" },
        { "cy", "Welsh" },            { "yo", "Yoruba" },
    };
    return kSources;
}

/// Target list: the 13 output languages of [R8] / docs section 13. The codes
/// are not our mapping guess - all thirteen were submitted to the live service
/// and accepted on 2026-10-02 (docs section 15, item 14.4).
const std::vector<LanguageDefinition>& targetLanguages()
{
    static const std::vector<LanguageDefinition> kTargets = {
        { "es", "Spanish" },     { "pt", "Portuguese" }, { "fr", "French" },
        { "ja", "Japanese" },    { "ru", "Russian" },    { "zh", "Chinese" },
        { "de", "German" },      { "ko", "Korean" },     { "hi", "Hindi" },
        { "id", "Indonesian" },  { "vi", "Vietnamese" }, { "it", "Italian" },
        { "en", "English" },
    };
    return kTargets;
}

std::string toLowerCode(std::string_view code)
{
    std::string out(code);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

bool containsCode(const std::vector<LanguageDefinition>& list, const std::string& code)
{
    return std::any_of(list.begin(), list.end(),
                       [&code](const LanguageDefinition& lang) { return lang.code == code; });
}

} // namespace

// ----------------------------------------------------------------------- registry

LanguageRegistry::LanguageRegistry(TranslationCapabilities capabilities)
    : capabilities_(std::move(capabilities))
{
}

const LanguageDefinition* LanguageRegistry::find(std::string_view code) const noexcept
{
    const std::string needle = toLowerCode(code);

    for (const LanguageDefinition& lang : capabilities_.sources)
        if (lang.code == needle)
            return &lang;
    for (const LanguageDefinition& lang : capabilities_.targets)
        if (lang.code == needle)
            return &lang;
    return nullptr;
}

bool LanguageRegistry::isSource(std::string_view code) const noexcept
{
    return containsCode(capabilities_.sources, toLowerCode(code));
}

bool LanguageRegistry::isTarget(std::string_view code) const noexcept
{
    return containsCode(capabilities_.targets, toLowerCode(code));
}

PairCheck LanguageRegistry::checkPair(std::string_view input, std::string_view output) const
{
    const std::string in = toLowerCode(input);
    const std::string out = toLowerCode(output);

    // Order matters for the operator: the target is the one knob the provider
    // actually needs; a missing one is the most actionable defect.
    if (out.empty())
        return { false, "a target language is required" };
    if (in.empty())
        return { false, "an input language is required (the operator's expectation of what is spoken)" };
    if (in == out)
        return { false, "input and output language are the same ('" + out
                             + "'); there is nothing to translate" };
    if (!isSource(in))
        return { false, "input language '" + std::string(input)
                             + "' is not one of the supported source languages" };
    if (!isTarget(out))
        return { false, "output language '" + std::string(output)
                             + "' is not one of the supported target languages" };
    return { true, {} };
}

const LanguageRegistry& openAiManifest()
{
    static const LanguageRegistry registry = [] {
        TranslationCapabilities caps;
        caps.manifestVersion = 1;
        caps.manifestSource =
            "docs/openai-realtime-protocol.md sections 13/15: [R8] cookbook enumeration "
            "fetched 2026-10-02 (sources, names verbatim) and section 15 item 14.4 "
            "(13 target codes live-verified against the service the same day). No dynamic "
            "discovery with this key: GET /v1/models -> 403 Missing scopes api.model.read. "
            "The source set never crosses the wire (provider auto-detects); target codes do, "
            "ISO 639-1 only.";
        caps.sources = sourceLanguages();
        caps.targets = targetLanguages();
        return LanguageRegistry(std::move(caps));
    }();
    return registry;
}

} // namespace translation
} // namespace liveai
