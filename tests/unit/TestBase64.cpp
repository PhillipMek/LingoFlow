//
// Task 009: base64 codec for the protocol's audio framing
// (docs/openai-realtime-protocol.md section 7: every append and every delta
// carries base64 PCM16). RFC 4648 vectors plus round-trips are the contract.

#include <cstdint>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "Network/Base64.h"

using namespace liveai::network;

TEST_CASE("Base64: RFC 4648 standard vectors", "[network][base64]")
{
    auto enc = [](const std::string& s) {
        return base64Encode(reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
    };

    CHECK(enc("") == "");
    CHECK(enc("f") == "Zg==");
    CHECK(enc("fo") == "Zm8=");
    CHECK(enc("foo") == "Zm9v");
    CHECK(enc("foob") == "Zm9vYg==");
    CHECK(enc("fooba") == "Zm9vYmE=");
    CHECK(enc("foobar") == "Zm9vYmFy");

    std::vector<std::uint8_t> out;
    CHECK(base64Decode("Zm9vYmFy", out));
    CHECK(std::string(out.begin(), out.end()) == "foobar");
    CHECK(base64Decode("Zg==", out));
    CHECK(out == std::vector<std::uint8_t> { 'f' });
}

TEST_CASE("Base64: round-trip over every tail length and byte value", "[network][base64]")
{
    std::vector<std::uint8_t> data;
    for (int i = 0; i < 256; ++i)
        data.push_back(static_cast<std::uint8_t>(i));
    for (int i = 0; i < 100; ++i)
        data.push_back(static_cast<std::uint8_t>((i * 7) & 0xFF));

    for (std::size_t len = 0; len <= data.size(); ++len)
    {
        const std::string encoded = base64Encode(data.data(), len);
        std::vector<std::uint8_t> decoded;
        REQUIRE(base64Decode(encoded, decoded));
        REQUIRE(decoded.size() == len);
        CHECK(std::equal(decoded.begin(), decoded.end(), data.begin()));
    }
}

TEST_CASE("Base64: invalid input is rejected, not tolerated", "[network][base64]")
{
    std::vector<std::uint8_t> out;

    CHECK_FALSE(base64Decode("Zm9", out));     // length not a multiple of 4
    CHECK_FALSE(base64Decode("Zm9vX==", out)); // padding in a non-final quantum
    CHECK_FALSE(base64Decode("Zm=v", out));    // "=x" tail is never legal
    CHECK_FALSE(base64Decode("Z=9v", out));    // '=' in a data slot
    CHECK_FALSE(base64Decode("Zm9!", out));    // not in the alphabet
    CHECK_FALSE(base64Decode("====", out));    // no data at all

    CHECK(base64Decode("Zm9v", out)); // control: valid, no padding
    CHECK(out.size() == 3);

    // Whitespace tolerance is deliberate (it only strips, never corrupts).
    CHECK(base64Decode("Zm9v\n Yg==", out));
    CHECK(std::string(out.begin(), out.end()) == "foob");
}

TEST_CASE("Base64: a full protocol-shaped audio payload round-trips", "[network][base64][audio]")
{
    // 200 ms of 24 kHz mono PCM16 = 9600 bytes; the real appends/deltas of the
    // protocol are exactly this order of magnitude.
    std::vector<std::uint8_t> pcm;
    pcm.reserve(9600);
    for (int i = 0; i < 4800; ++i)
    {
        const std::int16_t v = static_cast<std::int16_t>((i * 131) % 65536 - 32768);
        pcm.push_back(static_cast<std::uint8_t>(v & 0xFF));        // little-endian, wire order
        pcm.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
    }

    const std::string encoded = base64Encode(pcm.data(), pcm.size());
    CHECK(encoded.size() == 12800); // 9600 bytes -> ceil(9600/3)*4, exactly divisible

    std::vector<std::uint8_t> decoded;
    REQUIRE(base64Decode(encoded, decoded));
    REQUIRE(decoded.size() == pcm.size());
    CHECK(decoded == pcm);
}
