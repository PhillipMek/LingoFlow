#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "Audio/Asio/AsioDeviceInfo.h"

using namespace liveai;
using liveai::asio::BufferChoice;
using liveai::asio::DeviceCapabilitiesReport;
using liveai::asio::DeviceEntry;
using liveai::asio::RateChoice;
using liveai::asio::selectConfiguration;

namespace {

std::vector<double> rates48{ 44100.0, 48000.0, 96000.0 };
std::vector<int> buffers{ 128, 256, 512, 1024 };

DeviceCapabilitiesReport openedReport()
{
    DeviceCapabilitiesReport report;
    report.name = "Waves SoundGrid";
    report.opened = true;
    report.inputChannels = { "In 1", "In 2", "In 3" };
    report.outputChannels = { "Out 1", "Out 2" };
    return report;
}

} // namespace

TEST_CASE("AsioDeviceInfo: exact rate and buffer are preferred", "[audio][asio][policy]")
{
    const auto selection = selectConfiguration(rates48, buffers, 48000.0, 512);

    CHECK(selection.sampleRate.has_value());
    CHECK(*selection.sampleRate == 48000.0);
    CHECK(selection.rateChoice == RateChoice::exactMatch);
    CHECK(selection.bufferFrames.has_value());
    CHECK(*selection.bufferFrames == 512);
    CHECK(selection.bufferChoice == BufferChoice::exactMatch);
    CHECK(selection.notes.empty());
}

TEST_CASE("AsioDeviceInfo: missing values fall back to the nearest, and it is recorded",
          "[audio][asio][policy]")
{
    const auto selection = selectConfiguration(rates48, buffers, 32000.0, 300);

    CHECK(selection.rateChoice == RateChoice::nearestAvailable);
    CHECK(*selection.sampleRate == 44100.0);          // nearest to 32000
    CHECK(selection.bufferChoice == BufferChoice::nearestAvailable);
    CHECK(*selection.bufferFrames == 256);           // nearest to 300
    REQUIRE(selection.notes.size() == 2);
    INFO(selection.notes[0] + " | " + selection.notes[1]);
    CHECK(selection.notes[0].find("not offered") != std::string::npos);
}

TEST_CASE("AsioDeviceInfo: an empty capability list is reported, never invented",
          "[audio][asio][policy][honesty]")
{
    const auto selection = selectConfiguration({}, {}, 48000.0, 480);

    CHECK_FALSE(selection.sampleRate.has_value());
    CHECK_FALSE(selection.bufferFrames.has_value());
    CHECK(selection.rateChoice == RateChoice::noneAvailable);
    CHECK(selection.bufferChoice == BufferChoice::noneAvailable);
    CHECK(selection.notes.size() == 2);
    CHECK(selection.notes[0].find("no sample rates") != std::string::npos);
    CHECK(selection.notes[1].find("no buffer sizes") != std::string::npos);
}

TEST_CASE("AsioDeviceInfo: single-value device lists still select that value",
          "[audio][asio][policy]")
{
    const auto selection = selectConfiguration({ 48000.0 }, { 480 }, 44100.0, 512);

    CHECK(*selection.sampleRate == 48000.0);
    CHECK(*selection.bufferFrames == 480);
    CHECK(selection.rateChoice == RateChoice::nearestAvailable);
    CHECK(selection.bufferChoice == BufferChoice::nearestAvailable);
}

TEST_CASE("AsioDeviceInfo: channel selection is validated against the device",
          "[audio][asio][policy]")
{
    const auto report = openedReport();
    std::string error;

    CHECK(asio::validateChannelSelection(report, 1, 1, error));
    CHECK(error.empty());

    CHECK(asio::validateChannelSelection(report, 3, 2, error));

    CHECK_FALSE(asio::validateChannelSelection(report, 4, 1, error));
    CHECK(error.find("input channel 4") != std::string::npos);
    CHECK(error.find("3 channel") != std::string::npos);

    CHECK_FALSE(asio::validateChannelSelection(report, 1, 3, error));
    CHECK(error.find("output channel 3") != std::string::npos);

    CHECK_FALSE(asio::validateChannelSelection(report, 0, 1, error));
    CHECK(error.find("1-based") != std::string::npos);

    CHECK_FALSE(asio::validateChannelSelection(report, -1, 1, error));
}

TEST_CASE("AsioDeviceInfo: capabilities of an unopened device cannot validate channels",
          "[audio][asio][policy]")
{
    DeviceCapabilitiesReport closed;
    closed.name = "Waves SoundGrid";
    std::string error;

    CHECK_FALSE(asio::validateChannelSelection(closed, 1, 1, error));
    CHECK(error.find("never opened") != std::string::npos);
}

TEST_CASE("AsioDeviceInfo: descriptions never invent numbers", "[audio][asio][policy][honesty]")
{
    DeviceEntry entry;
    entry.id = "Waves SoundGrid";
    entry.name = "Waves SoundGrid";
    entry.driverName = "ASIO";
    entry.registered = true;
    entry.driverLoaded = false;
    entry.note = "registry-only scan; the driver DLL was not loaded";

    const auto text = asio::describeDevice(entry);
    INFO(text);
    CHECK(text.find("Waves SoundGrid") != std::string::npos);
    CHECK(text.find("registered=yes") != std::string::npos);
    CHECK(text.find("driverLoaded=no") != std::string::npos);
    CHECK(text.find("backend=ASIO") != std::string::npos);
    CHECK(text.find("driver DLL was not loaded") != std::string::npos);

    DeviceCapabilitiesReport report;
    report.name = "Waves SoundGrid";
    const auto notOpened = asio::describeCapabilities(report);
    INFO(notOpened);
    CHECK(notOpened.find("not opened") != std::string::npos);
    CHECK(notOpened.find("rate=") == std::string::npos);   // no invented rate

    report.opened = true;
    report.inputChannels = { "In 1" };
    report.outputChannels = { "Out 1" };
    const auto opened = asio::describeCapabilities(report);
    INFO(opened);
    CHECK(opened.find("opened") != std::string::npos);
    CHECK(opened.find("rate=unknown") != std::string::npos);   // still not invented
    CHECK(opened.find("in=1 out=1") != std::string::npos);
}
