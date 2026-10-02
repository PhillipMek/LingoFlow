#pragma once
//
// PcmResampler - the fixed-ratio sample rate converter inside the translation
// backend (task 009, docs/openai-realtime-protocol.md section 7: "a resampler is
// mandatory inside the backend, on the network/worker thread").
//
// Scope decided by the protocol, not by ambition: the provider speaks 24 kHz on
// the wire, the engine runs at its device rate (48 kHz in the dev defaults,
// SoundGrid convention). This class therefore handles exactly the ratios 1, 2, 4
// down and 1/2, 1/4 up, as a cascade of halfband stages, and refuses everything
// else loudly at configuration time. No fancy arbitrary-ratio engine - an
// unsupported pair must become a rejected session, never a silent guess
// (AGENTS.md 8, 19).
//
// Filter: 15-tap windowed-sinc halfband (odd taps around the center are zero by
// construction), stop-band roughly -50 dB - far below anything audible in a
// speech monitoring path, and cheap on a worker thread. The object is STATEFUL
// across process() calls: the filter history continues from chunk to chunk, so
// the resampled stream has no seams. It is not thread-safe and does not need to
// be: one instance lives on one network thread.

#include <vector>

namespace liveai {
namespace network {

class PcmResampler
{
public:
    /// True when both rates are positive and the pair is one of 24/48/96 kHz
    /// with ratio 1, 2 or 4 in either direction.
    static bool isSupportedPair(int inputRate, int outputRate) noexcept;

    /// Configures the stage cascade. Returns false for an unsupported pair and
    /// leaves the object untouched.
    bool configure(int inputRate, int outputRate);

    /// Drops the filter history (new session - a reopened session must not
    /// inherit filter state from the old one).
    void reset() noexcept;

    int inputRate() const noexcept { return inputRate_; }
    int outputRate() const noexcept { return outputRate_; }

    /// Worst-case output frames for inFrames input frames, including stage
    /// transients. Callers size their buffer with this and process() cannot
    /// overflow it.
    static int maxOutputFor(int inFrames) noexcept;

    /// Consumes inFrames floats and writes converted frames into out. Returns
    /// the number written, or -1 if outCapacity is smaller than maxOutputFor
    /// (caller bug, not a data condition).
    int process(const float* in, int inFrames, float* out, int outCapacity) noexcept;

private:
    struct DownState
    {
        float win[15] {}; // sliding input window, [0] = newest
        bool parity = false; // only every second sample completes an output frame
    };

    struct UpState
    {
        float win[8] {}; // input history for the two polyphase sub-filters
    };

    int runDown(const float* in, int n, float* out, DownState& st) noexcept;
    int runUp(const float* in, int n, float* out, UpState& st) noexcept;

    int inputRate_ = 0;
    int outputRate_ = 0;
    int downStages_ = 0;
    int upStages_ = 0;

    std::vector<DownState> downs_;
    std::vector<UpState> ups_;

    std::vector<float> scratch_; // intermediate buffer between cascade stages
};

} // namespace network
} // namespace liveai
