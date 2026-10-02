#include "Network/Base64.h"

namespace liveai {
namespace network {

namespace {

constexpr char kAlphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/// Reverse table: alphabet index per byte, 255 = invalid, 254 = '='.
struct ReverseTable
{
    unsigned char map[256];

    constexpr ReverseTable() : map{}
    {
        for (int i = 0; i < 256; ++i)
            map[i] = 255;
        for (int i = 0; i < 64; ++i)
            map[static_cast<unsigned char>(kAlphabet[i])] = static_cast<unsigned char>(i);
        map[static_cast<unsigned char>('=')] = 254;
    }
};

constexpr ReverseTable kReverse {};

} // namespace

std::string base64Encode(const std::uint8_t* data, std::size_t len)
{
    std::string out;
    out.reserve(((len + 2) / 3) * 4);

    std::size_t i = 0;
    for (; i + 3 <= len; i += 3)
    {
        const unsigned v = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out.push_back(kAlphabet[(v >> 18) & 0x3F]);
        out.push_back(kAlphabet[(v >> 12) & 0x3F]);
        out.push_back(kAlphabet[(v >> 6) & 0x3F]);
        out.push_back(kAlphabet[v & 0x3F]);
    }

    if (i + 1 == len)
    {
        const unsigned v = data[i] << 16;
        out.push_back(kAlphabet[(v >> 18) & 0x3F]);
        out.push_back(kAlphabet[(v >> 12) & 0x3F]);
        out.push_back('=');
        out.push_back('=');
    }
    else if (i + 2 == len)
    {
        const unsigned v = (data[i] << 16) | (data[i + 1] << 8);
        out.push_back(kAlphabet[(v >> 18) & 0x3F]);
        out.push_back(kAlphabet[(v >> 12) & 0x3F]);
        out.push_back(kAlphabet[(v >> 6) & 0x3F]);
        out.push_back('=');
    }

    return out;
}

bool base64Decode(std::string_view text, std::vector<std::uint8_t>& out)
{
    // Strip ASCII whitespace; the protocol emits unbroken base64, but tolerance
    // here costs nothing and cannot corrupt data.
    std::string compact;
    compact.reserve(text.size());
    for (const char c : text)
    {
        if (c == '\r' || c == '\n' || c == ' ' || c == '\t')
            continue;
        compact.push_back(c);
    }

    if (compact.size() % 4 != 0)
        return false;

    out.clear();
    out.reserve((compact.size() / 4) * 3);

    for (std::size_t i = 0; i < compact.size(); i += 4)
    {
        const unsigned a = kReverse.map[static_cast<unsigned char>(compact[i])];
        const unsigned b = kReverse.map[static_cast<unsigned char>(compact[i + 1])];
        const unsigned c = kReverse.map[static_cast<unsigned char>(compact[i + 2])];
        const unsigned d = kReverse.map[static_cast<unsigned char>(compact[i + 3])];

        if (a > 63 || b > 63)
            return false; // '=' or invalid in the first two slots is never legal

        const bool cPad = (c == 254);
        const bool dPad = (d == 254);

        if (cPad && !dPad)
            return false; // "=x" is not a legal tail
        if (c > 64 && !cPad)
            return false;
        if (d > 64 && !dPad)
            return false;
        if (i + 4 != compact.size() && (cPad || dPad))
            return false; // padding only in the final quantum

        // Non-zero leftover bits are not canonical base64: 1-byte quantum keeps
        // b's low 4 bits empty, 2-byte quantum keeps c's low 2 bits empty.
        if (cPad && dPad && (b & 0x0F) != 0)
            return false;
        if (dPad && !cPad && (c & 0x03) != 0)
            return false;

        const unsigned cc = cPad ? 0 : c;
        const unsigned dd = dPad ? 0 : d;
        const unsigned v = (a << 18) | (b << 12) | (cc << 6) | dd;

        out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFF));
        if (!cPad)
            out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
        if (!dPad)
            out.push_back(static_cast<std::uint8_t>(v & 0xFF));
    }

    return true;
}

} // namespace network
} // namespace liveai
