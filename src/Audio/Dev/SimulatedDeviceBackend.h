#pragma once
//
// Simulated devices - the developer mode's stand-ins for sound hardware
// (task 019, "allow operation without SoundGrid").
//
// A SimulatedDeviceBackend is an IAudioBackend that is not a driver: it owns a
// worker thread that delivers input blocks to the engine and takes output
// blocks back at the configured pace, the way an ASIO callback would - minus
// the driver. This is exactly what makes the whole product exercisable with no
// hardware: the engine cannot tell the difference (same contract, same block
// sizes, same rates), and the operator can rehearse the chain on a laptop.
//
// Honesty rules this module keeps:
//   * It is a DEVELOPER feature. The composition root mounts it only when the
//     task 019 developer plan says so (App/DeveloperMode.h); nothing here is
//     selected by default, and the log and the operator badge say which device
//     story is actually running. The name() strings carry "(developer source)"
//     so no status line can be mistaken for a SoundGrid machine.
//   * The file backend refuses a WAV whose sample rate disagrees with the
//     settings instead of resampling silently - the delivered-audio rule of
//     task 007 applied to the input side: a wrong speed is an error, not a
//     detail.
//   * The pacing thread measures its OWN lateness (lateBlocks): a simulated
//     device on a busy laptop drifts, pretending otherwise would be fake
//     instrumentation. The counters are printed when the device closes.
//   * File I/O happens only on this worker thread between processAudio()
//     calls - never inside the engine callback - so the realtime rules of
//     AGENTS.md 5 stay intact for the pipeline itself.
//
// Threading: the lifecycle calls (open/start/stop/close/capabilities) come
// from the controller thread, as with any backend. The pacing worker is owned
// and joined inside start()/stop().

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "Audio/IAudioBackend.h"
#include "Utils/WavFile.h"

namespace liveai {
namespace audio {

/// Base: lifecycle, pacing, block plumbing and the optional output recording.
class SimulatedDeviceBackend : public IAudioBackend
{
public:
    /// `recordPath` empty = do not record. When set, every block the engine
    /// produced for "the audience" is appended to this PCM16 WAV file.
    explicit SimulatedDeviceBackend(std::string recordPath = {});
    ~SimulatedDeviceBackend() override;

    SimulatedDeviceBackend(const SimulatedDeviceBackend&) = delete;
    SimulatedDeviceBackend& operator=(const SimulatedDeviceBackend&) = delete;

    BackendState state() const noexcept override { return state_; }
    DeviceCapabilities capabilities() const noexcept override { return capabilities_; }

    bool open(IAudioProcessor& processor, const DeviceRequest& request, std::string& error) override;
    bool start(std::string& error) override;
    bool stop(std::string& error) override;
    void close() noexcept override;

    /// Pacing evidence (see the header: this measures the simulator, not the audio).
    std::uint64_t deliveredBlocks() const noexcept { return deliveredBlocks_.load(std::memory_order_relaxed); }
    std::uint64_t lateBlocks() const noexcept { return lateBlocks_.load(std::memory_order_relaxed); }

protected:
    /// Source hook: the derived class reports the sample rate its input really
    /// has (a WAV file knows; a tone takes the requested one). A source that
    /// cannot start fills `error` and returns false - open() then fails the
    /// same honest way a driver open does.
    virtual bool openSource(const DeviceRequest& request, int& outSampleRate, std::string& error) = 0;
    virtual void closeSource() noexcept {}

    /// Called on the pacing thread before each processAudio(): fill `dest`
    /// with exactly `frames` input samples.
    virtual void provideInput(float* dest, int frames) = 0;

    /// Called after processAudio() returns; the base records output. Derived
    /// classes that add behaviour must call the base implementation.
    virtual void consumeOutput(const float* data, int frames);

    /// The sample rate the source settled on (valid after open()).
    int sourceSampleRate() const noexcept { return capabilities_.sampleRate; }

private:
    void run();

    IAudioProcessor* processor_ = nullptr;
    DeviceCapabilities capabilities_;
    BackendState state_ = BackendState::closed;

    std::vector<float> input_;
    std::vector<float> output_;
    std::vector<const float*> inputPointers_;
    std::vector<float*> outputPointers_;

    std::thread thread_;
    std::atomic<bool> stopRequested_ { false };
    std::atomic<std::uint64_t> deliveredBlocks_ { 0 };
    std::atomic<std::uint64_t> lateBlocks_ { 0 };

    std::string recordPath_;
    wavio::Mono16Writer writer_;
    std::string recordError_;
};

/// Plays a WAV file as its "microphone" (mono/stereo, PCM16/24/32 or float32;
/// stereo is downmixed by the reader). The file loops back to its start when
/// it ends - a rehearsal input that silently stops is a worse surprise than a
/// repeating one, and the log marks the first wrap.
class WavFileAudioBackend final : public SimulatedDeviceBackend
{
public:
    WavFileAudioBackend(std::string inputPath, std::string recordPath = {});

    std::string_view name() const noexcept override { return "WAV file (developer source)"; }

protected:
    bool openSource(const DeviceRequest& request, int& outSampleRate, std::string& error) override;
    void provideInput(float* dest, int frames) override;

private:
    std::string inputPath_;
    wavio::WavAudio samples_;
    std::size_t position_ = 0;
    bool wrappedOnce_ = false;
};

/// A sine generator as its "microphone" - level-meter zeroing, gain staging
/// and pacing checks need a signal with known amplitude and no room.
class TestToneAudioBackend final : public SimulatedDeviceBackend
{
public:
    TestToneAudioBackend(double frequencyHz, double levelDb, std::string recordPath = {});

    std::string_view name() const noexcept override { return "Test tone (developer source)"; }

protected:
    bool openSource(const DeviceRequest& request, int& outSampleRate, std::string& error) override;
    void provideInput(float* dest, int frames) override;

private:
    double frequencyHz_;
    double levelDb_;
    double phase_ = 0.0;
    double phaseStep_ = 0.0;
    float amplitude_ = 0.0f;
};

} // namespace audio
} // namespace liveai
