#pragma once
//
// WavFile - the minimal WAV I/O the developer mode (task 019) needs, and nothing
// more. The product plays audio through ASIO, not through files; this module
// exists so the pipeline can be run, recorded and rehearsed WITHOUT a sound
// device and without the OpenAI account - "where practical", as the task says,
// means exactly these two directions:
//
//   * readMono: PCM16/PCM24/PCM32 and IEEE-float32 files, mono or stereo
//     (stereo is averaged down to mono, the product's channel model), at any
//     sample rate - the caller decides what rate is acceptable and says so with
//     its own words (the simulated device refuses a file that disagrees with the
//     settings rather than resampling silently: the rule task 007 set for
//     delivered audio applies to read audio too).
//   * Mono16Writer: float -> PCM16, header reserved at open and patched at
//     close, so an interrupted recording is salvageable and no whole-show
//     buffer is ever held in memory.
//
// The probe tools (task 009) carry their own tiny mono16 reader/writer next to
// their main(); this module is the portable home for the product's side.
// Everything here runs on worker threads - file I/O is never realtime-safe,
// which is precisely why only backends that simulate a device call it.

#include <cstdint>
#include <string>
#include <vector>

namespace liveai {
namespace wavio {

/// A decoded mono WAV file. `samples` are floats in [-1, 1] (PCM integer full
/// scale maps to 1.0; float payloads pass through).
struct WavAudio
{
    std::vector<float> samples;
    int sampleRate = 0;
    int sourceChannels = 1;   ///< 1 or 2 - 2 means the file was stereo and was downmixed
};

/// Reads a WAV file. Returns false with a human-readable `error` naming exactly
/// what about the file was unusable. Never guesses: a missing chunk, an
/// unsupported format tag or a zero sample rate is an error, not a default.
bool readMono(const std::string& path, WavAudio& out, std::string& error);

/// Streaming PCM16 mono writer: open() writes a 44-byte header with reserved
/// sizes, write() appends samples, close() patches the sizes. If close() never
/// runs (a crash mid-rehearsal), the data bytes are still on disk and readable
/// by any player - the sizes just say what the last flush promised.
/// Errors after open() are not thrown and not swallowed: the first one is kept
/// in lastError() and close() reports the failure, so a silently lost recording
/// cannot pretend to have happened.
class Mono16Writer final
{
public:
    Mono16Writer() = default;
    ~Mono16Writer();

    Mono16Writer(const Mono16Writer&) = delete;
    Mono16Writer& operator=(const Mono16Writer&) = delete;

    bool open(const std::string& path, int sampleRate, std::string& error);

    /// Converts float samples to PCM16 and appends them. `frames` is the number
    /// of samples (mono). Safe to call only while open; a failed write sets the
    /// error state and later calls do nothing but keep the state.
    void write(const float* samples, std::uint64_t frames);

    /// Patches the header sizes and closes the file. Returns false when the
    /// file could not be finalized (reason in `error`). Idempotent.
    bool close(std::string& error);

    std::uint64_t framesWritten() const noexcept { return framesWritten_; }
    const std::string& lastError() const noexcept { return lastError_; }

private:
    void flushScratch();

    std::string path_;
    void* file_ = nullptr;   ///< a FILE*, hidden to keep <cstdio> out of the header
    bool open_ = false;
    std::uint64_t framesWritten_ = 0;
    std::string lastError_;

    /// Fixed reused buffer: 256 frames of PCM16 between flushes, so steady-state
    /// writing allocates nothing.
    static constexpr std::size_t kScratchFrames = 256;
    std::uint8_t scratchBuf_[kScratchFrames * 2];
    std::size_t scratchPos_ = 0;
};

} // namespace wavio
} // namespace liveai
