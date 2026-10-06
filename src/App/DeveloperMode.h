#pragma once
//
// DeveloperMode - the single place that decides WHAT the developer settings
// mean. Pure functions over AppConfig: the composition root (Main.cpp) mounts
// backends according to the answer, the controller gates loopback through it,
// the UI badge and the diagnostics export render its summary - and none of
// those actors can disagree, because there is exactly one interpretation of
// the settings in the product.
//
// The isolation rule this file exists to enforce (isolation FAIL criterion:
// "mock behavior leaks into production"):
//
//   * A default configuration plans NOTHING: every field is false, the badge
//     is empty, and no caller has anything to mount. This is asserted by
//     tests, not trusted.
//   * When developer mode is off, every other developer field is INERT - even
//     `loopback = true` left in a file from an old rehearsal changes nothing.
//   * The `--dev` command-line preset turns on tone + mock and NEVER loopback:
//     routing a room's microphone to its own speakers is a settings-level,
//     two-intentional-clicks decision, never the side effect of a flag.
//   * Incoherent wishes degrade loudly, not silently: "wav" without a file
//     falls back to the real device and says so in the notes; a WAV file that
//     disagrees with the sample rate is a Start failure with the file's rate
//     named (the simulated device refuses, it does not convert).

#include <string>
#include <vector>

#include "Config/AppConfig.h"

namespace liveai {

/// The mounted meaning of the developer settings (plus the optional --dev
/// forcing). Read-only value: Main.cpp consumes it once at startup, the
/// controller keeps a copy as the authority for loopback and streaming
/// decisions, and tests assert it directly.
struct DeveloperPlan
{
    bool enabled = false;

    bool useWavSource = false;
    bool useToneSource = false;
    std::string wavInputPath;
    std::string wavOutputPath;   ///< recording target when non-empty (any source)
    double toneFrequencyHz = 1000.0;
    double toneLevelDb = -20.0;

    bool mockTranslation = false;
    int mockLatencyMs = 300;

    bool loopback = false;

    /// true = the capture goes to the audience, so the translation session is
    /// NOT fed (the two consumers of the input ring are mutually exclusive by
    /// design since the pipeline's first design - the plan makes the choice visible).
    bool captureStreamOff = false;

    /// Human-readable notes on degradations and conflicts. Printed into the
    /// log and the export; never empty when the plan deviated from what the
    /// settings literally said.
    std::vector<std::string> notes;

    /// The operator-facing badge: empty in production, unmistakable otherwise.
    std::string badge;
};

/// Builds the plan. `forcedByCommandLine` (the --dev flag) turns the mode on
/// with the tone+mock preset regardless of the settings file; it does not
/// rewrite the file, it does not enable loopback, and developer settings from
/// the file keep their meaning if the mode was already enabled there.
DeveloperPlan developerPlan(const AppConfig& settings, bool forcedByCommandLine = false);

} // namespace liveai
