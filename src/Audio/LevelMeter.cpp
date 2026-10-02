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
    constexpr float kFloorDb = -120.0f;

    if (!std::isfinite(linear) || linear <= 0.0f)
        return kFloorDb;

    // 20*log10 for amplitude quantities; a magnitude of 1.0 is full scale (0 dBFS).
    const double db = 20.0 * std::log10(static_cast<double>(linear));

    return db < static_cast<double>(kFloorDb) ? kFloorDb : static_cast<float>(db);
}

} // namespace audio
} // namespace liveai
