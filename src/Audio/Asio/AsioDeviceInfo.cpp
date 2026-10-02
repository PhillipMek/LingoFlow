#include "Audio/Asio/AsioDeviceInfo.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>

namespace liveai {
namespace asio {
namespace {

double nearestRate(const std::vector<double>& available, double requested)
{
    double best = 0.0;
    double bestDistance = std::numeric_limits<double>::max();

    for (const auto rate : available)
    {
        const double distance = std::abs(rate - requested);
        if (distance < bestDistance)
        {
            bestDistance = distance;
            best = rate;
        }
    }

    return best;
}

int nearestBuffer(const std::vector<int>& available, int requested)
{
    int best = 0;
    int bestDistance = std::numeric_limits<int>::max();

    for (const auto size : available)
    {
        const int distance = std::abs(size - requested);
        if (distance < bestDistance)
        {
            bestDistance = distance;
            best = size;
        }
    }

    return best;
}

} // namespace

Selection selectConfiguration(const std::vector<double>& availableSampleRates,
                             const std::vector<int>& availableBufferSizes,
                             double requestedSampleRate,
                             int requestedBufferFrames)
{
    Selection selection;

    if (availableSampleRates.empty())
    {
        selection.notes.push_back("device reported no sample rates");
    }
    else
    {
        const auto exact = std::find(availableSampleRates.begin(), availableSampleRates.end(), requestedSampleRate);

        if (exact != availableSampleRates.end())
        {
            selection.sampleRate = requestedSampleRate;
            selection.rateChoice = RateChoice::exactMatch;
        }
        else
        {
            const double chosen = nearestRate(availableSampleRates, requestedSampleRate);
            selection.sampleRate = chosen;
            selection.rateChoice = RateChoice::nearestAvailable;
            selection.notes.push_back("sample rate " + std::to_string(static_cast<long>(requestedSampleRate))
                                      + " Hz not offered, using " + std::to_string(static_cast<long>(chosen)) + " Hz");
        }
    }

    if (availableBufferSizes.empty())
    {
        selection.notes.push_back("device reported no buffer sizes");
    }
    else
    {
        const auto exact = std::find(availableBufferSizes.begin(), availableBufferSizes.end(), requestedBufferFrames);

        if (exact != availableBufferSizes.end())
        {
            selection.bufferFrames = requestedBufferFrames;
            selection.bufferChoice = BufferChoice::exactMatch;
        }
        else
        {
            const int chosen = nearestBuffer(availableBufferSizes, requestedBufferFrames);
            selection.bufferFrames = chosen;
            selection.bufferChoice = BufferChoice::nearestAvailable;
            selection.notes.push_back("buffer " + std::to_string(requestedBufferFrames) + " frames not offered, using "
                                      + std::to_string(chosen) + " frames");
        }
    }

    return selection;
}

std::string describeDevice(const DeviceEntry& entry)
{
    std::ostringstream out;
    out << "ASIO device '" << entry.name << "' id=" << entry.id
        << " registered=" << (entry.registered ? "yes" : "no")
        << " driverLoaded=" << (entry.driverLoaded ? "yes" : "no");

    if (!entry.driverName.empty())
        out << " backend=" << entry.driverName;
    if (!entry.note.empty())
        out << " note=" << entry.note;

    return out.str();
}

std::string describeCapabilities(const DeviceCapabilitiesReport& report)
{
    std::ostringstream out;
    out << "device '" << report.name << "' ";

    if (!report.opened)
    {
        out << "not opened";
        if (!report.error.empty())
            out << " (" << report.error << ")";
        return out.str();
    }

    out << "opened: channels in=" << report.inputChannels.size() << " out=" << report.outputChannels.size();

    if (report.selectedSampleRate.has_value())
        out << " rate=" << static_cast<long>(*report.selectedSampleRate);
    else
        out << " rate=unknown";

    if (report.selectedBufferFrames.has_value())
        out << " buffer=" << *report.selectedBufferFrames;
    else
        out << " buffer=unknown";

    // Latencies are only meaningful when the driver actually reported them.
    out << " latency in=" << report.inputLatencySamples << " out=" << report.outputLatencySamples
        << " bits=" << report.bitDepth
        << " rates=" << report.sampleRates.size() << " buffers=" << report.bufferSizes.size();

    return out.str();
}

std::string describeXRunCount(int reportedValue)
{
    if (reportedValue < 0)
        return "not reported by this driver";
    return std::to_string(reportedValue);
}

bool validateChannelSelection(const DeviceCapabilitiesReport& report,
                              int inputChannel,
                              int outputChannel,
                              std::string& error)
{
    error.clear();

    if (!report.opened)
    {
        error = "device capabilities are unknown: the device was never opened";
        return false;
    }

    const auto check = [&error](int index, const std::vector<std::string>& channels, const char* what)
    {
        if (index < 1)
        {
            error = std::string(what) + " channel index must be 1-based and positive";
            return false;
        }

        if (static_cast<std::size_t>(index) > channels.size())
        {
            error = std::string(what) + " channel " + std::to_string(index) + " does not exist: the device reported "
                  + std::to_string(channels.size()) + " channel(s)";
            return false;
        }

        return true;
    };

    if (!check(inputChannel, report.inputChannels, "input"))
        return false;
    if (!check(outputChannel, report.outputChannels, "output"))
        return false;

    return true;
}

} // namespace asio
} // namespace liveai
