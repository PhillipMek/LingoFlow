#include "Audio/LevelMeter.h"

#include <cmath>

namespace liveai {
namespace audio {

void LevelMeter::measure(const float* data, std::size_t frames) noexcept
{
    if (data == nullptr || frames == 0)
        return;

    float peak = 0.0f;
    double sumOfSquares = 0.0;
    std::uint64_t clipped = 0;

    for (std::size_t i = 0; i < frames; ++i)
    {
        const float magnitude = std::fabs(data[i]);

        // A non-finite sample is not signal: it is a driver or upstream defect. It is
        // counted as out-of-range and excluded from the averages so one NaN cannot
        // poison the meter (and the UI) forever.
        if (!std::isfinite(magnitude))
        {
            ++clipped;
            continue;
        }

        if (magnitude > peak)
            peak = magnitude;

        if (magnitude >= 1.0f)
            ++clipped;

        sumOfSquares += static_cast<double>(magnitude) * static_cast<double>(magnitude);
    }

    const float rms = static_cast<float>(std::sqrt(sumOfSquares / static_cast<double>(frames)));

    peak_.store(peak, std::memory_order_relaxed);
    rms_.store(rms, std::memory_order_relaxed);
    blocks_.fetch_add(1, std::memory_order_relaxed);

    if (clipped != 0)
        clipFrames_.fetch_add(clipped, std::memory_order_relaxed);

    if (rms >= kSignalPresentRms)
        signalledBlocks_.fetch_add(1, std::memory_order_relaxed);
}

bool LevelMeter::signalPresent() const noexcept
{
    return rms_.load(std::memory_order_relaxed) >= kSignalPresentRms;
}

void LevelMeter::reset() noexcept
{
    peak_.store(0.0f, std::memory_order_relaxed);
    rms_.store(0.0f, std::memory_order_relaxed);
    blocks_.store(0, std::memory_order_relaxed);
    clipFrames_.store(0, std::memory_order_relaxed);
    signalledBlocks_.store(0, std::memory_order_relaxed);
}

float linearToDb(float linear) noexcept
{
    if (!std::isfinite(linear) || linear <= 0.0f)
        return kMeterFloorDb;

    // 20*log10 for amplitude quantities; a magnitude of 1.0 is full scale (0 dBFS).
    const double db = 20.0 * std::log10(static_cast<double>(linear));

    return db < static_cast<double>(kMeterFloorDb) ? kMeterFloorDb : static_cast<float>(db);
}

MeterSmoother::Display MeterSmoother::update(float rawPeakDb, float rawRmsDb,
                                             double elapsedMs) noexcept
{
    const double dt = elapsedMs > 0.0 ? elapsedMs : 0.0;   // a rewound clock is not a step back

    auto sanitize = [](float value) noexcept
    {
        // A non-finite readout is not a level. It arrives only from an already
        // broken measurement; the display owes the operator silence, not NaN.
        if (!std::isfinite(value))
            return kMeterFloorDb;
        return value < kMeterFloorDb ? kMeterFloorDb : value;
    };

    auto glide = [dt](float& value, float target, double tauMs) noexcept
    {
        if (dt == 0.0)
            return;   // same instant: no time passed, no movement is owed

        // First-order exponential approach. Note the algebra the tests lean on:
        // each step multiplies the remaining distance by e^(-dt/tau), so the
        // product over a sequence depends only on the TOTAL time - the filter
        // is a rate in seconds, not a magic coefficient per repaint.
        const double alpha = 1.0 - std::exp(-dt / tauMs);
        const double moved = static_cast<double>(value) + alpha * (static_cast<double>(target) - value);

        // Never below the floor (exp() cannot cross the target, but float
        // rounding near it deserves the boundary), and never away from it in
        // the wrong direction: an exponential to a target cannot overshoot.
        value = moved < static_cast<double>(kMeterFloorDb) ? kMeterFloorDb : static_cast<float>(moved);
    };

    const float peakTarget = sanitize(rawPeakDb);
    const float rmsTarget = sanitize(rawRmsDb);

    glide(peak_, peakTarget, peakTarget > peak_ ? kAttackTauMs : kReleaseTauMs);
    glide(rms_, rmsTarget, rmsTarget > rms_ ? kAttackTauMs : kReleaseTauMs);

    return Display{ peak_, rms_ };
}

} // namespace audio
} // namespace liveai
