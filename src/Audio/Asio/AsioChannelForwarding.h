#pragma once
//
// ASIO channel forwarding - the one portable decision about what the driver's
// channel-pointer array means, free of JUCE types so it can be unit-tested.
//
// Why this decision exists at all (code review P0, 2026-10-05): the engine must
// receive exactly the operator's selected channels as logical views 0..n-1. A
// driver callback hands over "an array plus its length" - and what the entries
// are indexed BY is a per-device-type implementation detail. Vendored JUCE
// 9.0.3's ASIO wrapper compacts: it creates driver buffers for active channels
// only, in ascending physical order (native/juce_ASIO_windows.cpp: resetBuffers
// lines 891-925, the createBuffers fill loop lines 510-555, processBuffer's
// call lines 1349-1360), and passes numChannels == number of active channels.
// The generic AudioIODeviceCallback contract does NOT promise that - it only
// documents that entries may be null for channels the callback has no data
// for - and another JUCE tree (the supported LIVEAI_JUCE_PATH build option) or
// another device path may deliver a PHYSICALLY indexed array instead. Trusting
// compaction is trusting one file of one version; trusting it wrongly turns
// "operator selected channel 8" into "translator listens to channel 1" with no
// warning anywhere. So the shape is OBSERVED from the array length, and the
// mapping rules below are the ones the tests pin.
//
// Realtime-safe by construction: fixed work over pre-sized containers, no
// allocation, no locking, no strings. Listed in the realtime safety audit
// (tests/RealtimeSafetyAudit.cmake).

#include <cstddef>
#include <vector>

namespace liveai {
namespace asio {

/// What the driver's array length said about its own layout, compared against
/// the number of active channels.
enum class ChannelArrayShape
{
    compacted,         ///< exactly one entry per active channel, in map order
    physicallyIndexed, ///< more entries than active channels: indexed by physical position
    truncated,         ///< fewer entries than active channels: the tail has no data
};

/// Writes one entry per active channel into `views`, which the caller sized to
/// `physical.size()` on a non-realtime thread. `physical[k]` is the zero-based
/// driver index of logical channel k, ascending; `driverData` is non-null (the
/// callback's own guard covers a null array before this runs).
///
/// Deciding by length is sound: a compacted array always has exactly
/// physical.size() entries, so a longer array cannot be one; the only situation
/// where the two rules could disagree is a physically indexed array that omits
/// inactive channels - which means every physical channel up to the last active
/// one IS active, and then physical[k] == k and both rules select the same
/// entry. Null entries pass through as null - and they are NOT silence to mix:
/// AudioEngine::processAudio counts any null among the promised views as a
/// malformed callback (code review P2, 2026-10-05), so a selected channel that
/// vanished mid-show is reported in the operator's counters, never quietly
/// dropped from the mix.
template <typename ChannelPtr>
ChannelArrayShape forwardActiveChannels(ChannelPtr* const* driverData,
                                        int driverChannelCount,
                                        const std::vector<int>& physical,
                                        std::vector<ChannelPtr*>& views) noexcept
{
    if (driverChannelCount < 0)
        driverChannelCount = 0;

    const int logical = static_cast<int>(physical.size());
    ChannelArrayShape shape = ChannelArrayShape::compacted;

    if (driverChannelCount > logical)
    {
        shape = ChannelArrayShape::physicallyIndexed;
        for (int k = 0; k < logical; ++k)
        {
            const int p = physical[static_cast<std::size_t>(k)];
            views[static_cast<std::size_t>(k)] = (p >= 0 && p < driverChannelCount)
                                                     ? driverData[p]
                                                     : nullptr;
        }
    }
    else if (driverChannelCount < logical)
    {
        shape = ChannelArrayShape::truncated;
        for (int k = 0; k < driverChannelCount; ++k)
            views[static_cast<std::size_t>(k)] = driverData[k];
        for (int k = driverChannelCount; k < logical; ++k)
            views[static_cast<std::size_t>(k)] = nullptr;
    }
    else
    {
        for (int k = 0; k < logical; ++k)
            views[static_cast<std::size_t>(k)] = driverData[k];
    }

    // views must be sized to the map; if a rebuilt map ever outgrew the vector
    // (only audioDeviceAboutToStart rebuilds both, so it cannot), the extra
    // entries are nulled rather than left pointing at a stale buffer.
    for (std::size_t k = static_cast<std::size_t>(logical); k < views.size(); ++k)
        views[k] = nullptr;

    return shape;
}

} // namespace asio
} // namespace liveai
