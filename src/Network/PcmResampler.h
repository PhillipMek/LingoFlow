#pragma once
//
// PcmResampler - the fixed-ratio sample rate converter inside the translation
// backend (task 009, docs/openai-realtime-protocol.md section 7: "a resampler is
// mandatory inside the backend, on the network/worker thread").
//
// Scope decided by the protocol and the device envelope, not by ambition: the
// provider speaks 24 kHz on the wire; the product's devices run at the config
// set 44.1/48/88.2/96 kHz (ConfigSchema's kSupportedSampleRates) plus 24. Two
// converters live here, and everything else is refused loudly at configuration
// time - an unsupported pair must become a rejected session, never a silent
// guess (AGENTS.md 8, 19):
//   * a halfband cascade for the power-of-two relations (ratios 1, 2, 4 in
//     either direction, e.g. 48<->24, 96<->24, 88.2<->44.1), 15-tap, stop-band
//     roughly -50 dB - far below anything audible in a speech monitoring path;
//   * a rational polyphase stage for the pairs the wire actually meets:
//     24000 <-> 44100 (147/80) and 24000 <-> 88200 (147/40), added by code
//     review P1 (2026-10-05) because the config permitted those device rates
//     while the backend refused them - the show would have died at Start
//     Translation. The phase table is EXACT: the ratio reduces to integers
//     (in/gcd, out/gcd), the coefficient pattern repeats with that period, so
//     there is no fractional drift over a two-hour show. 49-tap Blackman
//     windowed-sinc, cutoff at half of min(in, out); TestPcmResampler measures
//     and pins the real pass-band and stop-band behavior (no paper claims).
//
// The object is STATEFUL across process() calls: the filter history continues
// from chunk to chunk, so the resampled stream has no seams - the streaming
// tests assert chunked output equals one-shot output bit for bit. It is not
// thread-safe and does not need to be: one instance lives on one network
// thread.

#include <vector>

namespace liveai {
namespace network {

class PcmResampler
{
public:
    /// True when both rates are positive and the pair is either a power-of-two
    /// relation (1, 2, 4 in either direction) or one of the wire pairs the
    /// rational stage exists for: 24000 <-> 44100 and 24000 <-> 88200.
    /// Nothing else - 32000 Hz or 22050 <-> 24000 are refused by design.
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
    /// transients and the rational stage's lookahead bound. Callers size their
    /// buffer with this and process() cannot overflow it.
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

    // ---- rational stage (24000 <-> 44100 / 88200): exact periodic polyphase ----
    /// Builds the coefficient table (outS rows of kRationalTaps floats, each row
    /// DC-normalised to unity) from the reduced ratio inS/outS.
    bool configureRational(int inputRate, int outputRate);
    int runRational(const float* in, int n, float* out) noexcept;
    /// Zero-padded stream start: clears the window and the absolute counters
    /// without touching the coefficient table.
    void resetRationalStream();

    int inputRate_ = 0;
    int outputRate_ = 0;
    int downStages_ = 0;
    int upStages_ = 0;

    std::vector<DownState> downs_;
    std::vector<UpState> ups_;

    std::vector<float> scratch_; // intermediate buffer between cascade stages

    // Rational-stage state (all of it stream position bookkeeping, bounded).
    bool rational_ = false;
    long long inSpan_ = 0;     ///< input samples per period = inRate / gcd
    long long outSpan_ = 0;    ///< output samples per period = outRate / gcd
    std::vector<float> coeffs_; ///< outSpan * kRationalTaps, row (outCount % outSpan)
    std::vector<float> xin_;   ///< the sliding window: input samples from baseAbs on
    long long absIn_ = 0;      ///< absolute input samples ever seen since configure/reset
    long long absOut_ = 0;     ///< the next output's absolute index since configure/reset
    long long baseAbs_ = 0;    ///< absolute input index of xin_[0]
};

} // namespace network
} // namespace liveai
