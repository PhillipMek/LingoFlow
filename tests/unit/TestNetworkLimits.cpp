// An earlier review: an unbounded inbound stream in front of the
// audio path is a production robustness hole regardless of who the sender is.
// NetworkLimits.h is pure std precisely so these numbers and this arithmetic
// are testable without a socket, a server or a network: the transport wires
// the budget, the backend enforces the audio bound, and the claims between
// them (the margins from the live measurement in protocol docs section 15)
// are assertions here, not comments.

#include <catch2/catch_test_macros.hpp>

#include "Network/NetworkLimits.h"

using liveai::network::InboundMessageBudget;
using liveai::network::kMaxAudioDeltaBytes;
using liveai::network::kMaxInboundMessageBytes;

TEST_CASE("InboundMessageBudget: counts to the exact boundary and not past it",
          "[network][limits]")
{
    InboundMessageBudget budget(100);

    CHECK(budget.accept(60));
    CHECK(budget.received() == 60);
    CHECK(budget.accept(40));            // exactly to the ceiling is still admitted
    CHECK(budget.received() == 100);
    CHECK_FALSE(budget.accept(1));       // one byte over - refused
    CHECK(budget.exhausted());
    CHECK_FALSE(budget.accept(0));       // once exhausted, stay exhausted: the
                                         // transport must not dribble fragments in
    budget.reset();
    CHECK(budget.accept(100));           // a new message starts from zero
}

TEST_CASE("InboundMessageBudget: the default is the documented reassembly ceiling",
          "[network][limits]")
{
    InboundMessageBudget budget;
    CHECK(budget.accept(kMaxInboundMessageBytes));
    CHECK_FALSE(budget.accept(1));

    // The margin claim written into NetworkLimits.h: the largest documented
    // event is a ~26 KB base64 audio delta (19200-byte payload measured live,
    // protocol docs section 15). 16 MiB is meant to be ~250x clear of any
    // legitimate traffic - if anyone ever shrinks the ceiling, this trips.
    CHECK(kMaxInboundMessageBytes >= 250 * 60u * 1024u);
}

TEST_CASE("audio delta budget: sized against the live measurement, still even",
          "[network][limits]")
{
    CHECK(kMaxAudioDeltaBytes % 2 == 0);                       // whole PCM16 samples
    CHECK(kMaxAudioDeltaBytes >= 13 * 19200u);                 // 13x the measured 400 ms chunk
    CHECK(kMaxAudioDeltaBytes < kMaxInboundMessageBytes);      // the block bound sits inside the message bound
}
