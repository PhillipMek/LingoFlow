//
// Task 009: the fixed-ratio resampler inside the backend.
//
// The protocol pins the wire at 24 kHz mono (docs/openai-realtime-protocol.md
// section 7) and the engines run 48 kHz (dev defaults), so these conversions
// carry real translated audio. What the tests assert is the DSP property the
// product needs: the band we keep passes at unity, the band beyond the new
// Nyquist must not fold into it, and the streaming state makes chunked
// processing byte-identical to one-shot processing (no seams, no drift).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "Network/PcmResampler.h"

using namespace liveai::network;

namespace {

std::vector<float> makeSine(int frames, double rate, double freq, double amplitude = 1.0)
{
    std::vector<float> out (static_cast<std::size_t>(frames));
    for (int i = 0; i < frames; ++i)
        out[static_cast<std::size_t>(i)] = static_cast<float>(
            amplitude * std::sin (2.0 * 3.14159265358979 * static_cast<double>(freq)
                                  * static_cast<double>(i) / rate));
    return out;
}

double rms(const std::vector<float>& v, int skip)
{
    if (static_cast<int>(v.size()) <= skip)
        return 0.0;
    double acc = 0.0;
    const std::size_t n = v.size() - static_cast<std::size_t>(skip);
    for (std::size_t i = static_cast<std::size_t>(skip); i < v.size(); ++i)
        acc += static_cast<double>(v[i]) * v[i];
    return std::sqrt (acc / static_cast<double>(n));
}

/// |X(f)| via Goertzel on a float block.
double goertzel(const std::vector<float>& v, double rate, double freq)
{
    const double w = 2.0 * 3.14159265358979 * freq / rate;
    const double coeff = 2.0 * std::cos (w);
    double s1 = 0.0, s2 = 0.0;
    for (const float x : v)
    {
        const double s0 = static_cast<double>(x) + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return std::sqrt (s1 * s1 + s2 * s2 - coeff * s1 * s2) / static_cast<double>(v.size());
}

int runAll(PcmResampler& r, const std::vector<float>& in, std::vector<float>& out)
{
    out.resize(static_cast<std::size_t>(PcmResampler::maxOutputFor(static_cast<int>(in.size()))));
    const int n = r.process(in.data(), static_cast<int>(in.size()), out.data(),
                            static_cast<int>(out.size()));
    if (n > 0)
        out.resize(static_cast<std::size_t>(n));
    else
        out.clear();
    return n;
}

} // namespace

TEST_CASE("PcmResampler: only the documented rate pairs are supported", "[audio][resampler]")
{
    CHECK(PcmResampler::isSupportedPair(24000, 24000));
    CHECK(PcmResampler::isSupportedPair(48000, 24000));
    CHECK(PcmResampler::isSupportedPair(96000, 24000));
    CHECK(PcmResampler::isSupportedPair(24000, 48000));
    CHECK(PcmResampler::isSupportedPair(24000, 96000));
    CHECK(PcmResampler::isSupportedPair(48000, 96000));
    CHECK(PcmResampler::isSupportedPair(96000, 48000));

    // 44.1k and nonsense pairs are refused at configuration; the backend then
    // refuses the session instead of guessing (AGENTS.md 8).
    CHECK_FALSE(PcmResampler::isSupportedPair(44100, 24000));
    CHECK_FALSE(PcmResampler::isSupportedPair(24000, 44100));
    CHECK_FALSE(PcmResampler::isSupportedPair(48000, 20000));
    CHECK_FALSE(PcmResampler::isSupportedPair(0, 24000));
    CHECK_FALSE(PcmResampler::isSupportedPair(24000, 0));
    CHECK_FALSE(PcmResampler::isSupportedPair(-48000, 24000));

    PcmResampler r;
    CHECK_FALSE(r.configure(44100, 24000));
    CHECK_FALSE(r.configure(24000, 44100));
    CHECK(r.inputRate() == 0); // a refused configure leaves the object untouched
    REQUIRE(r.configure(24000, 24000));
    CHECK(r.configure(48000, 24000)); // a supported pair replaces the cascade
    CHECK(r.configure(24000, 48000));
    CHECK(r.inputRate() == 24000);
    CHECK(r.outputRate() == 48000);
    CHECK_FALSE(r.configure(48000, 22050));
    CHECK(r.inputRate() == 24000); // still the last valid pair
}

TEST_CASE("PcmResampler: 48k->24k passes the speech band at unity", "[audio][resampler]")
{
    PcmResampler r;
    REQUIRE(r.configure(48000, 24000));
    r.reset();

    const auto in = makeSine(9600, 48000.0, 1000.0); // 200 ms of 1 kHz
    std::vector<float> out;
    REQUIRE(runAll(r, in, out) == 4800);

    // Amplitude preserved (RMS ratio ~ 1/sqrt2 of peak is the same both sides).
    CHECK(rms(out, 100) == Catch::Approx(0.7071).epsilon(0.05));
}

TEST_CASE("PcmResampler: decimation suppresses the band that would alias", "[audio][resampler]")
{
    PcmResampler r;
    REQUIRE(r.configure(48000, 24000));

    // 20 kHz at the 48k input would fold to 4 kHz at 24k without the antialias
    // filter: after the halfband it must be negligible.
    const auto in = makeSine(9600, 48000.0, 20000.0, 0.9);
    std::vector<float> out;
    REQUIRE(runAll(r, in, out) == 4800);

    // Measured in the 24 kHz view: nothing at the would-be alias (4 kHz) and
    // nothing anywhere above the new Nyquist-relevant band.
    CHECK(goertzel(out, 24000.0, 4000.0) < 5e-3);
    CHECK(rms(out, 200) < 0.02);
}

TEST_CASE("PcmResampler: 24k->48k keeps DC and rejects images", "[audio][resampler]")
{
    PcmResampler r;
    REQUIRE(r.configure(24000, 48000));

    { // DC
        std::vector<float> in (2400, 0.25f);
        std::vector<float> out;
        REQUIRE(runAll(r, in, out) == 4800);
        CHECK(out[4000] == Catch::Approx(0.25f).epsilon(0.01));
        CHECK(out[4001] == Catch::Approx(0.25f).epsilon(0.01));
    }

    { // 1 kHz sine: the image at 23 kHz (24k - 1k around the new half-rate) must die
        r.reset();
        const auto in = makeSine(4800, 24000.0, 1000.0);
        std::vector<float> out;
        REQUIRE(runAll(r, in, out) == 9600);
        CHECK(goertzel(out, 48000.0, 1000.0) > 0.4); // fundamental present
        CHECK(goertzel(out, 48000.0, 23000.0) < 0.01); // image suppressed
    }
}

TEST_CASE("PcmResampler: chunked streaming equals one-shot processing", "[audio][resampler]")
{
    // The backend feeds chunks of varying size (engine blocks, deltas); the
    // filter state must carry across them with no seams and no level jumps.
    const auto in = makeSine(9600, 48000.0, 1000.0);

    PcmResampler whole;
    REQUIRE(whole.configure(48000, 24000));
    std::vector<float> outWhole;
    REQUIRE(runAll(whole, in, outWhole) == 4800);

    PcmResampler chunked;
    REQUIRE(chunked.configure(48000, 24000));
    std::vector<float> outChunked;
    int offset = 0;
    const int sizes[] = { 997, 512, 1920, 480, 2048, 3721, 48, 1920 };
    for (const int sz : sizes)
    {
        const int n = std::min(sz, static_cast<int>(in.size()) - offset);
        if (n <= 0)
            break;
        std::vector<float> piece (static_cast<std::size_t>(
            PcmResampler::maxOutputFor(n)));
        const int produced = chunked.process(in.data() + offset, n, piece.data(),
                                             static_cast<int>(piece.size()));
        REQUIRE(produced >= 0);
        outChunked.insert(outChunked.end(), piece.begin(), piece.begin() + produced);
        offset += n;
    }
    REQUIRE(offset == static_cast<int>(in.size()));

    REQUIRE(outChunked.size() == outWhole.size());
    CHECK(std::equal(outWhole.begin(), outWhole.end(), outChunked.begin()));
}

TEST_CASE("PcmResampler: the 4:1 cascades work in both directions", "[audio][resampler]")
{
    PcmResampler down;
    REQUIRE(down.configure(96000, 24000));
    const auto in96 = makeSine(19200, 96000.0, 1000.0);
    std::vector<float> out24;
    REQUIRE(runAll(down, in96, out24) == 4800);
    CHECK(rms(out24, 200) == Catch::Approx(0.7071).epsilon(0.05));
    CHECK(goertzel(out24, 24000.0, 12000.0) < 6e-3); // 84k input tone aliases to 12k: filtered

    PcmResampler up;
    REQUIRE(up.configure(24000, 96000));
    std::vector<float> dcIn (1200, 0.5f);
    std::vector<float> dcOut;
    REQUIRE(runAll(up, dcIn, dcOut) == 4800);
    CHECK(dcOut[4000] == Catch::Approx(0.5f).epsilon(0.01));
}

TEST_CASE("PcmResampler: identity passes samples through untouched", "[audio][resampler]")
{
    PcmResampler r;
    REQUIRE(r.configure(24000, 24000));
    std::vector<float> in { 0.1f, -0.2f, 0.3f, 0.0f, 1.0f, -1.0f };
    std::vector<float> out (static_cast<std::size_t>(PcmResampler::maxOutputFor(6)));
    REQUIRE(r.process(in.data(), 6, out.data(), static_cast<int>(out.size())) == 6);
    CHECK(out[0] == 0.1f);
    CHECK(out[4] == 1.0f);
    CHECK(out[5] == -1.0f);
}

TEST_CASE("PcmResampler: undersized output capacity is refused, not overrun",
          "[audio][resampler][robustness]")
{
    PcmResampler r;
    REQUIRE(r.configure(24000, 48000));
    std::vector<float> in (1000, 0.1f);
    std::vector<float> out (1000); // below maxOutputFor(1000)
    CHECK(r.process(in.data(), 1000, out.data(), 1000) == -1);

    std::vector<float> tiny (1);
    CHECK(r.process(nullptr, 0, tiny.data(), 1) == 0); // zero input is zero output
}
