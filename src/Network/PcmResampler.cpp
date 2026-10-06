#include "Network/PcmResampler.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <numeric>

namespace liveai {
namespace network {

namespace {

constexpr int kTaps = 15;
constexpr double kPi = 3.14159265358979323846;

/// The rational stage: 49 taps (half-length 24), Blackman windowed-sinc. The
/// lookahead bound L is also the zero-padding the stream starts with - the
/// same way every filter begins - and it is what maxOutputFor's slack covers.
constexpr int kRationalHalf = 24;
constexpr int kRationalTaps = 2 * kRationalHalf + 1;

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

    const bool cascade =
        inputRate == outputRate || inputRate == 2 * outputRate || inputRate == 4 * outputRate ||
        2 * inputRate == outputRate || 4 * inputRate == outputRate;
    if (cascade)
        return true;

    // The rational stage's whole duty: the wire rate against the config's
    // 44.1/88.2 device entries (review P1, 2026-10-05). Not an arbitrary-ratio
    // license - 32000 Hz and 22050<->24000 stay refused, by design.
    return (inputRate == 24000 && (outputRate == 44100 || outputRate == 88200))
        || (outputRate == 24000 && (inputRate == 44100 || inputRate == 88200));
}

bool PcmResampler::configureRational(int inputRate, int outputRate)
{
    const long long g = std::gcd(static_cast<long long>(inputRate), static_cast<long long>(outputRate));
    inSpan_ = static_cast<long long>(inputRate) / g;   // 147 for 44.1 <-> 24
    outSpan_ = static_cast<long long>(outputRate) / g; //  80 for 44.1 -> 24, 147 for 24 -> 44.1

    // Cutoff at half of the smaller rate, in cycles per INPUT sample: the band
    // that survives is min(Nyquist_in, Nyquist_out) in absolute Hz, which is
    // exactly what keeps the down direction alias-free and the up direction
    // image-free in the speech band.
    const double ratio = static_cast<double>(outputRate) / static_cast<double>(inputRate);
    const double fc = 0.5 * std::min(1.0, ratio);

    coeffs_.assign(static_cast<std::size_t>(outSpan_ * kRationalTaps), 0.0f);
    for (long long k = 0; k < outSpan_; ++k)
    {
        // Output k of the period sits at continuous input position
        // k*inSpan_/outSpan_; phase and fractional offset are EXACT integers -
        // the pattern repeats every period, so a two-hour show accumulates no
        // timing drift whatsoever. Tap j of this phase lands at input index
        // (floor of that position) - L + j, so its distance from the output
        // position is (j - L) - frac: the integer part cancels, the row only
        // needs the fraction.
        const long long num = k * inSpan_;
        const double frac = static_cast<double>(num % outSpan_) / static_cast<double>(outSpan_);

        float* row = coeffs_.data() + static_cast<std::size_t>(k * kRationalTaps);
        double sum = 0.0;
        for (int j = 0; j < kRationalTaps; ++j)
        {
            const double x = static_cast<double>(j - kRationalHalf) - frac;
            const double t = x / kRationalHalf;
            const double w = 0.42 + 0.5 * std::cos(kPi * t) + 0.08 * std::cos(2.0 * kPi * t);
            const double u = 2.0 * fc * x;
            const double s = std::fabs(u) < 1e-12 ? 1.0 : std::sin(kPi * u) / (kPi * u);
            const double c = 2.0 * fc * s * w;
            row[j] = static_cast<float>(c);
            sum += c;
        }
        for (int j = 0; j < kRationalTaps; ++j)
            row[j] = static_cast<float>(row[j] / sum);   // every phase is DC-unity
    }
    return true;
}

void PcmResampler::resetRationalStream()
{
    // The filter begins the way every filter begins: with silence behind it.
    xin_.assign(static_cast<std::size_t>(kRationalHalf), 0.0f);
    baseAbs_ = -kRationalHalf;
    absIn_ = 0;
    absOut_ = 0;
}

bool PcmResampler::configure(int inputRate, int outputRate, int maxInFrames)
{
    if (!isSupportedPair(inputRate, outputRate) || maxInFrames < 1)
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

    rational_ = downStages_ == 0 && upStages_ == 0 && inputRate != outputRate;
    if (rational_)
        configureRational(inputRate, outputRate);

    // Preallocate the whole working set here, while allocation is allowed,
    // so process() can keep its noexcept honestly (An earlier review,
    // 2026-10-06): the scratch only ever serves the two-stage cascades and
    // only ever needs maxOutputFor(maxInFrames); the rational window holds
    // at most the filter's retained history plus one chunk - the history is
    // bounded by the lookahead (2 * kRationalHalf + a period's slack), and
    // three tap-lengths of margin covers it generously. A chunk larger than
    // the plan is refused by process(), never absorbed by a resize.
    maxInFrames_ = maxInFrames;
    const std::size_t scratchNeed = (downStages_ == 2 || upStages_ == 2)
                                        ? static_cast<std::size_t>(maxOutputFor(maxInFrames))
                                        : 0u;
    scratch_.assign(scratchNeed, 0.0f);
    if (rational_)
        xin_.reserve(static_cast<std::size_t>(maxInFrames) + 3 * kRationalTaps);

    downs_.assign(static_cast<std::size_t>(downStages_), DownState {});
    ups_.assign(static_cast<std::size_t>(upStages_), UpState {});
    resetRationalStream();
    return true;
}

void PcmResampler::reset() noexcept
{
    for (DownState& s : downs_)
        s = DownState {};
    for (UpState& s : ups_)
        s = UpState {};
    // Only the rational object owns xin_ - and after a successful configure()
    // its capacity already covers the zero-padding, so the assign inside
    // cannot allocate and this noexcept stays honest.
    if (rational_)
        resetRationalStream();   // a reopened session must not inherit the old stream's tail
}

int PcmResampler::maxOutputFor(int inFrames) noexcept
{
    // Worst case is the up4 cascade or the rational 24k->88.2k (3.675 per
    // input frame, plus the filter's bounded lookahead/transients). Every
    // supported ratio fits under this ceiling; process() may not exceed it.
    return inFrames * 4 + 64;
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

int PcmResampler::runRational(const float* in, int n, float* out) noexcept
{
    // The insert below cannot allocate: process() has refused n larger than
    // maxInFrames_, configure() reserved maxInFrames + 3 * kRationalTaps, and
    // the retained history never exceeds the filter's lookahead (the loop
    // stops one output short of needing samples that have not arrived, and
    // the erase at the bottom keeps at most ~2 * kRationalHalf + 1 of it).
    xin_.insert(xin_.end(), in, in + n);
    absIn_ += n;

    int produced = 0;

    for (;;)
    {
        const long long num = absOut_ * inSpan_;
        const long long first = num / outSpan_ - kRationalHalf;
        if (first + kRationalTaps > absIn_)
            break;   // this output's lookahead is not on the wire yet - wait, do not guess

        const float* taps = xin_.data() + static_cast<std::size_t>(first - baseAbs_);
        const float* row = coeffs_.data()
                         + static_cast<std::size_t>((absOut_ % outSpan_) * kRationalTaps);

        double acc = 0.0;
        for (int j = 0; j < kRationalTaps; ++j)
            acc += static_cast<double>(taps[j]) * static_cast<double>(row[j]);
        out[produced++] = static_cast<float>(acc);
        ++absOut_;
    }

    // Keep precisely the history the next output needs, and nothing more:
    // the window stays bounded (a few taps) no matter how long the show runs.
    const long long nextFirst = ((absOut_ * inSpan_) / outSpan_) - kRationalHalf;
    long long keepFrom = nextFirst > absIn_ ? absIn_ : nextFirst;
    if (keepFrom < baseAbs_)
        keepFrom = baseAbs_;   // the zero-padding in front is part of the stream's start
    if (keepFrom > baseAbs_)
    {
        xin_.erase(xin_.begin(),
                   xin_.begin() + static_cast<std::ptrdiff_t>(keepFrom - baseAbs_));
        baseAbs_ = keepFrom;
    }
    return produced;
}

int PcmResampler::process(const float* in, int inFrames, float* out, int outCapacity) noexcept
{
    if (inFrames == 0)
        return 0;
    if (inputRate_ == 0)
        return -1;
    if (inFrames > maxInFrames_)
        return -1;   // larger than the cadence configure() planned for
    if (outCapacity < maxOutputFor(inFrames))
        return -1;

    if (rational_)
        return runRational(in, inFrames, out);

    int produced = inFrames;

    if (downStages_ == 2)
    {
        // Belt and braces: configure() sized the scratch to
        // maxOutputFor(maxInFrames) and the inFrames guard above already holds,
        // so this refusal is unreachable for configured objects - but it keeps
        // the no-allocation promise local and greppable: no resize, ever.
        if (static_cast<int>(scratch_.size()) < maxOutputFor(inFrames))
            return -1;
        produced = runDown(in, inFrames, scratch_.data(), downs_[0]);
        produced = runDown(scratch_.data(), produced, out, downs_[1]);
        return produced;
    }
    if (downStages_ == 1)
        return runDown(in, inFrames, out, downs_[0]);
    if (upStages_ == 2)
    {
        if (static_cast<int>(scratch_.size()) < maxOutputFor(inFrames))
            return -1;
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
