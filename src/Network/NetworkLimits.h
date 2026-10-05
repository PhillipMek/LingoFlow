#pragma once
//
// NetworkLimits - the product's size refusals (code review P1, 2026-10-05).
// The realtime backend sits one step before audio delivery: an event stream
// that can grow without a bound is not "unlikely" (the reviewer's own framing
// - OpenAI is not the threat model), it is a production robustness hole. So
// every inbound byte now lands inside a declared budget, and each budget
// failure takes the smallest action that is honest:
//
//   * Transport reassembly (kMaxInboundMessageBytes): a fragmented message
//     that outgrows the budget is severed as a transport error - the
//     established death path (the supervisor recovers per task 010 policy).
//     16 MiB is roughly 250x the largest documented event: a 400 ms audio
//     delta measured live was 19200 PCM16 bytes (protocol docs section 15),
//     base64 ~26 KB of JSON. Legitimate traffic never brushes the ceiling;
//     a runaway stream cannot eat the process.
//   * Audio delta payload (kMaxAudioDeltaBytes): 256 KiB decoded PCM16 is
//     13x the measured chunk. One oversized event is a SHAPE anomaly, not a
//     transport failure, and section 7's established rule for shape anomalies
//     applies: drop the block, report it as a non-fatal protocol error, keep
//     the session. Killing the show because the provider mis-sized one delta
//     would trade the bug for a bigger one.
//
// The numbers live in one header, pure std, so tests pin the arithmetic
// without a socket, a server or a network.

#include <cstddef>

namespace liveai {
namespace network {

constexpr std::size_t kMaxInboundMessageBytes = 16u * 1024u * 1024u;
constexpr std::size_t kMaxAudioDeltaBytes = 256u * 1024u;

/// Reassembly budget a WebSocket transport spends before calling the
/// connection dead. Pure and stateful by design: accept() either admits the
/// chunk or declares the budget exhausted, and once exhausted it never
/// admits again - the transport reports the overflow once, clears what it
/// accumulated, and stops allocating. No thread safety is needed or claimed:
/// one connection reads on one thread.
class InboundMessageBudget
{
public:
    explicit InboundMessageBudget(std::size_t maxBytes = kMaxInboundMessageBytes) noexcept
        : maxBytes_(maxBytes)
    {
    }

    /// true = chunk admitted (and counted). false = budget blown: the caller
    /// must abandon the in-progress message and fail the connection; further
    /// accept() calls stay false until reset().
    bool accept(std::size_t chunkBytes) noexcept
    {
        if (exhausted_ || received_ + chunkBytes > maxBytes_)
        {
            exhausted_ = true;
            return false;
        }
        received_ += chunkBytes;
        return true;
    }

    bool exhausted() const noexcept { return exhausted_; }
    std::size_t received() const noexcept { return received_; }
    void reset() noexcept
    {
        received_ = 0;
        exhausted_ = false;
    }

private:
    std::size_t maxBytes_;
    std::size_t received_ = 0;
    bool exhausted_ = false;
};

} // namespace network
} // namespace liveai
