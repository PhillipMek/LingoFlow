#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "Utils/WavFile.h"

using liveai::wavio::Mono16Writer;
using liveai::wavio::WavAudio;

namespace {

struct TempFile
{
    std::filesystem::path path;

    TempFile()
        : path(std::filesystem::temp_directory_path()
               / ("lingoflow_wav_test_" + std::to_string(counter()++) + ".wav"))
    {
    }

    ~TempFile()
    {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }

    static int& counter()
    {
        static int value = 0;
        return value;
    }
};

// A hand-built little WAV writer for the tests, so the READER gets tested by
// something that is not the writer under test.
void writeRawWav(const std::filesystem::path& file, std::uint16_t formatTag, std::uint16_t channels,
                 std::uint32_t sampleRate, std::uint16_t bits, const std::vector<std::uint8_t>& data)
{
    std::ofstream out(file, std::ios::binary | std::ios::trunc);

    const std::uint32_t byteRate = sampleRate * channels * (bits / 8);
    const std::uint16_t blockAlign = static_cast<std::uint16_t>(channels * (bits / 8));

    auto put32 = [&](std::uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
    auto put16 = [&](std::uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };

    out.write("RIFF", 4);
    put32(36 + static_cast<std::uint32_t>(data.size()));
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    put32(16);
    put16(formatTag);
    put16(channels);
    put32(sampleRate);
    put32(byteRate);
    put16(blockAlign);
    put16(bits);
    out.write("data", 4);
    put32(static_cast<std::uint32_t>(data.size()));
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

std::vector<std::uint8_t> pcm16(const std::vector<int>& values)
{
    std::vector<std::uint8_t> bytes;

    for (const int v : values)
    {
        const auto u = static_cast<std::uint16_t>(v);
        bytes.push_back(static_cast<std::uint8_t>(u & 0xff));
        bytes.push_back(static_cast<std::uint8_t>((u >> 8) & 0xff));
    }

    return bytes;
}

} // namespace

TEST_CASE("WavFile: the writer's output reads back as the same signal", "[utils][wav]")
{
    TempFile file;
    std::string error;

    Mono16Writer writer;
    REQUIRE(writer.open(file.path.string(), 48000, error));

    // A shape the PCM16 grid can represent closely: full scale, half scale,
    // silence and the negative floor.
    const std::vector<float> samples{ 1.0f, 0.5f, 0.0f, -1.0f, -0.25f, 0.125f };
    writer.write(samples.data(), samples.size());
    REQUIRE(writer.close(error));
    CHECK(writer.framesWritten() == samples.size());

    WavAudio read;
    REQUIRE(liveai::wavio::readMono(file.path.string(), read, error));
    CHECK(read.sampleRate == 48000);
    CHECK(read.sourceChannels == 1);
    REQUIRE(read.samples.size() == samples.size());

    // 16-bit quantisation plus the writer's 32767 scale vs the reader's 32768
    // divide: a few LSBs of honest difference, nothing that reorders the signal.
    for (std::size_t i = 0; i < samples.size(); ++i)
    {
        INFO(i);
        CHECK(std::abs(read.samples[i] - samples[i]) < 2.0f / 32768.0f);
    }

    // Sizes in the header must match the file on disk exactly - a player reads
    // the declared length, and a rehearsal file that lies is a broken receipt.
    const auto onDisk = std::filesystem::file_size(file.path);
    CHECK(onDisk == 44 + samples.size() * 2);
}

TEST_CASE("WavFile: the reader speaks the formats files actually come in", "[utils][wav]")
{
    TempFile file;
    WavAudio read;
    std::string error;

    SECTION("PCM24")
    {
        std::vector<std::uint8_t> bytes;

        for (const int v : { 0, 8388607, -8388608, 4194304 })
        {
            bytes.push_back(static_cast<std::uint8_t>(v & 0xff));
            bytes.push_back(static_cast<std::uint8_t>((v >> 8) & 0xff));
            bytes.push_back(static_cast<std::uint8_t>((v >> 16) & 0xff));
        }

        writeRawWav(file.path, 1, 1, 44100, 24, bytes);
        REQUIRE(liveai::wavio::readMono(file.path.string(), read, error));
        CHECK(read.sampleRate == 44100);
        REQUIRE(read.samples.size() == 4);
        CHECK(std::abs(read.samples[1] - (8388607.0f / 8388608.0f)) < 0.0001f);
        CHECK(std::abs(read.samples[2] + 1.0f) < 0.0001f);
    }

    SECTION("PCM32")
    {
        std::vector<std::uint8_t> bytes;
        const std::int32_t values[] = { 0, 2147483647, -2147483648 };

        for (const std::int32_t v : values)
        {
            const auto u = static_cast<std::uint32_t>(v);
            for (int i = 0; i < 4; ++i)
                bytes.push_back(static_cast<std::uint8_t>((u >> (8 * i)) & 0xff));
        }

        writeRawWav(file.path, 1, 1, 48000, 32, bytes);
        REQUIRE(liveai::wavio::readMono(file.path.string(), read, error));
        REQUIRE(read.samples.size() == 3);
        CHECK(std::abs(read.samples[2] + 1.0f) < 0.0001f);
    }

    SECTION("float32 stereo downmixes to mono")
    {
        std::vector<std::uint8_t> bytes;
        const float pairs[][2] = { { 1.0f, -1.0f }, { 0.5f, 0.25f }, { 0.0f, 0.0f } };

        for (const auto& pair : pairs)
        {
            for (const float f : pair)
            {
                std::uint8_t raw[4];
                std::memcpy(raw, &f, 4);
                bytes.insert(bytes.end(), raw, raw + 4);
            }
        }

        writeRawWav(file.path, 3, 2, 48000, 32, bytes);
        REQUIRE(liveai::wavio::readMono(file.path.string(), read, error));
        CHECK(read.sourceChannels == 2);
        REQUIRE(read.samples.size() == 3);
        CHECK(read.samples[0] == 0.0f);        // (1 + -1) / 2
        CHECK(std::abs(read.samples[1] - 0.375f) < 0.001f);
    }

    SECTION("a LIST chunk of odd size does not derail the walk")
    {
        std::ofstream out(file.path, std::ios::binary | std::ios::trunc);
        std::string note = "note";   // 4 bytes, padded to the evenness rule below anyway

        auto put32 = [&](std::uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
        auto put16 = [&](std::uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };

        const std::vector<std::uint8_t> data = pcm16({ 0, 16384 });
        const std::uint32_t listSize = 5;   // odd: the walker must skip the pad byte

        out.write("RIFF", 4);
        put32(4 + 24 + 8 + listSize + 8 + static_cast<std::uint32_t>(data.size()) + 1);
        out.write("WAVE", 4);
        out.write("fmt ", 4);
        put32(16);
        put16(1);
        put16(1);
        put32(48000);
        put32(96000);
        put16(2);
        put16(16);
        out.write("LIST", 4);
        put32(listSize);
        out.write(note.data(), 4);
        out.write("\0", 1);   // the 5th byte
        out.write("\0", 1);   // the pad byte
        out.write("data", 4);
        put32(static_cast<std::uint32_t>(data.size()));
        out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        out.close();

        REQUIRE(liveai::wavio::readMono(file.path.string(), read, error));
        REQUIRE(read.samples.size() == 2);
        CHECK(std::abs(read.samples[1] - 0.5f) < 0.001f);
    }
}

TEST_CASE("WavFile: refuses what it cannot honestly read", "[utils][wav]")
{
    TempFile file;
    WavAudio read;
    std::string error;

    SECTION("not a RIFF file")
    {
        std::ofstream out(file.path, std::ios::binary | std::ios::trunc);
        out << "this is not a wav file at all, not even close";
        out.close();

        CHECK_FALSE(liveai::wavio::readMono(file.path.string(), read, error));
        CHECK(error.find("RIFF") != std::string::npos);
    }

    SECTION("unsupported format tag names itself in the error")
    {
        writeRawWav(file.path, 7 /* mu-law */, 1, 48000, 8, pcm16({ 0, 0 }));
        CHECK_FALSE(liveai::wavio::readMono(file.path.string(), read, error));
        CHECK(error.find("format tag 7") != std::string::npos);
    }

    SECTION("five channels are not a thing we guess down")
    {
        writeRawWav(file.path, 1, 5, 48000, 16, pcm16({ 0, 0, 0, 0, 0 }));
        CHECK_FALSE(liveai::wavio::readMono(file.path.string(), read, error));
        CHECK(error.find("5 channels") != std::string::npos);
    }

    SECTION("data shorter than its header claims")
    {
        auto bytes = pcm16({ 0, 100 });
        std::ofstream out(file.path, std::ios::binary | std::ios::trunc);
        out.write("RIFF", 4);
        const std::uint32_t sizes[] = { 100000, 16, 1, 1, 48000, 96000, 2, 16, 100000 };

        out.write(reinterpret_cast<const char*>(&sizes[0]), 4);   // riff size
        out.write("WAVE", 4);
        out.write("fmt ", 4);
        out.write(reinterpret_cast<const char*>(&sizes[1]), 4);   // fmt size
        out.write(reinterpret_cast<const char*>(&sizes[2]), 2);   // tag
        out.write(reinterpret_cast<const char*>(&sizes[3]), 2);   // channels
        out.write(reinterpret_cast<const char*>(&sizes[4]), 4);   // rate
        out.write(reinterpret_cast<const char*>(&sizes[5]), 4);   // byterate
        out.write(reinterpret_cast<const char*>(&sizes[6]), 2);   // align
        out.write(reinterpret_cast<const char*>(&sizes[7]), 2);   // bits
        out.write("data", 4);
        out.write(reinterpret_cast<const char*>(&sizes[8]), 4);   // a lie about size
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        out.close();

        CHECK_FALSE(liveai::wavio::readMono(file.path.string(), read, error));
        CHECK(error.find("shorter than its header claims") != std::string::npos);
    }

    SECTION("a writer that was never given samples still closes as a valid empty file")
    {
        Mono16Writer writer;
        REQUIRE(writer.open(file.path.string(), 44100, error));
        REQUIRE(writer.close(error));

        // The reader refuses an empty data chunk - honestly, without guessing
        // the file is fine for players; the product just will not "play" nothing.
        CHECK_FALSE(liveai::wavio::readMono(file.path.string(), read, error));
        CHECK(error.find("data") != std::string::npos);
    }
}

TEST_CASE("WavFile: the simulated device refuses a rate it was not asked for", "[utils][wav][dev]")
{
    // The check itself lives in the backend; here the fixture proves the file
    // really is 44100, so a mismatch failure downstream cannot be blamed on
    // the reader.
    TempFile file;
    std::string error;

    Mono16Writer writer;
    REQUIRE(writer.open(file.path.string(), 44100, error));
    const std::vector<float> zeros(128, 0.0f);
    writer.write(zeros.data(), zeros.size());
    REQUIRE(writer.close(error));

    WavAudio read;
    REQUIRE(liveai::wavio::readMono(file.path.string(), read, error));
    CHECK(read.sampleRate == 44100);
}
