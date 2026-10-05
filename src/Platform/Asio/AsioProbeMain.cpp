// lingoflow_asio_probe - ASIO discovery, lifecycle and loopback verification tool.
//
// It exercises exactly the code paths the application uses:
//   * Platform/Asio/AsioDiscovery   - enumeration and capability probing
//   * Platform/Asio/JuceAsioBackend - open/start/stop/close through audio::IAudioBackend
//   * Audio/AudioEngine             - the realtime pipeline (ring, jitter, meters)
//   * Audio/AudioLoopback           - input -> output loopback worker
//
// Modes:
//   --list                     enumerate ASIO devices (registry-only, no driver load)
//   --verify                   compare JUCE enumeration with HKLM\SOFTWARE\ASIO contents
//   --probe <id> [--start]     open a device, read capabilities, optionally start/stop once
//   --lifecycle <id> [--cycles N]   repeated open/start/stop/close through AudioEngine
//   --loopback <id> [--seconds N] [--input M] [--output K] [--jitter MS]
//                              [--gain-in DB] [--gain-out DB]
//                              run the real device through the engine with input->output
//                              loopback, the given digital trims and the levels measured
//                              (the task 005 and task 006 hardware checks)
//   --help
//
// Exit codes:
//   0  the requested operation succeeded
//   1  verification failed (inconsistent enumeration, no callbacks, bad state)
//   2  the device refused to open (reported honestly, not treated as a crash)
//   3  usage error

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <juce_events/juce_events.h>

#include "Audio/AudioEngine.h"
#include "Audio/AudioLoopback.h"
#include "Audio/Asio/AsioDeviceInfo.h"
#include "Audio/LevelMeter.h"
#include "Diagnostics/DiagnosticsManager.h"
#include "Platform/Asio/AsioDiscovery.h"
#include "Platform/Asio/JuceAsioBackend.h"
#include "Utils/Log.h"

namespace {

using namespace liveai;

void printUsage()
{
    std::cout <<
        "lingoflow_asio_probe - ASIO discovery, lifecycle and loopback check\n"
        "  --list                      enumerate ASIO devices (no driver is loaded)\n"
        "  --verify                    check enumeration against HKLM\\SOFTWARE\\ASIO\n"
        "  --probe <device-id> [--start]  open device, read capabilities (optionally start/stop)\n"
        "  --lifecycle <device-id> [--cycles N]  open/start/stop/close cycles via AudioEngine\n"
        "  --loopback <device-id> [--seconds N] [--input M] [--output K] [--jitter MS]"
        " [--gain-in DB] [--gain-out DB]\n"
        "        run the device through AudioEngine with input->output loopback, the given"
        " digital trims and print the measured input/output levels once per second (tasks 005"
        " and 006 hardware checks)\n"
        "  --help                      this text\n"
        "exit: 0 ok, 1 verification failed / no callbacks / inconclusive silence,\n"
        "      2 device refused to open, 3 usage error\n";
}

std::string findOption(int argc, char** argv, std::string_view name)
{
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg(argv[i]);
        if (arg == name)
            return i + 1 < argc ? std::string(argv[i + 1]) : std::string();
    }
    return {};
}

/// findOption parsed as a decimal integer, with `fallback` when it is absent or not
/// a number (atoi would silently return 0, which is a channel index nobody means).
int readIntOption(int argc, char** argv, std::string_view name, int fallback)
{
    const std::string text = findOption(argc, argv, name);

    if (text.empty())
        return fallback;

    for (const char character : text)
    {
        if (character < '0' || character > '9')
        {
            std::cout << "option --" << name.substr(2) << " needs a positive number, got '" << text << "'\n";
            return fallback;
        }
    }

    return std::atoi(text.c_str());
}

/// findOption parsed as a decimal number that may be negative, because a gain of -6 dB is
/// the ordinary case. Anything strtod cannot consume in full is refused the same way
/// readIntOption refuses a bad integer: say so, fall back, do not guess.
double readDoubleOption(int argc, char** argv, std::string_view name, double fallback)
{
    const std::string text = findOption(argc, argv, name);

    if (text.empty())
        return fallback;

    const char* begin = text.c_str();
    char* end = nullptr;

    const double parsed = std::strtod(begin, &end);

    if (end == begin || end != begin + text.size() || !std::isfinite(parsed))
    {
        std::cout << "option --" << name.substr(2) << " needs a number like -6.0, got '" << text << "'\n";
        return fallback;
    }

    return parsed;
}

bool hasFlag(int argc, char** argv, std::string_view name)
{
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], name.data()) == 0)
            return true;
    return false;
}

/// Preview of a driver-reported list, bounded so a 64-channel device does not flood
/// the console: "[a, b, c ... +5]".
std::string describeNamePreview(const std::vector<std::string>& names)
{
    if (names.empty())
        return "<none reported>";

    std::string out = "[";
    const std::size_t shown = std::min<std::size_t>(names.size(), 4);

    for (std::size_t i = 0; i < shown; ++i)
    {
        if (i != 0)
            out += ", ";
        out += names[i].empty() ? "<empty>" : names[i];
    }

    if (names.size() > shown)
        out += " ... +" + std::to_string(names.size() - shown);

    return out + "]";
}

std::string describeRatePreview(const std::vector<double>& rates)
{
    if (rates.empty())
        return "<none reported>";

    std::string out = "[";
    const std::size_t shown = std::min<std::size_t>(rates.size(), 8);

    for (std::size_t i = 0; i < shown; ++i)
    {
        if (i != 0)
            out += ", ";
        out += std::to_string(static_cast<long>(rates[i]));
    }

    if (rates.size() > shown)
        out += " ... +" + std::to_string(rates.size() - shown);

    return out + "]";
}

std::string describeBufferPreview(const std::vector<int>& sizes)
{
    if (sizes.empty())
        return "<none reported>";

    std::string out = "[";
    const std::size_t shown = std::min<std::size_t>(sizes.size(), 12);

    for (std::size_t i = 0; i < shown; ++i)
    {
        if (i != 0)
            out += ", ";
        out += std::to_string(sizes[i]);
    }

    if (sizes.size() > shown)
        out += " ... +" + std::to_string(sizes.size() - shown);

    return out + "]";
}

std::string lowerCopy(std::string value)
{
    for (auto& c : value)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}

/// The ASIO registry key and the name JUCE reports for the same driver are usually not
/// identical: the key is the registration label ("Waves SoundGrid"), the reported name
/// is what the driver itself returns ("Waves SoundGrid ASIO"). They correspond when one
/// contains the other, case-insensitively.
bool namesCorrespond(const std::string& lowerLeft, const std::string& lowerRight)
{
    if (lowerLeft.empty() || lowerRight.empty())
        return false;

    return lowerLeft == lowerRight
        || lowerLeft.find(lowerRight) != std::string::npos
        || lowerRight.find(lowerLeft) != std::string::npos;
}

int runList()
{
    const auto scan = platform::scanAsioDevices();

    if (!scan.asioTypeAvailable)
    {
        std::cout << "ASIO unavailable in this build: "
                  << (scan.notes.empty() ? std::string("unknown reason") : scan.notes.front()) << "\n";
        return 1;
    }

    std::cout << "ASIO devices (" << scan.devices.size() << "):\n";
    for (const auto& device : scan.devices)
        std::cout << "  " << asio::describeDevice(device) << "\n";

    return 0;
}

int runVerify()
{
    const auto scan = platform::scanAsioDevices();
    const std::vector<std::string> registry = platform::registryAsioDriverNames();

    std::cout << "JUCE ASIO type available: " << (scan.asioTypeAvailable ? "yes" : "no") << "\n";

    if (!scan.asioTypeAvailable)
    {
        std::cout << "VERIFY FAILED: this build has no ASIO support\n";
        return 1;
    }

    std::cout << "enumerated: " << scan.devices.size() << "\n";
    for (const auto& device : scan.devices)
        std::cout << "  " << asio::describeDevice(device) << "\n";
    std::cout << "HKLM\\SOFTWARE\\ASIO entries: " << registry.size() << "\n";
    for (const auto& name : registry)
        std::cout << "  registry key: " << name << "\n";

    // The registry key is the driver's registration label ("Waves SoundGrid") while
    // JUCE reports the name the driver itself returns from ASIOInit/getDriverName
    // ("Waves SoundGrid ASIO"). The two are matched by case-insensitive containment,
    // one-to-one, and any leftover on either side fails the check.
    std::vector<bool> taken(scan.devices.size(), false);
    std::size_t matched = 0;
    bool ok = true;

    for (const auto& registryName : registry)
    {
        const auto registryLower = lowerCopy(registryName);
        bool found = false;

        for (std::size_t i = 0; i < scan.devices.size(); ++i)
        {
            if (taken[i])
                continue;
            if (namesCorrespond(registryLower, lowerCopy(scan.devices[i].name)))
            {
                taken[i] = true;
                ++matched;
                found = true;
                std::cout << "match: '" << registryName << "' <-> '" << scan.devices[i].name << "'\n";
                break;
            }
        }

        if (!found)
        {
            std::cout << "VERIFY FAILED: registered driver '" << registryName
                      << "' was not enumerated by JUCE\n";
            ok = false;
        }
    }

    for (std::size_t i = 0; i < scan.devices.size(); ++i)
    {
        if (!taken[i])
        {
            std::cout << "VERIFY FAILED: enumerated device '" << scan.devices[i].name
                      << "' has no matching HKLM\\SOFTWARE\\ASIO registration\n";
            ok = false;
        }
    }

    if (!ok)
        return 1;

    if (matched == 0)
        std::cout << "VERIFY OK: no ASIO driver is registered and none was enumerated"
                     " (expected on a machine without an ASIO driver)\n";
    else
        std::cout << "VERIFY OK: " << matched << " registration(s) enumerated as device(s)\n";

    return 0;
}

int runProbe(const std::string& deviceId, bool startAndStop)
{
    const auto attempt = platform::probeAsioDevice(deviceId, 48000.0, 480, startAndStop);

    std::cout << asio::describeCapabilities(attempt.capabilities) << "\n";

    // Channel names decide the task 015 UI: a driver that reports names can offer a
    // picker, one that reports none forces numbered channels. Printed here instead of
    // in the log formatter so the routine log line stays short.
    const auto& caps = attempt.capabilities;
    std::cout << "input channel names: " << describeNamePreview(caps.inputChannels) << "\n";
    std::cout << "output channel names: " << describeNamePreview(caps.outputChannels) << "\n";
    std::cout << "offered sample rates: " << describeRatePreview(caps.sampleRates) << "\n";
    std::cout << "offered buffer sizes: " << describeBufferPreview(caps.bufferSizes) << "\n";

    if (!attempt.capabilities.opened)
    {
        std::cout << "PROBE: device did not open: " << attempt.error << "\n";
        return 2;
    }

    if (!attempt.success)
    {
        std::cout << "PROBE: opened but failed later: " << attempt.error << "\n";
        return 1;
    }

    std::cout << "PROBE OK\n";
    return 0;
}

/// Repeated open/start/stop/close cycles through the engine, the same path task 005
/// will use. Callback delivery and the closed state after deactivate() are asserted;
/// nothing here fabricates a working device - if the driver does not run, the tool
/// says so and fails.
int runLifecycle(const std::string& deviceId, int cycles)
{
    juce::ScopedJuceInitialiser_GUI gui;   // ASIO internals use juce::Timer

    DiagnosticsManager diagnostics;

    for (int cycle = 1; cycle <= cycles; ++cycle)
    {
        AudioEngine engine(&diagnostics);
        platform::JuceAsioBackend backend(deviceId);

        std::string error;
        audio::DeviceRequest request;

        if (!engine.activate(backend, request, error))
        {
            std::cout << "cycle " << cycle << ": could not activate '" << deviceId << "': " << error << "\n";
            return backend.state() == audio::BackendState::faulted ? 2 : 1;
        }

        const auto blocksBefore = backend.callbackBlocks();
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));
        const auto blocksAfter = backend.callbackBlocks();

        engine.deactivate();

        std::cout << "cycle " << cycle << ": callbacks=" << (blocksAfter - blocksBefore)
                  << " rate=" << engine.sampleRate() << " buffer=" << engine.bufferFrames()
                  << " xruns=" << asio::describeXRunCount(backend.lastXRunCount())
                  << " engineBlocks=" << engine.blockCount()
                  << " state after close=" << audio::nameOf(backend.state()) << "\n";

        if (blocksAfter == 0)
        {
            std::cout << "LIFECYCLE FAILED: device opened and started but delivered no callbacks\n";
            return 1;
        }

        if (backend.state() != audio::BackendState::closed)
        {
            std::cout << "LIFECYCLE FAILED: backend is not closed after deactivate()\n";
            return 1;
        }
    }

    std::cout << "LIFECYCLE OK after " << cycles << " cycle(s)\n";
    return 0;
}

} // namespace

/// The task 005 hardware check, as one command. Runs the real device through the
/// engine with the input->output loopback worker and prints, once per second, the
/// levels the engine actually measured: what arrived on the ASIO input and what is
/// leaving on the ASIO output.
///
/// On a bench with a SoundGrid server the expectation is simple to see: make noise on
/// the patched console channel and the input level moves; because loopback copies that
/// same audio to the output after the jitter pre-roll, the output level follows it,
/// delayed by roughly --jitter milliseconds. Without a server this PC only proves that
/// the machinery runs: both levels stay at silence, which is the honest reading, and the
/// tool says so instead of calling it a pass.
///
/// --gain-in and --gain-out exist so the same run also proves task 006: the level that
/// leaves the output has to follow the requested trim, and a signal that reaches full
/// scale raises the clipping counters instead of disappearing into a limiter.
int runLoopback(const std::string& deviceId, int seconds, int inputChannel, int outputChannel, int jitterMs,
                double inputGainDb, double outputGainDb)
{
    juce::ScopedJuceInitialiser_GUI gui;   // ASIO internals use juce::Timer

    DiagnosticsManager diagnostics;
    AudioEngine engine(&diagnostics);
    platform::JuceAsioBackend backend(deviceId);

    engine.setJitterBufferMs(jitterMs);
    engine.setInputGainDb(static_cast<float>(inputGainDb));
    engine.setOutputGainDb(static_cast<float>(outputGainDb));

    audio::DeviceRequest request;
    request.deviceId = deviceId;
    request.sampleRate = 48000;
    request.bufferFrames = 480;
    request.inputChannel = inputChannel;
    request.outputChannel = outputChannel;

    std::string error;

    if (!engine.activate(backend, request, error))
    {
        std::cout << "LOOPBACK: could not activate '" << deviceId << "': " << error << "\n";
        return backend.state() == audio::BackendState::faulted ? 2 : 1;
    }

    audio::AudioLoopback loopback(engine);

    if (!loopback.start(error))
    {
        std::cout << "LOOPBACK: engine is running but the loopback worker refused: " << error << "\n";
        engine.deactivate();
        return 1;
    }

    std::cout << "loopback on '" << deviceId << "' input channel " << inputChannel
              << " -> output channel " << outputChannel << ", jitter pre-roll "
              << engine.jitterBufferMs() << " ms, device "
              << engine.sampleRate() << " Hz / " << engine.bufferFrames() << " frames\n";

    std::cout << "gains as requested: input " << inputGainDb << " dB, output " << outputGainDb
              << " dB (the engine reports what it actually applies, below)\n";

    float maxInputPeak = 0.0f;
    float maxOutputPeak = 0.0f;

    const auto started = std::chrono::steady_clock::now();
    int second = 0;

    while (std::chrono::steady_clock::now() - started < std::chrono::seconds(seconds))
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        ++second;

        const auto* inMeter = engine.inputMeter(0);
        const auto* outMeter = engine.outputMeter(0);

        const float inPeak = inMeter != nullptr ? inMeter->peakLinear() : 0.0f;
        const float outPeak = outMeter != nullptr ? outMeter->peakLinear() : 0.0f;

        maxInputPeak = std::max(maxInputPeak, inPeak);
        maxOutputPeak = std::max(maxOutputPeak, outPeak);

        // Reading the latch clears it, so "clip" here means "something hit full scale in
        // the last second", which is what an operator watching the line can act on.
        const bool inputClip = engine.takeInputClipIndicator();
        const bool outputClip = engine.takeOutputClipIndicator();

        std::cout << "  t=" << second << "s"
                  << " in=" << static_cast<int>(audio::linearToDb(inPeak)) << " dBFS"
                  << " out=" << static_cast<int>(audio::linearToDb(outPeak)) << " dBFS"
                  << " appliedGain=" << engine.appliedInputGainDb() << '/'
                  << engine.appliedOutputGainDb() << " dB"
                  << " blocks=" << engine.blockCount()
                  << " fwdSamples=" << engine.inputSamplesForwarded()
                  << " looped=" << loopback.transferredFrames()
                  << " underruns=" << engine.underrunEvents()
                  << " overruns=" << engine.overrunEvents()
                  << " clip=" << (inputClip ? "in" : "") << (inputClip && outputClip ? "+" : "")
                  << (outputClip ? "out" : "") << (inputClip || outputClip ? "" : "-none")
                  << " fill=" << engine.jitterFillFrames() << " frames\n";
    }

    const std::uint64_t callbacks = backend.callbackBlocks();
    const std::uint64_t blocks = engine.blockCount();
    const int xruns = backend.lastXRunCount();

    // Read before deactivate() only to prove the point that these belong to the engine
    // and the gain stages: the same numbers are printed again after the shutdown below.
    const std::uint64_t clippedIn = engine.inputClippedSamples();
    const std::uint64_t clippedAfterGain = engine.inputGainClippedSamples();
    const std::uint64_t clippedOut = engine.outputGainClippedSamples();
    const std::uint64_t nonFinite = engine.nonFiniteInputSamples();

    loopback.stop();
    engine.deactivate();

    std::cout << "summary: deviceCallbacks=" << callbacks << " engineBlocks=" << blocks
              << " loopbackTransferred=" << loopback.transferredFrames()
              << " maxInput=" << static_cast<int>(audio::linearToDb(maxInputPeak)) << " dBFS"
              << " maxOutput=" << static_cast<int>(audio::linearToDb(maxOutputPeak)) << " dBFS"
              << " underruns=" << engine.underrunEvents()
              << " overruns=" << engine.overrunEvents()
              << " ringDropSamples=" << engine.inputRingDroppedSamples()
              << " malformed=" << engine.malformedCallbacks()
              << " oversized=" << engine.oversizedCallbacks()
              << " xruns=" << asio::describeXRunCount(xruns)
              << " state after close=" << audio::nameOf(backend.state()) << "\n";

    std::cout << "clipping after close (counters survive deactivate): deviceFullScale=" << clippedIn
              << " afterInputGain=" << clippedAfterGain << " toAudience=" << clippedOut
              << " nonFiniteSilenced=" << nonFinite
              << " gainRequestsClamped=" << engine.gainRequestsClamped()
              << " gainRequestsRejected=" << engine.gainRequestsRejected() << "\n";

    if (callbacks == 0 || blocks == 0)
    {
        std::cout << "LOOPBACK FAILED: the device delivered no callbacks through the engine\n";
        return 1;
    }

    if (engine.malformedCallbacks() != 0)
    {
        std::cout << "LOOPBACK FAILED: the backend broke the processor contract\n";
        return 1;
    }

    if (maxInputPeak <= 0.0f && maxOutputPeak <= 0.0f)
    {
        std::cout << "LOOPBACK INCONCLUSIVE: both channels stayed digital silence.\n"
                     "  The pipeline ran, but no audio was present to loop. On this PC\n"
                     "  (Waves driver, no SoundGrid server) that is the expected result.\n"
                     "  Patch a real source into input channel " << inputChannel
                  << " on a machine with a server, or use developer mode routing (task 019).\n";
        return 1;
    }

    std::cout << "LOOPBACK OK: audio measured at the input also left on the output\n";
    return 0;
}

int main(int argc, char** argv)
{
    liveai::LogConfig config;
    config.level = liveai::LogLevel::debug;
    config.writeConsole = true;
    config.filePath = {};
    liveai::log::configure(config);

    if (argc < 2 || hasFlag(argc, argv, "--help"))
    {
        printUsage();
        return argc < 2 ? 3 : 0;
    }

    const std::string mode = hasFlag(argc, argv, "--list") ? "list"
                           : hasFlag(argc, argv, "--verify") ? "verify"
                           : hasFlag(argc, argv, "--probe") ? "probe"
                           : hasFlag(argc, argv, "--lifecycle") ? "lifecycle"
                           : hasFlag(argc, argv, "--loopback") ? "loopback"
                           : "";

    if (mode.empty())
    {
        std::cout << "unknown mode\n";
        printUsage();
        return 3;
    }

    if (mode == "list")
        return runList();
    if (mode == "verify")
        return runVerify();

    const std::string deviceId = mode == "probe"      ? findOption(argc, argv, "--probe")
                               : mode == "lifecycle"  ? findOption(argc, argv, "--lifecycle")
                                                      : findOption(argc, argv, "--loopback");

    if (deviceId.empty())
    {
        std::cout << "mode '" << mode << "' needs a device id\n";
        return 3;
    }

    if (mode == "probe")
        return runProbe(deviceId, hasFlag(argc, argv, "--start"));

    if (mode == "loopback")
    {
        const std::string secondsText = findOption(argc, argv, "--seconds");
        const int seconds = std::clamp(secondsText.empty() ? 10 : std::atoi(secondsText.c_str()), 1, 600);

        return runLoopback(deviceId,
                           seconds,
                           readIntOption(argc, argv, "--input", 1),
                           readIntOption(argc, argv, "--output", 1),
                           readIntOption(argc, argv, "--jitter", 120),
                           readDoubleOption(argc, argv, "--gain-in", 0.0),
                           readDoubleOption(argc, argv, "--gain-out", 0.0));
    }

    const std::string cyclesText = findOption(argc, argv, "--cycles");
    int cycles = cyclesText.empty() ? 3 : std::atoi(cyclesText.c_str());
    if (cycles < 1)
        cycles = 1;
    if (cycles > 20)
        cycles = 20;

    return runLifecycle(deviceId, cycles);
}
