//
// lingoflow_openai_probe - the live validation tool for the live translation chain (HUMAN
// CHECKPOINT support, protocol doc section 14/15 workflow).
//
// It exercises the EXACT backend the operator app will link, over the real
// network: open a translation session, stream a WAV file through submitAudio()
// at wall-clock pace, collect the translated audio and the transcript from the
// sink, close gracefully (the drain window included) and write the delivered
// audio back to a WAV the operator can listen to.
//
// Credentials: process environment OPENAI_API_KEY first, then
// HKCU\Environment (the Windows user-variable the owner set); the value is
// never printed, only its presence. Nothing here is a product path - the product
// owns the production credential store.
//
// Exit code 0 = connected, streamed, received translated audio, closed clean.
// 1 = anything else; the printed diagnostics say which.

#define _CRT_SECURE_NO_WARNINGS 1

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Network/OpenAIRealtimeBackend.h"
#include "Security/ISecretStore.h"
#include "Utils/Log.h"

namespace {

using namespace liveai;

class EnvSecretStore final : public security::ISecretStore
{
public:
    std::string_view name() const noexcept override { return "Development environment"; }

    security::SecretStatus store(std::string_view, std::string_view) override
    {
        return security::SecretStatus::unavailable; // read-only dev store
    }

    std::optional<std::string> load(std::string_view identifier) override
    {
        if (identifier != security::kOpenAiApiKey)
            return std::nullopt;

        if (const char* env = std::getenv("OPENAI_API_KEY"); env != nullptr && env[0] != '\0')
            return std::string(env);

        wchar_t buffer[4096];
        DWORD size = sizeof(buffer);
        if (RegGetValueW(HKEY_CURRENT_USER, L"Environment", L"OPENAI_API_KEY", RRF_RT_REG_SZ,
                         nullptr, buffer, &size) == ERROR_SUCCESS)
        {
            const int len = WideCharToMultiByte(CP_UTF8, 0, buffer, -1, nullptr, 0, nullptr,
                                                nullptr);
            if (len > 1)
            {
                std::string out(static_cast<std::size_t>(len - 1), '\0');
                WideCharToMultiByte(CP_UTF8, 0, buffer, -1, out.data(), len, nullptr, nullptr);
                return out;
            }
        }
        return std::nullopt;
    }

    security::SecretStatus remove(std::string_view) override
    {
        return security::SecretStatus::unavailable;
    }

    std::vector<std::string> identifiers() const override
    {
        return { std::string(security::kOpenAiApiKey) };
    }
};

// ------------------------------------------------------------------ tiny WAV IO

struct WavData
{
    int sampleRate = 0;
    std::vector<std::int16_t> samples; // mono PCM16
};

bool readWavMono16(const char* path, WavData& out)
{
    std::FILE* f = std::fopen(path, "rb");
    if (f == nullptr)
        return false;

    char riff[4] {};
    std::fseek(f, 0, SEEK_SET);
    if (std::fread(riff, 1, 4, f) != 4 || std::memcmp(riff, "RIFF", 4) != 0)
    {
        std::fclose(f);
        return false;
    }
    std::fseek(f, 8, SEEK_SET); // skip the 4-byte RIFF size, land on "WAVE"

    char wave[4] {};
    if (std::fread(wave, 1, 4, f) != 4 || std::memcmp(wave, "WAVE", 4) != 0)
    {
        std::fclose(f);
        return false;
    }
    // chunk loop reads from offset 12

    bool haveFormat = false;
    int channels = 0;
    int bits = 0;
    long dataBytes = 0;
    long dataStart = 0;

    while (true)
    {
        char id[4] {};
        DWORD size = 0;
        if (std::fread(id, 1, 4, f) != 4 || std::fread(&size, 1, 4, f) != 4)
            break;
        const long pos = std::ftell(f);
        if (std::memcmp(id, "fmt ", 4) == 0)
        {
            std::uint16_t fmt = 0, ch = 0;
            std::uint32_t rate = 0;
            std::fread(&fmt, 2, 1, f);
            std::fread(&ch, 2, 1, f);
            std::fread(&rate, 4, 1, f);
            std::fseek(f, pos + 14, SEEK_SET); // bits-per-sample is at offset 14
            std::fread(&bits, 2, 1, f);
            if (fmt != 1)
            {
                std::printf("probe: only PCM WAV input is supported (got format %u)\n", fmt);
                std::fclose(f);
                return false;
            }
            channels = ch;
            out.sampleRate = static_cast<int>(rate);
            haveFormat = true;
        }
        else if (std::memcmp(id, "data", 4) == 0)
        {
            dataBytes = static_cast<long>(size);
            dataStart = pos;
        }
        std::fseek(f, pos + static_cast<long>(size + (size & 1)), SEEK_SET);
    }

    if (!haveFormat || dataBytes == 0 || channels != 1 || bits != 16)
    {
        std::printf("probe: input must be mono 16-bit PCM WAV (channels=%d bits=%d)\n", channels,
                    bits);
        std::fclose(f);
        return false;
    }

    std::fseek(f, dataStart, SEEK_SET);
    out.samples.resize(static_cast<std::size_t>(dataBytes / 2));
    const std::size_t got =
        std::fread(out.samples.data(), sizeof(std::int16_t), out.samples.size(), f);
    out.samples.resize(got);
    std::fclose(f);
    return got > 0;
}

bool writeWavMono16(const char* path, int sampleRate, const std::vector<std::int16_t>& samples)
{
    std::FILE* f = std::fopen(path, "wb");
    if (f == nullptr)
        return false;

    const std::uint32_t dataBytes = static_cast<std::uint32_t>(samples.size() * 2);
    const std::uint32_t byteRate = static_cast<std::uint32_t>(sampleRate * 2);
    const std::uint32_t chunkSize = 36 + dataBytes;

    auto w32 = [&](std::uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto w16 = [&](std::uint16_t v) { std::fwrite(&v, 2, 1, f); };
    auto wr = [&](const char* s) { std::fwrite(s, 1, 4, f); };

    wr("RIFF");
    w32(chunkSize);
    wr("WAVE");
    wr("fmt ");
    w32(16);
    w16(1); // PCM
    w16(1); // mono
    w32(static_cast<std::uint32_t>(sampleRate));
    w32(byteRate);
    w16(2); // block align
    w16(16);
    wr("data");
    w32(dataBytes);
    std::fwrite(samples.data(), sizeof(std::int16_t), samples.size(), f);
    std::fclose(f);
    return true;
}

// ------------------------------------------------------------------- probe sink

class ProbeSink final : public translation::ITranslationSink
{
public:
    void onTranslatedAudio(const float* samples, int frameCount, int sampleRate) override
    {
        std::lock_guard<std::mutex> lock (mutex_);
        if (deliveredRate_ == 0)
            deliveredRate_ = sampleRate;
        else if (deliveredRate_ != sampleRate)
            rateMismatch_ = true;
        for (int i = 0; i < frameCount; ++i)
        {
            float v = samples[i];
            if (v > 1.0f) v = 1.0f;
            if (v < -1.0f) v = -1.0f;
            delivered_.push_back(static_cast<std::int16_t>(std::lround(v * 32767.0f)));
        }
    }

    // Sink since the text pipeline: partial is a whole-line SNAPSHOT (replacement), a
    // final closes a line. text_ accumulates closed lines; openLine_ holds the
    // line still in progress, so text() never double-counts and never loses an
    // unfinished tail.
    void onPartialText(std::string_view text) override
    {
        std::lock_guard<std::mutex> lock (mutex_);
        openLine_.assign(text);
    }

    void onFinalText(std::string_view text) override
    {
        std::lock_guard<std::mutex> lock (mutex_);
        text_ += text;
        text_ += '\n';            // one settled line per final (probe display)
        openLine_.clear();
    }

    void onSessionStateChanged(translation::SessionState state) override
    {
        std::lock_guard<std::mutex> lock (mutex_);
        states_ += std::string(translation::nameOf(state)) + " ";
        lastState_ = state;
    }

    void onTranslationError(const translation::TranslationError& error) override
    {
        ++errorCount_;
        if (error.fatal)
            ++fatalCount_;
        std::lock_guard<std::mutex> lock (mutex_);
        std::printf("  error [%s]%s: %s\n", std::string(translation::nameOf(error.category)).c_str(),
                    error.fatal ? " FATAL" : "", error.message.c_str());
    }

    std::vector<std::int16_t> takeAudio()
    {
        std::lock_guard<std::mutex> lock (mutex_);
        return delivered_;
    }

    std::string text()
    {
        std::lock_guard<std::mutex> lock (mutex_);

        // Closed lines (one per final) plus whatever line is still open: the
        // whole truth of what the translator delivered, in delivery order.
        return text_ + openLine_;
    }

    std::string states()
    {
        std::lock_guard<std::mutex> lock (mutex_);
        return states_;
    }

    translation::SessionState lastState() const noexcept { return lastState_; }
    int deliveredRate() const noexcept { return deliveredRate_; }
    bool rateMismatch() const noexcept { return rateMismatch_; }
    int errorCount() const noexcept { return errorCount_; }
    int fatalCount() const noexcept { return fatalCount_; }

private:
    std::mutex mutex_;
    std::vector<std::int16_t> delivered_;
    std::string text_;      ///< settled lines, newline-separated
    std::string openLine_;  ///< the line partial snapshots currently describe
    std::string states_;
    translation::SessionState lastState_ = translation::SessionState::closed;
    int deliveredRate_ = 0;
    bool rateMismatch_ = false;
    std::atomic<int> errorCount_ { 0 };
    std::atomic<int> fatalCount_ { 0 };
};

} // namespace

int main(int argc, char** argv)
{
    SetConsoleOutputCP(CP_UTF8);
    setvbuf(stdout, nullptr, _IONBF, 0); // every progress line must survive a kill

    const char* inPath = argc > 1 ? argv[1] : nullptr;
    std::string outPath = argc > 2 ? argv[2] : "translated_probe_out.wav";
    std::string language = argc > 3 ? argv[3] : "ru";
    const int outputRate = argc > 4 ? std::atoi(argv[4]) : 48000;

    if (inPath == nullptr)
    {
        std::printf("usage: lingoflow_openai_probe <input-mono-pcm16.wav> [out.wav] [lang] "
                    "[output-rate 24000|48000|96000]\n");
        return 2;
    }

    log::configure (LogConfig { .level = LogLevel::info, .writeConsole = true, .filePath = {} });

    WavData wav;
    if (!readWavMono16(inPath, wav))
    {
        std::printf("probe: cannot read input WAV: %s\n", inPath);
        return 2;
    }
    std::printf("input: %d Hz mono PCM16, %.2f s\n", wav.sampleRate,
                static_cast<double>(wav.samples.size()) / wav.sampleRate);

    EnvSecretStore secrets;
    network::OpenAIRealtimeOptions options; // documented defaults

    network::OpenAIRealtimeBackend backend (secrets, options);
    ProbeSink sink;
    backend.setSink(sink);

    translation::SessionRequest request;
    request.pair.input = "en"; // wire does not carry it (auto-detected, section 12.3)
    request.pair.output = language;
    request.model = ""; // backend default, validated against the docs (section 11)
    request.inputSampleRate = wav.sampleRate;
    request.outputSampleRate = outputRate;

    std::printf("opening session (language=%s, in=%d, out=%d)...\n", language.c_str(),
                request.inputSampleRate, request.outputSampleRate);
    const auto start = GetTickCount64();
    std::string error;
    if (!backend.openSession(request, error))
    {
        std::printf("probe: openSession failed: %s\n", error.c_str());
        return 1;
    }
    std::printf("session connected in %llu ms; states: %s\n",
                static_cast<unsigned long long>(GetTickCount64() - start),
                sink.states().c_str());

    // Stream at wall-clock pace (this is a live-event rehearsal, not a speed
    // test): submitAudio gets 100 ms chunks; the backend fills its own 200 ms
    // cadence, including silence between submissions.
    const int chunk = wav.sampleRate / 10;
    std::vector<float> f (static_cast<std::size_t>(chunk));
    for (std::size_t offset = 0; offset < wav.samples.size(); offset += static_cast<std::size_t>(chunk))
    {
        const std::size_t n =
            std::min<std::size_t>(static_cast<std::size_t>(chunk), wav.samples.size() - offset);
        for (std::size_t i = 0; i < n; ++i)
            f[i] = static_cast<float>(wav.samples[offset + i]) / 32768.0f;
        if (!backend.submitAudio(f.data(), static_cast<int>(n), error))
        {
            std::printf("probe: submitAudio refused: %s\n", error.c_str());
            break;
        }
        Sleep(100);
    }
    const auto fedMs = GetTickCount64() - start;
    std::printf("fed %.2f s of audio in %llu ms; tailing 2 s of live silence...\n",
                wav.samples.size() / static_cast<double>(wav.sampleRate),
                static_cast<unsigned long long>(fedMs));
    Sleep(2000);

    const auto closeStart = GetTickCount64();
    backend.closeSession(); // graceful flush: session.close + drain until session.closed
    std::printf("closeSession (incl. drain) took %llu ms\n",
                static_cast<unsigned long long>(GetTickCount64() - closeStart));

    const auto audio = sink.takeAudio();
    const std::string text = sink.text();
    std::printf("states seen: %s\n", sink.states().c_str());
    std::printf("translated audio: %zu samples at %d Hz (%.2f s), rate mismatch=%s\n",
                audio.size(), sink.deliveredRate(),
                sink.deliveredRate() > 0 ? audio.size() / static_cast<double>(sink.deliveredRate()) : 0.0,
                sink.rateMismatch() ? "YES" : "no");
    std::printf("translated text (%zu chars): %.200s\n", text.size(), text.c_str());
    std::printf("errors: %d (%d fatal), session seconds: %.1f\n", sink.errorCount(),
                sink.fatalCount(), static_cast<double>(GetTickCount64() - start) / 1000.0);

    if (!audio.empty() && sink.deliveredRate() > 0)
    {
        if (writeWavMono16(outPath.c_str(), sink.deliveredRate(), audio))
            std::printf("wrote %s\n", outPath.c_str());
        else
            std::printf("probe: could not write %s\n", outPath.c_str());
    }

    const bool ok = sink.lastState() == translation::SessionState::closed
                 && !sink.rateMismatch() && sink.fatalCount() == 0 && !audio.empty();
    std::printf("probe: %s\n", ok ? "PASS (functional live round-trip)" : "FAIL");
    return ok ? 0 : 1;
}
