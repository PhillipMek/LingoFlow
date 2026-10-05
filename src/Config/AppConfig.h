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
    /// Language tags. Config validates their SHAPE only (module boundary: it
    /// may not include the registry); supportability of the pair is checked at
    /// session start against the versioned capability manifest - task 011,
    /// Translation/LanguageRegistry.h. MVP en<->ru (AGENTS.md 9).
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

    /// Output pre-roll (SPEC "Output Jitter Buffer"). 250 ms is a provisional
    /// engineering value derived from the live wire facts of
    /// docs/openai-realtime-protocol.md section 15: deltas arrive in bursts of up
    /// to 2x realtime and the post-close drain runs at ~4.7x, and the engine's
    /// jitter headroom above the pre-roll is about one pre-roll
    /// (AudioEngine::jitterCapacity) - at the old 120 ms guess a burst longer than
    /// ~120 ms overflowed and dropped translated audio. This is still not an
    /// end-to-end measurement: the rig sweep (docs/rig-checklist.md section 7)
    /// picks the final default against a real device, and the operator can change
    /// it live.
    int jitterBufferMs = 250;

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
    /// is refused and dropped (task 010's gap policy). Where the server
    /// announces a concrete expiry (session.expires_at, protocol docs section
    /// 4bis), that announcement minus expirySafetyMarginSeconds is the deadline
    /// whenever it is EARLIER - this field then acts as the cap and the
    /// fallback, exactly the operator-policy role the provider's own word
    /// outranks.
    int sessionMaxAgeSeconds = 3300;
    /// How far ahead of a server-announced session expiry the controlled
    /// reopen starts (protocol docs section 4bis; code review P1, 2026-10-05).
    /// 0 means "reopen exactly at the announced instant" - allowed, not
    /// recommended: the reopen itself takes time and the provider does not wait.
    int expirySafetyMarginSeconds = 300;

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

/// Developer & mock mode (task 019). Every field is inert while `enabled` is
/// false, which is the default - and a config file from before this section
/// existed parses to exactly that, so an existing installation keeps running
/// the real chain (AGENTS.md 16's extension point, AGENTS.md 19's no fake
/// success, and this task's FAIL criterion all point the same way).
///
/// What this section does NOT contain: any credential. The mock runs with no
/// key at all; the OpenAI-backed paths keep using the credential store.
struct DeveloperSettings
{
    /// Master switch. False (the default) makes every field below meaningless:
    /// no simulated device is mounted, no mock backend exists, no loopback
    /// runs, and no badge appears. The plan builder (App/DeveloperMode.h) and
    /// the controller each re-check this - the isolation is belt AND braces.
    bool enabled = false;

    /// Where the "microphone" comes from: "device" (the real ASIO story, the
    /// default), "wav" (a file loops as input) or "tone" (a sine generator).
    std::string audioSource = "device";

    /// WAV file used when audioSource == "wav". Its sample rate must match the
    /// audio settings - the simulated device refuses a mismatch instead of
    /// resampling silently (task 007's delivered-audio rule, input side).
    std::string wavInputPath;

    /// When non-empty (and developer mode is on), everything the pipeline
    /// produced for the audience is appended here as a PCM16 WAV.
    std::string wavOutputPath;

    /// Test-tone parameters when audioSource == "tone".
    double toneFrequencyHz = 1000.0;
    double toneLevelDb = -20.0;

    /// Mount the echo translator instead of the provider chain. The session
    /// works end to end - audio returns, text events flow - and every text
    /// event says "mock" in words, because confusion with real translation is
    /// the one failure mode this feature cannot have.
    bool mockTranslation = false;
    int mockLatencyMs = 300;

    /// Route capture straight to the output (the task 005 loopback worker):
    /// the operator hears the room, not a translation. Loudly bad on a real
    /// show, so this is honored only while `enabled` is true, the controller
    /// refuses it otherwise, and the --dev command-line preset never turns it
    /// on - it stays a deliberate settings edit.
    bool loopback = false;

    friend constexpr bool operator==(const DeveloperSettings&, const DeveloperSettings&) = default;
};

struct AppConfig
{
    std::uint32_t schemaVersion = kConfigSchemaVersion;

    AudioSettings audio;
    TranslationSettings translation;
    NdiSettings ndi;
    DiagnosticsSettings diagnostics;
    DeveloperSettings developer;

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
