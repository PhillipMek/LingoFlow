// std::fopen is the portable file call; fopen_s is a Microsoft extension that
// would make the core unportable for the sake of one warning - the same
// reasoning and the same macro the probe tools of tasks 009/012 use. The
// define must precede <cstdio>, where the CRT's deprecation lives.
#define _CRT_SECURE_NO_WARNINGS 1

#include "Utils/WavFile.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace liveai {
namespace wavio {
namespace {

std::uint32_t le32(const std::uint8_t* p)
{
    return static_cast<std::uint32_t> (p[0])
         | static_cast<std::uint32_t> (p[1]) << 8
         | static_cast<std::uint32_t> (p[2]) << 16
         | static_cast<std::uint32_t> (p[3]) << 24;
}

std::uint16_t le16(const std::uint8_t* p)
{
    return static_cast<std::uint16_t> (p[0]) | static_cast<std::uint16_t> (p[1]) << 8;
}

void putLe16(std::uint8_t* p, std::uint16_t v)
{
    p[0] = static_cast<std::uint8_t> (v & 0xffu);
    p[1] = static_cast<std::uint8_t> ((v >> 8) & 0xffu);
}

void putLe32(std::uint8_t* p, std::uint32_t v)
{
    p[0] = static_cast<std::uint8_t> (v & 0xffu);
    p[1] = static_cast<std::uint8_t> ((v >> 8) & 0xffu);
    p[2] = static_cast<std::uint8_t> ((v >> 16) & 0xffu);
    p[3] = static_cast<std::uint8_t> ((v >> 24) & 0xffu);
}

float clampToUnit(float x)
{
    if (x < -1.0f)
        return -1.0f;

    return x > 1.0f ? 1.0f : x;
}

} // namespace

bool readMono(const std::string& path, WavAudio& out, std::string& error)
{
    out = {};

    std::FILE* file = std::fopen(path.c_str(), "rb");

    if (file == nullptr)
    {
        error = "could not open '" + path + "' for reading";
        return false;
    }

    auto fail = [&error, file](const std::string& reason)
    {
        error = reason;
        std::fclose(file);
        return false;
    };

    std::uint8_t header[12];

    if (std::fread(header, 1, sizeof(header), file) != sizeof(header))
        return fail("file is shorter than a WAV header: '" + path + "'");

    if (std::memcmp(header, "RIFF", 4) != 0)
        return fail("not a RIFF file: '" + path + "'");

    if (std::memcmp(header + 8, "WAVE", 4) != 0)
        return fail("a RIFF file that is not WAVE: '" + path + "'");

    // Chunk walk. The data chunk is usually after fmt, but a file written by an
    // editor can put LIST/NOTE anywhere: offsets are recorded and decoded only
    // after both fmt and data were seen.
    std::uint16_t formatTag = 0;
    std::uint16_t channels = 0;
    std::uint16_t bits = 0;
    std::uint32_t sampleRate = 0;
    std::uint64_t dataOffset = 0;
    std::uint64_t dataSize = 0;
    bool haveFmt = false;
    bool haveData = false;

    for (;;)
    {
        std::uint8_t chunkHeader[8];

        if (std::fread(chunkHeader, 1, sizeof(chunkHeader), file) != sizeof(chunkHeader))
            break;   // end of file: the walk stops, the checks below decide

        const char* id = reinterpret_cast<const char*> (chunkHeader);
        const std::uint32_t chunkSize = le32(chunkHeader + 4);

        if (std::memcmp(id, "fmt ", 4) == 0)
        {
            if (chunkSize < 16)
                return fail("a 'fmt ' chunk shorter than 16 bytes");

            std::uint8_t fmt[16];

            if (std::fread(fmt, 1, sizeof(fmt), file) != sizeof(fmt))
                return fail("the 'fmt ' chunk is truncated");

            formatTag = le16(fmt);
            channels = le16(fmt + 2);
            sampleRate = le32(fmt + 4);
            bits = le16(fmt + 14);
            haveFmt = true;

            if (chunkSize > 16)
                std::fseek(file, static_cast<long> (chunkSize - 16), SEEK_CUR);
        }
        else if (std::memcmp(id, "data", 4) == 0)
        {
            dataOffset = static_cast<std::uint64_t> (std::ftell(file));
            dataSize = chunkSize;
            haveData = true;

            // Jump out to the chunk end (+ the pad byte odd chunks carry).
            std::fseek(file, static_cast<long> (chunkSize + (chunkSize & 1u)), SEEK_CUR);
        }
        else
        {
            // Unknown chunk: skip it whole (+ pad). Its content is the file's
            // business; a product that never invents formats does not read one.
            if (std::fseek(file, static_cast<long> (chunkSize + (chunkSize & 1u)), SEEK_CUR) != 0)
                break;
        }
    }

    if (!haveFmt)
        return fail("no 'fmt ' chunk in '" + path + "'");

    if (!haveData || dataSize == 0)
        return fail("no audio 'data' chunk in '" + path + "'");

    if (sampleRate == 0)
        return fail("the WAV header declares a sample rate of 0 - nothing sane can be done with it");

    if (channels != 1 && channels != 2)
        return fail("only mono and stereo WAV files are read, this one has "
                    + std::to_string(channels) + " channels");

    const int bytesPerSample = bits / 8;

    if (formatTag != 1 && formatTag != 3)
        return fail("unsupported WAV encoding: format tag " + std::to_string(formatTag)
                    + " (only PCM=1 and IEEE float=3 are read)");

    if ((formatTag == 1 && bits != 16 && bits != 24 && bits != 32)
        || (formatTag == 3 && bits != 32))
        return fail("unsupported bit depth " + std::to_string(bits)
                    + " for format tag " + std::to_string(formatTag));

    const std::uint64_t frameBytes = static_cast<std::uint64_t> (channels) * bytesPerSample;
    const std::uint64_t frames = dataSize / frameBytes;

    if (frames == 0)
        return fail("the 'data' chunk holds no whole frame");

    if (std::fseek(file, static_cast<long> (dataOffset), SEEK_SET) != 0)
        return fail("could not seek back to the 'data' chunk");

    std::vector<std::uint8_t> raw(static_cast<std::size_t> (dataSize - dataSize % frameBytes));

    if (std::fread(raw.data(), 1, raw.size(), file) != raw.size())
        return fail("the 'data' chunk is shorter than its header claims");

    std::fclose(file);

    out.sampleRate = static_cast<int> (sampleRate);
    out.sourceChannels = channels;
    out.samples.resize(static_cast<std::size_t> (frames));

    const std::uint8_t* p = raw.data();

    for (std::uint64_t frame = 0; frame < frames; ++frame)
    {
        double mixed = 0.0;

        for (int channel = 0; channel < channels; ++channel)
        {
            const std::uint8_t* s = p + frame * frameBytes + static_cast<std::uint64_t> (channel) * bytesPerSample;
            double value = 0.0;

            if (formatTag == 3)
            {
                float f;
                std::memcpy(&f, s, 4);
                value = f;
            }
            else if (bits == 16)
            {
                value = static_cast<std::int16_t> (le16(s)) / 32768.0;
            }
            else if (bits == 24)
            {
                std::int32_t v = s[0] | (static_cast<std::int32_t> (s[1]) << 8)
                               | (static_cast<std::int32_t> (s[2]) << 16);

                if (v & 0x800000)
                    v |= ~0x7fffff;

                value = v / 8388608.0;
            }
            else   // bits == 32 PCM
            {
                const std::int32_t v = static_cast<std::int32_t> (le32(s));
                value = v / 2147483648.0;
            }

            mixed += value;
        }

        out.samples[static_cast<std::size_t> (frame)] =
            clampToUnit(static_cast<float> (mixed / channels));
    }

    return true;
}

// --------------------------------------------------------------------------- writer

namespace {

constexpr std::size_t kHeaderBytes = 44;

} // namespace

Mono16Writer::~Mono16Writer()
{
    if (open_)
    {
        std::string ignored;
        close(ignored);
    }
}

bool Mono16Writer::open(const std::string& path, int sampleRate, std::string& error)
{
    if (open_)
    {
        error = "the writer already has a file open";
        return false;
    }

    if (sampleRate <= 0)
    {
        error = "refusing to write a WAV with sample rate " + std::to_string(sampleRate);
        return false;
    }

    std::FILE* file = std::fopen(path.c_str(), "wb");

    if (file == nullptr)
    {
        error = "could not open '" + path + "' for writing";
        return false;
    }

    std::uint8_t header[kHeaderBytes];
    std::memset(header, 0, sizeof(header));

    std::memcpy(header, "RIFF", 4);
    putLe32(header + 4, kHeaderBytes - 8 + 0);   // patched at close()
    std::memcpy(header + 8, "WAVE", 4);
    std::memcpy(header + 12, "fmt ", 4);
    putLe32(header + 16, 16);
    putLe16(header + 20, 1);                              // PCM
    putLe16(header + 22, 1);                              // mono
    putLe32(header + 24, static_cast<std::uint32_t> (sampleRate));
    putLe32(header + 28, static_cast<std::uint32_t> (sampleRate) * 2u);   // byte rate
    putLe16(header + 32, 2);                              // block align
    putLe16(header + 34, 16);                             // bits
    std::memcpy(header + 36, "data", 4);
    putLe32(header + 40, 0);                              // patched at close()

    if (std::fwrite(header, 1, sizeof(header), file) != sizeof(header))
    {
        error = "could not write the WAV header to '" + path + "'";
        std::fclose(file);
        return false;
    }

    path_ = path;
    file_ = file;
    open_ = true;
    framesWritten_ = 0;
    lastError_.clear();
    return true;
}

void Mono16Writer::write(const float* samples, std::uint64_t frames)
{
    if (!open_ || frames == 0)
        return;

    if (!lastError_.empty())
        return;   // a broken stream does not pretend to recover; close() reports it

    for (std::uint64_t frame = 0; frame < frames; ++frame)
    {
        const int v = static_cast<int> (std::lround(clampToUnit(samples[frame]) * 32767.0));
        putLe16(scratchBuf_ + (scratchPos_ * 2), static_cast<std::uint16_t> (v));
        ++scratchPos_;

        if (scratchPos_ == kScratchFrames)
        {
            flushScratch();

            if (!lastError_.empty())
                return;
        }
    }
}

bool Mono16Writer::close(std::string& error)
{
    if (!open_)
        return true;

    open_ = false;   // idempotent from here on, whatever the outcome below

    flushScratch();

    std::FILE* file = static_cast<std::FILE*> (file_);
    file_ = nullptr;

    const std::uint64_t dataBytes = framesWritten_ * 2;
    bool ok = lastError_.empty();

    if (ok && std::fflush(file) != 0)
    {
        lastError_ = "flush failed on '" + path_ + "'";
        ok = false;
    }

    // Patch the two sizes (little-endian; >4 GB truncates the field like every
    // other classic WAV writer - a multi-gigabyte rehearsal file is not a thing).
    if (ok)
    {
        std::uint8_t patched[4];

        putLe32(patched, static_cast<std::uint32_t> (dataBytes));
        ok = std::fseek(file, 40, SEEK_SET) == 0 && std::fwrite(patched, 1, 4, file) == 4;

        if (ok)
        {
            putLe32(patched, static_cast<std::uint32_t> (kHeaderBytes - 8 + dataBytes));
            ok = std::fseek(file, 4, SEEK_SET) == 0 && std::fwrite(patched, 1, 4, file) == 4;
        }

        if (!ok)
            lastError_ = "could not patch the size fields in '" + path_ + "'";
    }

    if (std::fclose(file) != 0)
        ok = false;

    if (!ok || !lastError_.empty())
    {
        error = lastError_.empty() ? "the WAV file '" + path_ + "' was not finalized" : lastError_;
        return false;
    }

    error.clear();
    return true;
}

void Mono16Writer::flushScratch()
{
    if (scratchPos_ == 0)
        return;

    const std::size_t bytes = scratchPos_ * 2;
    scratchPos_ = 0;

    if (std::fwrite(scratchBuf_, 1, bytes, static_cast<std::FILE*> (file_)) != bytes)
    {
        if (lastError_.empty())
            lastError_ = "a data write failed on '" + path_ + "'";
        return;
    }

    framesWritten_ += bytes / 2;
}

} // namespace wavio
} // namespace liveai
