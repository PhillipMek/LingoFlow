#include "Network/PcmResampler.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace liveai {
namespace network {

namespace {

constexpr int kTaps = 15;
constexpr double kPi = 3.14159265358979323846;

/// Windowed-sinc halfband lowpass at the /2 cutoff, hamming-windowed, DC gain
/// normalised to exactly 1. Odd-offset-from-center taps vanish by construction
/// (the halfband property), which is what makes the interpolator's phase-1
/// branch the textbook "center tap only" case.
const std::array<float, kTaps>& halfbandCoeffs()
{
    static const std::array<float, kTaps> h = [] {
        std::array<float, kTaps> c {};
        const int center = kTaps / 2;
        double sum = 0.0;
        for (int k = 0; k < kTaps; ++k)
        {
            const int n = k - center;
            double v;
            if (n == 0)
                v = 0.5;
            else if ((n & 1) == 0)
                v = 0.0;
            else
                v = std::sin(kPi * n / 2.0) / (kPi * static_cast<double>(n));
            const double w = 0.46 - 0.54 * std::cos(2.0 * kPi * static_cast<double>(k) / (kTaps - 1));
            c[k] = static_cast<float>(v * w);
            sum += v * w;
        }
        for (float& x : c)
            x = static_cast<float>(x / sum); // unity at DC
        return c;
    }();
    return h;
}

} // namespace

bool PcmResampler::isSupportedPair(int inputRate, int outputRate) noexcept
{
    if (inputRate <= 0 || outputRate <= 0)
        return false;

    return inputRate == outputRate || inputRate == 2 * outputRate || inputRate == 4 * outputRate ||
           2 * inputRate == outputRate || 4 * inputRate == outputRate;
}

bool PcmResampler::configure(int inputRate, int outputRate)
{
    if (!isSupportedPair(inputRate, outputRate))
        return false;

    inputRate_ = inputRate;
    outputRate_ = outputRate;

    downStages_ = 0;
    upStages_ = 0;
    if (inputRate == 4 * outputRate)
        downStages_ = 2;
    else if (inputRate == 2 * outputRate)
        downStages_ = 1;
    else if (4 * inputRate == outputRate)
        upStages_ = 2;
    else if (2 * inputRate == outputRate)
        upStages_ = 1;

    downs_.assign(static_cast<std::size_t>(downStages_), DownState {});
    ups_.assign(static_cast<std::size_t>(upStages_), UpState {});
    return true;
}

void PcmResampler::reset() noexcept
{
    for (DownState& s : downs_)
        s = DownState {};
    for (UpState& s : ups_)
        s = UpState {};
}

int PcmResampler::maxOutputFor(int inFrames) noexcept
{
    // Worst case is the up4 cascade; every other ratio needs less. +16 covers
    // the stage transients of chained passes.
    return inFrames * 4 + 16;
}

int PcmResampler::runDown(const float* in, int n, float* out, DownState& st) noexcept
{
    const auto& h = halfbandCoeffs();
    int written = 0;

    for (int i = 0; i < n; ++i)
    {
        std::memmove(&st.win[1], &st.win[0], sizeof(float) * (kTaps - 1));
        st.win[0] = in[i];
        st.parity = !st.parity;
        if (st.parity)
            continue; // wait for the second sample of the pair

        float acc = 0.0f;
        for (int k = 0; k < kTaps; ++k)
            acc += h[k] * st.win[k];
        out[written++] = acc;
    }

    return written;
}

int PcmResampler::runUp(const float* in, int n, float* out, UpState& st) noexcept
{
    const auto& h = halfbandCoeffs();

    // Polyphase branches of the same prototype scaled by the interpolation
    // gain of 2: phase 0 takes the even taps, phase 1 the odd taps. Under the
    // halfband property phase 1 collapses to the center tap; that is the
    // textbook halfband interpolator, and the resulting half-sample skew of
    // the two phases (10 us at 48 kHz) is inaudible in a speech chain.
    int written = 0;

    for (int i = 0; i < n; ++i)
    {
        std::memmove(&st.win[1], &st.win[0], sizeof(float) * 7);
        st.win[0] = in[i];

        float even = 0.0f;
        for (int j = 0; j < 8; ++j)
            even += 2.0f * h[2 * j] * st.win[j];

        float odd = 0.0f;
        for (int j = 0; j < 7; ++j)
            odd += 2.0f * h[2 * j + 1] * st.win[j];

        out[written++] = even;
        out[written++] = odd;
    }

    return written;
}

int PcmResampler::process(const float* in, int inFrames, float* out, int outCapacity) noexcept
{
    if (inFrames == 0)
        return 0;
    if (inputRate_ == 0)
        return -1;
    if (outCapacity < maxOutputFor(inFrames))
        return -1;

    int produced = inFrames;

    if (inFrames > 0)
    {
        const int need = maxOutputFor(inFrames);
        if (static_cast<int>(scratch_.size()) < need)
            scratch_.resize(static_cast<std::size_t>(need));
    }

    if (downStages_ == 2)
    {
        produced = runDown(in, inFrames, scratch_.data(), downs_[0]);
        produced = runDown(scratch_.data(), produced, out, downs_[1]);
        return produced;
    }
    if (downStages_ == 1)
        return runDown(in, inFrames, out, downs_[0]);
    if (upStages_ == 2)
    {
        produced = runUp(in, inFrames, scratch_.data(), ups_[0]);
        produced = runUp(scratch_.data(), produced, out, ups_[1]);
        return produced;
    }
    if (upStages_ == 1)
        return runUp(in, inFrames, out, ups_[0]);

    std::memcpy(out, in, sizeof(float) * static_cast<std::size_t>(inFrames));
    return inFrames;
}

} // namespace network
} // namespace liveai
