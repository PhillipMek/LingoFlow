#pragma once
//
// AppConfig - the complete set of non-secret settings (SPEC "Configuration").
//
// Hard rule (AGENTS.md 10, SPEC "Secrets separate"): an API key, token, password
// or any other credential must never be a member of this struct and never reach
// config.json. Credentials belong to the credential store (task 015); the tests
// additionally assert that nothing in the serialized config looks like a secret.
//
// Backend implementation details (socket options, driver internals) are not
// stored here either: only what the operator configures.

#include <cstdint>
#include <string>
#include <vector>

namespace liveai {

/// Persistent schema version. Bump it when the file layout changes and add a
/// migration step in ConfigSchema; the loader refuses files from the future.
inline constexpr std::uint32_t kConfigSchemaVersion = 1;

struct AudioSettings
{
    /// Empty means "no device selected yet"; device identifiers come from the
    /// backend enumeration (task 004) and are opaque here.
    std::string inputDeviceId;
    std::string outputDeviceId;
    int sampleRate = 48000;      ///< SPEC "Audio": preferred 48 kHz
    int bufferFrames = 480;      ///< SPEC MVP buffer
    int inputChannel = 1;        ///< one-based channel index, mono translation input
    int outputChannel = 1;
    float inputGainDb = 0.0f;
    float outputGainDb = 0.0f;

    friend constexpr bool operator==(const AudioSettings&, const AudioSettings&) = default;
};

struct TranslationSettings
{
    /// Language tags; validated against the LanguageRegistry in task 011. MVP en<->ru.
    std::string inputLanguage = "en";
    std::string outputLanguage = "ru";

    /// Interpreter instructions (SPEC "Translation instructions"), operator-editable.
    std::string instructions =
        "Preserve meaning, names, numbers, terminology, proper nouns and intent. "
        "Do not summarize, add explanations or comment. "
        "Prioritize low latency without sacrificing quality.";

    /// Empty = "use the backend default". Free-form model identifiers are not
    /// invented here: allowed values come from the capability manifest
    /// (AGENTS.md 9, task 008/011).
    std::string modelHint;

    int jitterBufferMs = 120;

    // Session recovery policy (task 010). The supervisor retries a dropped
    // session forever while the application runs - a venue network blip must
    // not end the event - and stops only on errors retrying cannot fix (bad
    // key, billing), reported as the faulted state for the operator.
    bool reconnectEnabled = true;
    int reconnectInitialBackoffMs = 1000;  ///< first retry delay
    int reconnectMaxBackoffMs = 15000;     ///< exponential backoff cap
    /// Proactively reopen the session before the one-hour ceiling measured
    /// live (protocol doc section 15); 0 disables the timer. The provider
    /// session restarts fresh either way - the gap is counted, audio during it
    /// is refused and dropped (task 010's gap policy).
    int sessionMaxAgeSeconds = 3300;

    friend constexpr bool operator==(const TranslationSettings&, const TranslationSettings&) = default;
};

struct NdiSettings
{
    bool enabled = false;
    std::string streamName = "LingoFlow";

    friend constexpr bool operator==(const NdiSettings&, const NdiSettings&) = default;
};

struct DiagnosticsSettings
{
    /// Log level name, validated against liveai::log (trace..critical, off).
    std::string logLevel = "info";
    bool writeLogFile = true;

    friend constexpr bool operator==(const DiagnosticsSettings&, const DiagnosticsSettings&) = default;
};

struct AppConfig
{
    std::uint32_t schemaVersion = kConfigSchemaVersion;

    AudioSettings audio;
    TranslationSettings translation;
    NdiSettings ndi;
    DiagnosticsSettings diagnostics;

    /// Used by tests, by change detection and by "did the load really change
    /// anything" checks. Comparison is exact: these are operator settings, not
    /// measured values.
    friend constexpr bool operator==(const AppConfig&, const AppConfig&) = default;
};

/// One validation problem, with the JSON path it came from.
struct ConfigProblem
{
    std::string field;     ///< e.g. "audio.sampleRate"
    std::string message;
};

using ConfigProblems = std::vector<ConfigProblem>;

} // namespace liveai
