#include "App/DeveloperMode.h"

#include <format>
#include <utility>

namespace liveai {

DeveloperPlan developerPlan(const AppConfig& settings, bool forcedByCommandLine)
{
    DeveloperPlan plan;
    const auto& dev = settings.developer;

    // The isolation gate, in one line: no forcing and the switch off means the
    // rest of the struct does not exist as far as this product is concerned.
    if (!forcedByCommandLine && !dev.enabled)
        return plan;

    plan.enabled = true;

    if (forcedByCommandLine && !dev.enabled)
    {
        // --dev preset: the tone + mock chain, chosen so that `LingoFlow.exe
        // --dev` is useful on a machine with neither ASIO device nor API key,
        // and loopback stays a deliberate settings edit - a flag that made the
        // room's speakers play the room's microphone would be exactly the
        // "mock behaviour leaks" incident this task must not ship.
        plan.useToneSource = true;
        plan.toneFrequencyHz = dev.toneFrequencyHz;
        plan.toneLevelDb = dev.toneLevelDb;
        plan.mockTranslation = true;
        plan.mockLatencyMs = dev.mockLatencyMs;
        plan.wavOutputPath = dev.wavOutputPath;   // an existing record path is honoured
        plan.notes.push_back("--dev preset: test tone + mock echo mounted; the settings "
                             "file was not changed and loopback was not enabled");
    }
    else
    {
        if (dev.audioSource == "wav")
        {
            if (dev.wavInputPath.empty())
            {
                // Validated away at load time already; checked again because
                // the plan is the last honest word before mounting, and
                // "mount nothing and say nothing" is not an option.
                plan.notes.push_back("developer audio source 'wav' named no file - "
                                     "falling back to the configured device");
            }
            else
            {
                plan.useWavSource = true;
                plan.wavInputPath = dev.wavInputPath;
            }
        }
        else if (dev.audioSource == "tone")
        {
            plan.useToneSource = true;
            plan.toneFrequencyHz = dev.toneFrequencyHz;
            plan.toneLevelDb = dev.toneLevelDb;
        }

        plan.wavOutputPath = dev.wavOutputPath;
        plan.mockTranslation = dev.mockTranslation;
        plan.mockLatencyMs = dev.mockLatencyMs;
        plan.loopback = dev.loopback;
    }

    plan.captureStreamOff = plan.loopback;

    std::vector<std::string> parts;
    parts.push_back("DEVELOPER MODE");

    if (plan.useWavSource)
        parts.push_back("audio source = WAV '" + plan.wavInputPath + "' (no device was opened)");
    else if (plan.useToneSource)
        parts.push_back(std::format("audio source = {} Hz tone at {:+.0f} dBFS (no device was opened)",
                                    plan.toneFrequencyHz, plan.toneLevelDb));
    else
        parts.push_back("audio source = real device");

    if (!plan.wavOutputPath.empty())
        parts.push_back("recording output to '" + plan.wavOutputPath + "'");

    if (plan.mockTranslation)
        parts.push_back(std::format("translation = mock echo +{} ms (OpenAI was NOT called)",
                                    plan.mockLatencyMs));
    else
        parts.push_back("translation = real provider chain");

    if (plan.loopback)
        parts.push_back("LOOPBACK: capture goes to the audience, the translator is NOT fed");

    plan.badge = parts.front();

    for (std::size_t i = 1; i < parts.size(); ++i)
        plan.badge += " | " + parts[i];

    for (const auto& note : plan.notes)
        plan.badge += " (" + note + ")";

    return plan;
}

} // namespace liveai
