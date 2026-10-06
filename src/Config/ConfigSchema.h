#pragma once
//
// ConfigSchema - defaults, validation and JSON text conversion for AppConfig.
//
// nlohmann/json is used only in the .cpp file: no header in the project sees a
// JSON type, so the UI and the audio path cannot accidentally start parsing
// configuration text themselves (tests/ArchitectureBoundaries.cmake allows this
// one include inside Config only).
//
// Secret rule: a field whose name looks like a credential is never read and never
// written. isSecretFieldName() is the single place that decides what "looks like
// a credential" means.

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Config/AppConfig.h"

namespace liveai {
namespace config {

    /// Operator defaults (SPEC: 48 kHz, float32, mono, en<->ru).
    AppConfig defaults() noexcept;

    // ----------------------------------------------------------------- ranges
    // The UI builds selectors from these, not from lists of its own: validate()
    // below and the controls on screen then cannot disagree (spec "single
    // Settings area", the established "UI does not own audio logic").

    /// The exact Hz values validate() accepts for audio.sampleRate, ascending.
    std::vector<int> supportedSampleRates();

    /// Inclusive bounds validate() accepts for audio.bufferFrames.
    std::pair<int, int> bufferFramesRange() noexcept;

    /// Inclusive bounds validate() accepts for the two gain fields, dB.
    std::pair<float, float> gainRange() noexcept;

    /// Inclusive bounds validate() accepts for translation.jitterBufferMs.
    std::pair<int, int> jitterBufferRange() noexcept;

    /// One-based channel bounds validate() accepts for input and output.
    std::pair<int, int> channelRange() noexcept;

    /// Inclusive bounds validate() accepts for both reconnect backoff fields, ms.
    std::pair<int, int> reconnectBackoffRange() noexcept;

    /// Inclusive bounds validate() accepts for translation.sessionMaxAgeSeconds
    /// (0 disables the proactive reopen).
    std::pair<int, int> sessionMaxAgeRange() noexcept;

    /// Inclusive bounds validate() accepts for
    /// translation.expirySafetyMarginSeconds (0 = reopen at the announced
    /// instant; protocol docs section 4bis).
    std::pair<int, int> expiryMarginRange() noexcept;

    /// Bounds validate() accepts for the developer fields: the mock
    /// echo delay (ms), the test-tone frequency (Hz) and level (dBFS).
    std::pair<int, int> mockLatencyRange() noexcept;
    std::pair<double, double> toneFrequencyRange() noexcept;
    std::pair<double, double> toneLevelRangeDb() noexcept;

    /// The audioSource values validate() accepts, for building a selector.
    std::vector<std::string> developerAudioSources();

    /// The longest instructions string validate() accepts, characters.
    std::size_t maxInstructionsLength() noexcept;

    /// The longest identifier-shaped field validate() accepts (model hint, NDI
    /// stream name), characters.
    std::size_t maxIdentifierLength() noexcept;

    /// Validation problems only; empty means the configuration is usable.
    ConfigProblems validate(const AppConfig& candidate);

    /// Single error string for callers that do not need per-field detail.
    bool validate(const AppConfig& candidate, std::string& error);

    /// JSON text for the config file. Never contains a secret-looking field.
    std::string toJsonText(const AppConfig& settings);

    /// Parses config text onto a copy of `fallback`, keeping the fallback value for
    /// every absent or invalid field. Returns false only when the text is not JSON
    /// at all; field-level problems end up in `problems` (partial recovery, never a
    /// crash and never a silent discard of the whole file).
    bool fromJsonText(std::string_view text,
                      AppConfig& out,
                      ConfigProblems& problems,
                      std::string& error,
                      const AppConfig& fallback = defaults());

    /// True when a key name refers to a credential and must not be stored or read
    /// from the ordinary configuration file.
    bool isSecretFieldName(std::string_view keyName) noexcept;

    /// Schema version found in JSON text without parsing the rest; 0 when absent
    /// or unreadable. Used by the store to refuse files from the future.
    std::uint32_t schemaVersionOf(std::string_view text) noexcept;

} // namespace config
} // namespace liveai
