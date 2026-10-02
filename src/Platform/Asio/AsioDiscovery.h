#pragma once
//
// ASIO device discovery - JUCE adapter (platform layer).
//
// This is the only place in the product that uses JUCE's ASIO backend. It fills the
// JUCE-free model in Audio/Asio/AsioDeviceInfo.h, so all selection logic stays
// testable without a driver.
//
// Safety notes (measured against JUCE 9.0.3, third_party/JUCE):
//   * scan() only reads the ASIO registry (ASIOAudioIODeviceType::scanForDevices),
//     it never loads a driver DLL - safe for automated tests.
//   * probe()/open paths call CoCreateInstance + ASIOInit. On a machine with the
//     Waves SoundGrid driver but no SoundGrid server this is expected to fail, and it
//     may take seconds. It is therefore opt-in, never part of the default test run.

#include <string>
#include <vector>

#include "Audio/Asio/AsioDeviceInfo.h"

namespace liveai {
namespace platform {

/// Result of an ASIO scan.
struct ScanResult
{
    std::vector<asio::DeviceEntry> devices;
    bool asioTypeAvailable = false;      ///< false = JUCE built without ASIO or no ASIO at all
    std::vector<std::string> notes;
};

/// Enumerates ASIO devices through JUCE. Non-blocking, registry-only.
ScanResult scanAsioDevices();

/// Names registered under HKLM\SOFTWARE\ASIO (+ WOW6432Node), read without JUCE.
/// Used to cross-check that the JUCE enumeration sees the same registrations.
std::vector<std::string> registryAsioDriverNames();

/// Opens a device to read its real capabilities and closes it again.
/// `startAndStop` additionally starts and stops the callback once.
asio::OpenAttempt probeAsioDevice(const std::string& deviceId,
                                  double requestedSampleRate,
                                  int requestedBufferFrames,
                                  bool startAndStop);

/// A short label for logs, e.g. "ASIO discovery".
std::string_view discoveryBackendName() noexcept;

} // namespace platform
} // namespace liveai
