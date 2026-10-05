#include "Config/ConfigSchema.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <functional>
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <vector>

#include "Utils/Log.h"

namespace liveai {
namespace config {
namespace {

constexpr int kMinBufferFrames = 64;
constexpr int kMaxBufferFrames = 2048;
constexpr int kMaxChannels = 128;  ///< one-based, the bound validate() and the UI share
// SPEC "Input Gain" suggests -24..+24 dB and keeps the range configurable; the
// validation window is wider so the console-side trim stays possible, but +24 dB
// must never be rejected (it was -60..+12 before, which blocked a spec value).
constexpr float kMinGainDb = -60.0f;
constexpr float kMaxGainDb = 24.0f;
constexpr int kMaxJitterBufferMs = 1000;
constexpr int kMinReconnectBackoffMs = 100;
constexpr int kMaxReconnectBackoffMs = 120000;
constexpr int kMaxSessionAgeSeconds = 7200;  ///< two measured one-hour ceilings
/// The safety margin for a server-announced expiry (protocol docs section 4bis)
/// is bounded by half the age ceiling: a margin of over 30 minutes against a
/// 60-minute session would reopen before the previous reopen made sense.
constexpr int kMaxExpiryMarginSeconds = 1800;
constexpr std::size_t kMaxInstructionsLength = 4000;
constexpr std::size_t kMaxIdentifierLength = 64;
constexpr std::size_t kMaxDeviceIdLength = 256;
constexpr std::size_t kMaxLanguageTagLength = 12;

// Task 019 developer-mode bounds. The paths ride the same shape rule as device
// identifiers (bounded, control-character-free) - a settings field is never a
// license for an unbounded string, even a "only ever dev" one.
constexpr int kMaxMockLatencyMs = 5000;
constexpr double kMinToneFrequencyHz = 20.0;
constexpr double kMaxToneFrequencyHz = 20000.0;
constexpr double kMinToneLevelDb = -60.0;
constexpr double kMaxToneLevelDb = 0.0;

const std::set<int> kSupportedSampleRates{ 44100, 48000, 88200, 96000 };

bool hasControlCharacter(std::string_view text)
{
    return std::any_of(text.begin(), text.end(),
                       [](unsigned char c) { return c < 0x20 || c == 0x7f; });
}

bool isLanguageTag(std::string_view tag)
{
    if (tag.empty() || tag.size() > kMaxLanguageTagLength)
        return false;

    return std::all_of(tag.begin(), tag.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    });
}

bool isOpaqueIdentifier(std::string_view text, std::size_t maxLength)
{
    return !text.empty() && text.size() <= maxLength && !hasControlCharacter(text);
}

template <typename T>
void readNumber(const nlohmann::json& object,
                const char* key,
                const std::string& path,
                T& destination,
                ConfigProblems& problems)
{
    const auto it = object.find(key);
    if (it == object.end() || it->is_null())
        return;

    if (!it->is_number())
    {
        problems.push_back(ConfigProblem{ path, "expected a number, ignored the value" });
        return;
    }

    try
    {
        destination = it->get<T>();
    }
    catch (const nlohmann::json::exception&)
    {
        problems.push_back(ConfigProblem{ path, "value out of range for its type, default kept" });
    }
}

void readBool(const nlohmann::json& object,
              const char* key,
              const std::string& path,
              bool& destination,
              ConfigProblems& problems)
{
    const auto it = object.find(key);
    if (it == object.end() || it->is_null())
        return;

    if (!it->is_boolean())
    {
        problems.push_back(ConfigProblem{ path, "expected a boolean, ignored the value" });
        return;
    }

    destination = it->get<bool>();
}

void readString(const nlohmann::json& object,
               const char* key,
               const std::string& path,
               std::string& destination,
               ConfigProblems& problems)
{
    const auto it = object.find(key);
    if (it == object.end() || it->is_null())
        return;

    if (!it->is_string())
    {
        problems.push_back(ConfigProblem{ path, "expected a string, ignored the value" });
        return;
    }

    destination = it->get<std::string>();
}

const nlohmann::json* findSection(const nlohmann::json& root, const char* name, ConfigProblems& problems)
{
    const auto it = root.find(name);
    if (it == root.end() || it->is_null())
        return nullptr;

    if (!it->is_object())
    {
        problems.push_back(ConfigProblem{ name, "section is not an object, defaults kept" });
        return nullptr;
    }

    return &(*it);
}

// Unknown keys are reported instead of being kept: a config file that grows a
// field this build does not know about must not silently carry it forward, and a
// typo must be visible to the operator.
void reportUnknownKeys(const nlohmann::json& object,
                       const std::string& path,
                       const std::set<std::string>& knownKeys,
                       ConfigProblems& problems)
{
    for (auto it = object.begin(); it != object.end(); ++it)
    {
        const std::string key = it.key();

        if (knownKeys.contains(key))
            continue;

        // Secret-like keys are reported by the credential scan, not as "unknown".
        if (isSecretFieldName(key))
            continue;

        problems.push_back(ConfigProblem{ path.empty() ? key : path + "." + key, "unknown field ignored" });
    }
}

} // namespace

AppConfig defaults() noexcept
{
    return AppConfig{};
}

std::vector<int> supportedSampleRates()
{
    // std::set iterates ascending; the same object validate() consults, so the
    // UI can never offer a rate the schema would refuse.
    return { kSupportedSampleRates.begin(), kSupportedSampleRates.end() };
}

std::pair<int, int> bufferFramesRange() noexcept
{
    return { kMinBufferFrames, kMaxBufferFrames };
}

std::pair<float, float> gainRange() noexcept
{
    return { kMinGainDb, kMaxGainDb };
}

std::pair<int, int> jitterBufferRange() noexcept
{
    return { 0, kMaxJitterBufferMs };
}

std::pair<int, int> channelRange() noexcept
{
    return { 1, kMaxChannels };
}

std::pair<int, int> reconnectBackoffRange() noexcept
{
    return { kMinReconnectBackoffMs, kMaxReconnectBackoffMs };
}

std::pair<int, int> sessionMaxAgeRange() noexcept
{
    return { 0, kMaxSessionAgeSeconds };
}

std::pair<int, int> expiryMarginRange() noexcept
{
    return { 0, kMaxExpiryMarginSeconds };
}

std::pair<int, int> mockLatencyRange() noexcept
{
    return { 0, kMaxMockLatencyMs };
}

std::pair<double, double> toneFrequencyRange() noexcept
{
    return { kMinToneFrequencyHz, kMaxToneFrequencyHz };
}

std::pair<double, double> toneLevelRangeDb() noexcept
{
    return { kMinToneLevelDb, kMaxToneLevelDb };
}

std::vector<std::string> developerAudioSources()
{
    return { "device", "wav", "tone" };
}

std::size_t maxInstructionsLength() noexcept
{
    return kMaxInstructionsLength;
}

std::size_t maxIdentifierLength() noexcept
{
    return kMaxIdentifierLength;
}

ConfigProblems validate(const AppConfig& candidate)
{
    ConfigProblems problems;

    if (candidate.schemaVersion == 0 || candidate.schemaVersion > kConfigSchemaVersion)
    {
        problems.push_back(ConfigProblem{ "schemaVersion",
                                          "value " + std::to_string(candidate.schemaVersion)
                                              + " is not supported by this build (max "
                                              + std::to_string(kConfigSchemaVersion) + ")" });
    }

    const auto& audio = candidate.audio;

    if (kSupportedSampleRates.find(audio.sampleRate) == kSupportedSampleRates.end())
        problems.push_back(ConfigProblem{ "audio.sampleRate", std::to_string(audio.sampleRate) + " Hz is not supported" });

    if (audio.bufferFrames < kMinBufferFrames || audio.bufferFrames > kMaxBufferFrames)
        problems.push_back(ConfigProblem{ "audio.bufferFrames",
                                          "must be between " + std::to_string(kMinBufferFrames) + " and "
                                              + std::to_string(kMaxBufferFrames) + " frames" });

    if (audio.inputChannel < 1 || audio.outputChannel < 1 || audio.inputChannel > kMaxChannels
        || audio.outputChannel > kMaxChannels)
        problems.push_back(ConfigProblem{ "audio.inputChannel",
                                          "channel numbers are one-based and must be between 1 and "
                                              + std::to_string(kMaxChannels) });

    const auto gainInRange = [](float value)
    { return std::isfinite(value) && value >= kMinGainDb && value <= kMaxGainDb; };

    if (!gainInRange(audio.inputGainDb) || !gainInRange(audio.outputGainDb))
        problems.push_back(ConfigProblem{ "audio.inputGainDb",
                                          "gain must be finite and between "
                                              + std::to_string(static_cast<int>(kMinGainDb)) + " and "
                                              + std::to_string(static_cast<int>(kMaxGainDb)) + " dB" });

    for (const auto& [value, path] : { std::pair{ audio.inputDeviceId, std::string{ "audio.inputDeviceId" } },
                                       std::pair{ audio.outputDeviceId, std::string{ "audio.outputDeviceId" } } })
    {
        if (value.size() > kMaxDeviceIdLength || hasControlCharacter(value))
            problems.push_back(ConfigProblem{ path, "device identifier is empty-ish or too long" });
    }

    const auto& translation = candidate.translation;

    if (!isLanguageTag(translation.inputLanguage))
        problems.push_back(ConfigProblem{ "translation.inputLanguage", "language tag must be 1-12 alphanumeric characters" });

    if (!isLanguageTag(translation.outputLanguage))
        problems.push_back(ConfigProblem{ "translation.outputLanguage", "language tag must be 1-12 alphanumeric characters" });

    if (translation.inputLanguage == translation.outputLanguage)
        problems.push_back(ConfigProblem{ "translation.outputLanguage", "input and output language must differ" });

    if (translation.instructions.empty() || translation.instructions.size() > kMaxInstructionsLength
        || hasControlCharacter(translation.instructions))
        problems.push_back(ConfigProblem{ "translation.instructions",
                                          "instructions must be non-empty, free of control characters and at most "
                                              + std::to_string(kMaxInstructionsLength) + " characters" });

    if (!translation.modelHint.empty()
        && (translation.modelHint.size() > kMaxIdentifierLength || hasControlCharacter(translation.modelHint)))
        problems.push_back(ConfigProblem{ "translation.modelHint", "model hint must be empty or up to 64 printable characters" });

    if (translation.jitterBufferMs < 0 || translation.jitterBufferMs > kMaxJitterBufferMs)
        problems.push_back(ConfigProblem{ "translation.jitterBufferMs",
                                          "must be between 0 and " + std::to_string(kMaxJitterBufferMs) + " ms" });

    if (translation.reconnectInitialBackoffMs < kMinReconnectBackoffMs
        || translation.reconnectInitialBackoffMs > kMaxReconnectBackoffMs)
        problems.push_back(ConfigProblem{ "translation.reconnectInitialBackoffMs",
                                          "must be between " + std::to_string(kMinReconnectBackoffMs) + " and "
                                              + std::to_string(kMaxReconnectBackoffMs) + " ms" });

    if (translation.reconnectMaxBackoffMs < translation.reconnectInitialBackoffMs
        || translation.reconnectMaxBackoffMs > kMaxReconnectBackoffMs)
        problems.push_back(ConfigProblem{ "translation.reconnectMaxBackoffMs",
                                          "must be at least the initial backoff and at most "
                                              + std::to_string(kMaxReconnectBackoffMs) + " ms" });

    if (translation.sessionMaxAgeSeconds < 0 || translation.sessionMaxAgeSeconds > kMaxSessionAgeSeconds)
        problems.push_back(ConfigProblem{ "translation.sessionMaxAgeSeconds",
                                          "must be between 0 (disabled) and "
                                              + std::to_string(kMaxSessionAgeSeconds) + " s" });

    if (translation.expirySafetyMarginSeconds < 0 || translation.expirySafetyMarginSeconds > kMaxExpiryMarginSeconds)
        problems.push_back(ConfigProblem{ "translation.expirySafetyMarginSeconds",
                                          "must be between 0 (reopen at the announced instant; not recommended) and "
                                              + std::to_string(kMaxExpiryMarginSeconds) + " s" });

    if (candidate.ndi.enabled && (candidate.ndi.streamName.empty() || candidate.ndi.streamName.size() > kMaxIdentifierLength
                                  || hasControlCharacter(candidate.ndi.streamName)))
        problems.push_back(ConfigProblem{ "ndi.streamName", "NDI is enabled but the stream name is invalid" });

    const auto level = log::levelFromName(candidate.diagnostics.logLevel);
    if (log::nameOf(level) != candidate.diagnostics.logLevel)
        problems.push_back(ConfigProblem{ "diagnostics.logLevel",
                                          "unknown log level '" + candidate.diagnostics.logLevel + "'" });

    // Developer mode (task 019): the SHAPE is validated always - a nonsense
    // field is repaired even in the disabled state, so switching developer mode
    // on later cannot resurrect a typo the operator made months ago. Whether
    // the values MEAN anything is decided elsewhere (App/DeveloperMode.h),
    // where everything but `enabled` is inert while it is false.
    const auto& developer = candidate.developer;

    const auto developerSources = developerAudioSources();

    if (!developerSources.empty()
        && std::find(developerSources.begin(), developerSources.end(), developer.audioSource)
               == developerSources.end())
        problems.push_back(ConfigProblem{ "developer.audioSource",
                                          "audio source must be one of device, wav, tone (got '"
                                              + developer.audioSource + "')" });

    const auto pathOk = [](const std::string& path)
    { return path.size() <= kMaxDeviceIdLength && !hasControlCharacter(path); };

    if (!developer.wavInputPath.empty() && !pathOk(developer.wavInputPath))
        problems.push_back(ConfigProblem{ "developer.wavInputPath",
                                          "a WAV path must stay under 256 characters with no control characters" });

    if (!developer.wavOutputPath.empty() && !pathOk(developer.wavOutputPath))
        problems.push_back(ConfigProblem{ "developer.wavOutputPath",
                                          "a WAV path must stay under 256 characters with no control characters" });

    if (!std::isfinite(developer.toneFrequencyHz)
        || developer.toneFrequencyHz < kMinToneFrequencyHz
        || developer.toneFrequencyHz > kMaxToneFrequencyHz)
        problems.push_back(ConfigProblem{ "developer.toneFrequencyHz",
                                          "test tone frequency must be between "
                                              + std::to_string(static_cast<int>(kMinToneFrequencyHz)) + " and "
                                              + std::to_string(static_cast<int>(kMaxToneFrequencyHz)) + " Hz" });

    if (!std::isfinite(developer.toneLevelDb) || developer.toneLevelDb < kMinToneLevelDb
        || developer.toneLevelDb > kMaxToneLevelDb)
        problems.push_back(ConfigProblem{ "developer.toneLevelDb",
                                          "test tone level must be between -60 and 0 dBFS" });

    if (developer.mockLatencyMs < 0 || developer.mockLatencyMs > kMaxMockLatencyMs)
        problems.push_back(ConfigProblem{ "developer.mockLatencyMs",
                                          "mock latency must be between 0 and "
                                              + std::to_string(kMaxMockLatencyMs) + " ms" });

    // An incoherent combination is refused here, not discovered at Start: a
    // WAV source needs the file. ("enabled" matters: the same fields while
    // developer mode sleeps are inert but shape-checked above; requiring the
    // path only when the mode lives keeps old files loadable.)
    if (developer.enabled && developer.audioSource == "wav" && developer.wavInputPath.empty())
        problems.push_back(ConfigProblem{ "developer.wavInputPath",
                                          "developer mode asks for a WAV source but names no file" });

    return problems;
}

bool validate(const AppConfig& candidate, std::string& error)
{
    const auto problems = validate(candidate);
    if (problems.empty())
    {
        error.clear();
        return true;
    }

    error.clear();
    for (const auto& problem : problems)
    {
        if (!error.empty())
            error += "; ";
        error += problem.field + ": " + problem.message;
    }
    return false;
}

std::string toJsonText(const AppConfig& settings)
{
    const nlohmann::json root = {
        { "schemaVersion", settings.schemaVersion },
        { "audio",
          { { "inputDeviceId", settings.audio.inputDeviceId },
            { "outputDeviceId", settings.audio.outputDeviceId },
            { "sampleRate", settings.audio.sampleRate },
            { "bufferFrames", settings.audio.bufferFrames },
            { "inputChannel", settings.audio.inputChannel },
            { "outputChannel", settings.audio.outputChannel },
            { "inputGainDb", settings.audio.inputGainDb },
            { "outputGainDb", settings.audio.outputGainDb } } },
        { "translation",
          { { "inputLanguage", settings.translation.inputLanguage },
            { "outputLanguage", settings.translation.outputLanguage },
            { "instructions", settings.translation.instructions },
            { "modelHint", settings.translation.modelHint },
            { "jitterBufferMs", settings.translation.jitterBufferMs },
            { "reconnectEnabled", settings.translation.reconnectEnabled },
            { "reconnectInitialBackoffMs", settings.translation.reconnectInitialBackoffMs },
            { "reconnectMaxBackoffMs", settings.translation.reconnectMaxBackoffMs },
            { "sessionMaxAgeSeconds", settings.translation.sessionMaxAgeSeconds },
            { "expirySafetyMarginSeconds", settings.translation.expirySafetyMarginSeconds } } },
        { "ndi", { { "enabled", settings.ndi.enabled }, { "streamName", settings.ndi.streamName } } },
        { "diagnostics", { { "logLevel", settings.diagnostics.logLevel }, { "writeLogFile", settings.diagnostics.writeLogFile } } },
        { "developer",
          { { "enabled", settings.developer.enabled },
            { "audioSource", settings.developer.audioSource },
            { "wavInputPath", settings.developer.wavInputPath },
            { "wavOutputPath", settings.developer.wavOutputPath },
            { "toneFrequencyHz", settings.developer.toneFrequencyHz },
            { "toneLevelDb", settings.developer.toneLevelDb },
            { "mockTranslation", settings.developer.mockTranslation },
            { "mockLatencyMs", settings.developer.mockLatencyMs },
            { "loopback", settings.developer.loopback } } },
    };

    return root.dump(2) + "\n";
}

bool fromJsonText(std::string_view text,
                  AppConfig& out,
                  ConfigProblems& problems,
                  std::string& error,
                  const AppConfig& fallback)
{
    out = fallback;

    nlohmann::json root;
    try
    {
        root = nlohmann::json::parse(std::string(text.begin(), text.end()));
    }
    catch (const nlohmann::json::exception& exception)
    {
        error = std::string("config is not valid JSON: ") + exception.what();
        return false;
    }

    if (!root.is_object())
    {
        error = "config root must be a JSON object";
        return false;
    }

    error.clear();

    // Credentials must never enter config.json. Every key that looks like one is
    // reported and ignored, at any nesting depth, instead of being loaded into
    // memory or silently kept on disk.
    std::function<void(const nlohmann::json&, const std::string&)> scanForSecretKeys =
        [&](const nlohmann::json& node, const std::string& path)
        {
            if (!node.is_object())
                return;

            for (auto it = node.begin(); it != node.end(); ++it)
            {
                const std::string childPath = path.empty() ? std::string(it.key()) : path + "." + it.key();

                if (isSecretFieldName(it.key()))
                    problems.push_back(ConfigProblem{ childPath,
                                                      "credential-like field is never stored in config.json and was ignored" });

                scanForSecretKeys(it.value(), childPath);
            }
        };

    scanForSecretKeys(root, "");

    static const std::set<std::string> kRootKeys{ "schemaVersion", "audio", "translation", "ndi", "diagnostics",
                                                  "developer" };
    reportUnknownKeys(root, "", kRootKeys, problems);

    readNumber<std::uint32_t>(root, "schemaVersion", "schemaVersion", out.schemaVersion, problems);

    if (const auto* section = findSection(root, "audio", problems); section != nullptr)
    {
        static const std::set<std::string> kKeys{ "inputDeviceId",      "outputDeviceId", "sampleRate",
                                                  "bufferFrames",        "inputChannel",   "outputChannel",
                                                  "inputGainDb",         "outputGainDb" };
        reportUnknownKeys(*section, "audio", kKeys, problems);

        auto& audio = out.audio;
        readString(*section, "inputDeviceId", "audio.inputDeviceId", audio.inputDeviceId, problems);
        readString(*section, "outputDeviceId", "audio.outputDeviceId", audio.outputDeviceId, problems);
        readNumber<int>(*section, "sampleRate", "audio.sampleRate", audio.sampleRate, problems);
        readNumber<int>(*section, "bufferFrames", "audio.bufferFrames", audio.bufferFrames, problems);
        readNumber<int>(*section, "inputChannel", "audio.inputChannel", audio.inputChannel, problems);
        readNumber<int>(*section, "outputChannel", "audio.outputChannel", audio.outputChannel, problems);
        readNumber<float>(*section, "inputGainDb", "audio.inputGainDb", audio.inputGainDb, problems);
        readNumber<float>(*section, "outputGainDb", "audio.outputGainDb", audio.outputGainDb, problems);
    }

    if (const auto* section = findSection(root, "translation", problems); section != nullptr)
    {
        static const std::set<std::string> kKeys{ "inputLanguage", "outputLanguage", "instructions",
                                                  "modelHint",     "jitterBufferMs",
                                                  "reconnectEnabled", "reconnectInitialBackoffMs",
                                                  "reconnectMaxBackoffMs", "sessionMaxAgeSeconds",
                                                  "expirySafetyMarginSeconds" };
        reportUnknownKeys(*section, "translation", kKeys, problems);

        auto& translation = out.translation;
        readString(*section, "inputLanguage", "translation.inputLanguage", translation.inputLanguage, problems);
        readString(*section, "outputLanguage", "translation.outputLanguage", translation.outputLanguage, problems);
        readString(*section, "instructions", "translation.instructions", translation.instructions, problems);
        readString(*section, "modelHint", "translation.modelHint", translation.modelHint, problems);
        readNumber<int>(*section, "jitterBufferMs", "translation.jitterBufferMs", translation.jitterBufferMs, problems);
        readBool(*section, "reconnectEnabled", "translation.reconnectEnabled", translation.reconnectEnabled, problems);
        readNumber<int>(*section, "reconnectInitialBackoffMs", "translation.reconnectInitialBackoffMs",
                        translation.reconnectInitialBackoffMs, problems);
        readNumber<int>(*section, "reconnectMaxBackoffMs", "translation.reconnectMaxBackoffMs",
                        translation.reconnectMaxBackoffMs, problems);
        readNumber<int>(*section, "sessionMaxAgeSeconds", "translation.sessionMaxAgeSeconds",
                        translation.sessionMaxAgeSeconds, problems);
        readNumber<int>(*section, "expirySafetyMarginSeconds", "translation.expirySafetyMarginSeconds",
                        translation.expirySafetyMarginSeconds, problems);
    }

    if (const auto* section = findSection(root, "ndi", problems); section != nullptr)
    {
        static const std::set<std::string> kKeys{ "enabled", "streamName" };
        reportUnknownKeys(*section, "ndi", kKeys, problems);

        readBool(*section, "enabled", "ndi.enabled", out.ndi.enabled, problems);
        readString(*section, "streamName", "ndi.streamName", out.ndi.streamName, problems);
    }

    if (const auto* section = findSection(root, "diagnostics", problems); section != nullptr)
    {
        static const std::set<std::string> kKeys{ "logLevel", "writeLogFile" };
        reportUnknownKeys(*section, "diagnostics", kKeys, problems);

        readString(*section, "logLevel", "diagnostics.logLevel", out.diagnostics.logLevel, problems);
        readBool(*section, "writeLogFile", "diagnostics.writeLogFile", out.diagnostics.writeLogFile, problems);
    }

    // Absent section = every field keeps its default, and the defaults say
    // "developer mode off, real chain" - which is why files written before this
    // section existed load into production behaviour untouched (task 019's
    // isolation rule works on the storage level too, not only at mount time).
    if (const auto* section = findSection(root, "developer", problems); section != nullptr)
    {
        static const std::set<std::string> kKeys{ "enabled",           "audioSource",   "wavInputPath",
                                                  "wavOutputPath",     "toneFrequencyHz", "toneLevelDb",
                                                  "mockTranslation",   "mockLatencyMs",  "loopback" };
        reportUnknownKeys(*section, "developer", kKeys, problems);

        auto& developer = out.developer;
        readBool(*section, "enabled", "developer.enabled", developer.enabled, problems);
        readString(*section, "audioSource", "developer.audioSource", developer.audioSource, problems);
        readString(*section, "wavInputPath", "developer.wavInputPath", developer.wavInputPath, problems);
        readString(*section, "wavOutputPath", "developer.wavOutputPath", developer.wavOutputPath, problems);
        readNumber<double>(*section, "toneFrequencyHz", "developer.toneFrequencyHz", developer.toneFrequencyHz,
                           problems);
        readNumber<double>(*section, "toneLevelDb", "developer.toneLevelDb", developer.toneLevelDb, problems);
        readBool(*section, "mockTranslation", "developer.mockTranslation", developer.mockTranslation, problems);
        readNumber<int>(*section, "mockLatencyMs", "developer.mockLatencyMs", developer.mockLatencyMs, problems);
        readBool(*section, "loopback", "developer.loopback", developer.loopback, problems);
    }

    // Values that parsed but are not usable: keep the file as it is, restore the
    // default for every offending field and report each one instead of hiding it.
    struct Repair
    {
        std::string field;
        std::function<void(AppConfig&, const AppConfig&)> restore;
    };

    static const std::vector<Repair> kRepairs = {
        { "schemaVersion", [](AppConfig& c, const AppConfig& d) { c.schemaVersion = d.schemaVersion; } },
        { "audio.sampleRate", [](AppConfig& c, const AppConfig& d) { c.audio.sampleRate = d.audio.sampleRate; } },
        { "audio.bufferFrames", [](AppConfig& c, const AppConfig& d) { c.audio.bufferFrames = d.audio.bufferFrames; } },
        { "audio.inputChannel",
          [](AppConfig& c, const AppConfig& d)
          {
              c.audio.inputChannel = d.audio.inputChannel;
              c.audio.outputChannel = d.audio.outputChannel;
          } },
        { "audio.inputGainDb",
          [](AppConfig& c, const AppConfig& d)
          {
              c.audio.inputGainDb = d.audio.inputGainDb;
              c.audio.outputGainDb = d.audio.outputGainDb;
          } },
        { "audio.inputDeviceId", [](AppConfig& c, const AppConfig& d) { c.audio.inputDeviceId = d.audio.inputDeviceId; } },
        { "audio.outputDeviceId", [](AppConfig& c, const AppConfig& d) { c.audio.outputDeviceId = d.audio.outputDeviceId; } },
        { "translation.inputLanguage",
          [](AppConfig& c, const AppConfig& d) { c.translation.inputLanguage = d.translation.inputLanguage; } },
        { "translation.outputLanguage",
          [](AppConfig& c, const AppConfig& d)
          {
              c.translation.outputLanguage = d.translation.outputLanguage;
              if (c.translation.inputLanguage == c.translation.outputLanguage)
                  c.translation.inputLanguage = d.translation.inputLanguage;
          } },
        { "translation.instructions",
          [](AppConfig& c, const AppConfig& d) { c.translation.instructions = d.translation.instructions; } },
        { "translation.modelHint", [](AppConfig& c, const AppConfig& d) { c.translation.modelHint = d.translation.modelHint; } },
        { "translation.jitterBufferMs",
          [](AppConfig& c, const AppConfig& d) { c.translation.jitterBufferMs = d.translation.jitterBufferMs; } },
        { "translation.reconnectEnabled",
          [](AppConfig& c, const AppConfig& d) { c.translation.reconnectEnabled = d.translation.reconnectEnabled; } },
        { "translation.reconnectInitialBackoffMs",
          [](AppConfig& c, const AppConfig& d) {
              c.translation.reconnectInitialBackoffMs = d.translation.reconnectInitialBackoffMs;
          } },
        { "translation.reconnectMaxBackoffMs",
          [](AppConfig& c, const AppConfig& d) { c.translation.reconnectMaxBackoffMs = d.translation.reconnectMaxBackoffMs; } },
        { "translation.sessionMaxAgeSeconds",
          [](AppConfig& c, const AppConfig& d) { c.translation.sessionMaxAgeSeconds = d.translation.sessionMaxAgeSeconds; } },
        { "translation.expirySafetyMarginSeconds",
          [](AppConfig& c, const AppConfig& d) { c.translation.expirySafetyMarginSeconds = d.translation.expirySafetyMarginSeconds; } },
        { "ndi.streamName",
          [](AppConfig& c, const AppConfig& d)
          {
              c.ndi.streamName = d.ndi.streamName;
              c.ndi.enabled = d.ndi.enabled;
          } },
        { "diagnostics.logLevel", [](AppConfig& c, const AppConfig& d) { c.diagnostics.logLevel = d.diagnostics.logLevel; } },
        { "developer.audioSource",
          [](AppConfig& c, const AppConfig& d)
          {
              c.developer.audioSource = d.developer.audioSource;
              c.developer.wavInputPath = d.developer.wavInputPath;
          } },
        { "developer.wavInputPath",
          [](AppConfig& c, const AppConfig& d)
          {
              c.developer.wavInputPath = d.developer.wavInputPath;

              // The empty-path-while-source-is-wav repair has to move the source
              // too, or the restored file stays as incoherent as the refused one.
              if (c.developer.audioSource == "wav")
                  c.developer.audioSource = d.developer.audioSource;
          } },
        { "developer.wavOutputPath",
          [](AppConfig& c, const AppConfig& d) { c.developer.wavOutputPath = d.developer.wavOutputPath; } },
        { "developer.toneFrequencyHz",
          [](AppConfig& c, const AppConfig& d) { c.developer.toneFrequencyHz = d.developer.toneFrequencyHz; } },
        { "developer.toneLevelDb",
          [](AppConfig& c, const AppConfig& d) { c.developer.toneLevelDb = d.developer.toneLevelDb; } },
        { "developer.mockLatencyMs",
          [](AppConfig& c, const AppConfig& d) { c.developer.mockLatencyMs = d.developer.mockLatencyMs; } },
    };

    for (const auto& problem : validate(out))
    {
        const auto repair = std::find_if(kRepairs.begin(), kRepairs.end(),
                                         [&problem](const Repair& candidate) { return candidate.field == problem.field; });

        if (repair != kRepairs.end())
            repair->restore(out, fallback);

        problems.push_back(ConfigProblem{ problem.field, problem.message + " (default restored)" });
    }

    return true;
}

bool isSecretFieldName(std::string_view keyName) noexcept
{
    static constexpr std::string_view kMarkers[] = {
        "api_key", "apikey", "api key", "secret", "token", "password", "passwd", "credential", "authorization", "bearer"
    };

    std::string lower(keyName);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    return std::any_of(std::begin(kMarkers), std::end(kMarkers),
                       [&lower](std::string_view marker) { return lower.find(marker) != std::string::npos; });
}

std::uint32_t schemaVersionOf(std::string_view text) noexcept
{
    try
    {
        const auto root = nlohmann::json::parse(std::string(text.begin(), text.end()));
        if (!root.is_object())
            return 0;

        const auto it = root.find("schemaVersion");
        if (it == root.end() || !it->is_number_unsigned())
            return 0;

        return it->get<std::uint32_t>();
    }
    catch (const nlohmann::json::exception&)
    {
        return 0;
    }
}

} // namespace config
} // namespace liveai
