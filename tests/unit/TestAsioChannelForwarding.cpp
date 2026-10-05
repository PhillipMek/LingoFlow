// Code review P0 (2026-10-05): the ASIO callback must never confuse "the k-th
// active channel" with "driver array entry k". The forwarding decision lives in
// Audio/Asio/AsioChannelForwarding.h - pure, portable, and here pinned under
// BOTH channel-array conventions: the compacted one the vendored JUCE ASIO
// wrapper actually delivers today, and the physically indexed one the generic
// callback contract allows. The review's own scenario - the operator selects
// channel 8 on a 32-channel device - is a test case, not a comment.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <vector>

#include "Audio/Asio/AsioChannelForwarding.h"

namespace {

using liveai::asio::ChannelArrayShape;
using liveai::asio::forwardActiveChannels;

/// One frame of storage per physical channel; the ADDRESS of the buffer is what
/// the forwarding decision routes, so the frame contents only identify channels.
struct DriverBuffers
{
    static constexpr int kChannels = 32;

    DriverBuffers()
    {
        buffers.reserve(kChannels);
        for (auto& frame : frames)
            buffers.push_back(frame.data());
    }

    std::array<std::array<float, 1>, kChannels> frames{};
    std::vector<float*> buffers;  ///< physically indexed: buffers[p] is channel p
};

/// A physically indexed driver array of `total` entries: every active physical
/// channel points at its real buffer, every inactive one at null - the shape a
/// non-compacting callback contract permits.
std::vector<const float*> sparsePhysical(int total, const std::vector<int>& active,
                                         const DriverBuffers& d)
{
    std::vector<const float*> array(static_cast<std::size_t>(total), nullptr);
    for (const int p : active)
        array[static_cast<std::size_t>(p)] = d.buffers[static_cast<std::size_t>(p)];
    return array;
}

} // namespace

TEST_CASE("channel forwarding: compacted array - the layout vendored JUCE ASIO delivers",
          "[audio][asio][review]")
{
    DriverBuffers d;
    const std::vector<int> physical = { 7 };  // operator selected input channel 8

    // A compacted array carries the ACTIVE channels only, in map order: entry 0
    // is the driver buffer JUCE created for physical channel 7.
    const float* compacted[] = { d.buffers[7] };
    std::vector<const float*> views(1, nullptr);

    REQUIRE(forwardActiveChannels(compacted, 1, physical, views) == ChannelArrayShape::compacted);
    CHECK(views[0] == d.buffers[7]);
}

TEST_CASE("channel forwarding: physically indexed array - channel 8 comes from entry 7, not entry 0",
          "[audio][asio][review]")
{
    DriverBuffers d;
    const std::vector<int> physical = { 7 };  // operator selected input channel 8

    // Inactive channels are null. Reading entry 0 here - what an unverified
    // compacting assumption would do against this layout - is silence, the
    // "system looks connected but nothing translates" failure this review
    // called P0. The map makes the callback take entry 7 instead.
    const std::vector<const float*> array = sparsePhysical(DriverBuffers::kChannels, { 7 }, d);
    std::vector<const float*> views(1, nullptr);

    REQUIRE(forwardActiveChannels(array.data(), static_cast<int>(array.size()), physical, views)
            == ChannelArrayShape::physicallyIndexed);
    CHECK(views[0] == d.buffers[7]);
    CHECK(views[0] != nullptr);
}

TEST_CASE("channel forwarding: single-channel selections 1, 2, 8 and 32 survive both conventions",
          "[audio][asio][review]")
{
    // No SECTIONs here: a section inside this loop would run once, not per
    // iteration, and a fresh DriverBuffers per iteration makes every run a
    // different set of addresses. Both conventions are checked in plain order.
    for (const int p : { 0, 1, 7, 31 })  // the review's one-based channels 1, 2, 8, 32
    {
        DriverBuffers d;
        const std::vector<int> physical = { p };
        const float* firstEntry = d.buffers[static_cast<std::size_t>(p)];

        {
            std::vector<const float*> views(1, nullptr);
            const float* compacted[] = { firstEntry };
            REQUIRE(forwardActiveChannels(compacted, 1, physical, views) == ChannelArrayShape::compacted);
            CHECK(views[0] == firstEntry);
        }
        {
            std::vector<const float*> views(1, nullptr);
            const std::vector<const float*> array = sparsePhysical(DriverBuffers::kChannels, { p }, d);
            REQUIRE(forwardActiveChannels(array.data(), static_cast<int>(array.size()), physical, views)
                    == ChannelArrayShape::physicallyIndexed);
            CHECK(views[0] == firstEntry);
        }
    }
}

TEST_CASE("channel forwarding: non-contiguous multi-channel selection {1, 8, 32}",
          "[audio][asio][review]")
{
    DriverBuffers d;
    const std::vector<int> physical = { 0, 7, 31 };  // channels 1, 8, 32 together
    std::vector<const float*> views(3, nullptr);

    SECTION("compacted: map order is the entry order")
    {
        const float* compacted[] = { d.buffers[0], d.buffers[7], d.buffers[31] };
        REQUIRE(forwardActiveChannels(compacted, 3, physical, views) == ChannelArrayShape::compacted);
    }
    SECTION("physically indexed: holes stay holes, entries land by position")
    {
        const std::vector<const float*> array = sparsePhysical(DriverBuffers::kChannels, physical, d);
        REQUIRE(forwardActiveChannels(array.data(), static_cast<int>(array.size()), physical, views)
                == ChannelArrayShape::physicallyIndexed);
    }

    CHECK(views[0] == d.buffers[0]);
    CHECK(views[1] == d.buffers[7]);
    CHECK(views[2] == d.buffers[31]);
}

TEST_CASE("channel forwarding: output views reach the selected physical output buffer",
          "[audio][asio][review]")
{
    DriverBuffers d;
    const std::vector<int> physical = { 11 };  // operator selected output channel 12

    std::vector<float*> array = d.buffers;  // every output wired, none null
    std::vector<float*> views(1, nullptr);

    REQUIRE(forwardActiveChannels(array.data(), static_cast<int>(array.size()), physical, views)
            == ChannelArrayShape::physicallyIndexed);
    REQUIRE(views[0] != nullptr);

    views[0][0] = 0.5f;  // what the engine would write as translated audio
    CHECK(d.frames[11][0] == 0.5f);
    CHECK(d.frames[0][0] == 0.0f);   // and not onto output channel 1
}

TEST_CASE("channel forwarding: a short driver array forwards what exists and nulls the rest",
          "[audio][asio][review]")
{
    DriverBuffers d;
    const std::vector<int> physical = { 0, 7 };  // two active channels expected
    std::vector<const float*> views(2, d.buffers[7]);  // pre-dirtied: stale pointers must not survive

    const float* shortArray[] = { d.buffers[0] };
    REQUIRE(forwardActiveChannels(shortArray, 1, physical, views) == ChannelArrayShape::truncated);
    CHECK(views[0] == d.buffers[0]);
    CHECK(views[1] == nullptr);

    REQUIRE(forwardActiveChannels(shortArray, -1, physical, views) == ChannelArrayShape::truncated);
    CHECK(views[0] == nullptr);
    CHECK(views[1] == nullptr);
}

TEST_CASE("channel forwarding: null entries pass through - the engine reads them as no data",
          "[audio][asio][review]")
{
    const float* compacted[] = { nullptr };  // a driver that hands a hole even in a compacted array
    const std::vector<int> physical = { 3 };
    float poisonValue = 0.f;
    std::vector<const float*> views(1, &poisonValue);  // a stale pointer must not survive

    REQUIRE(forwardActiveChannels(compacted, 1, physical, views) == ChannelArrayShape::compacted);
    CHECK(views[0] == nullptr);
}

TEST_CASE("channel forwarding: all channels active - the two conventions agree, so observation cannot pick wrong",
          "[audio][asio][review]")
{
    DriverBuffers d;
    const std::vector<int> physical = { 0, 1, 2, 3 };  // a 4-channel device, nothing switched off
    std::vector<const float*> views(4, nullptr);

    const float* array[] = { d.buffers[0], d.buffers[1], d.buffers[2], d.buffers[3] };
    REQUIRE(forwardActiveChannels(array, 4, physical, views) == ChannelArrayShape::compacted);

    // Length 4 equals the active count, so the compacted rule applies; with every
    // channel active physical[k] == k, and the physically indexed rule would have
    // chosen the identical entries. This is the whole ambiguity, and it is empty.
    CHECK(views[0] == d.buffers[0]);
    CHECK(views[1] == d.buffers[1]);
    CHECK(views[2] == d.buffers[2]);
    CHECK(views[3] == d.buffers[3]);
}
