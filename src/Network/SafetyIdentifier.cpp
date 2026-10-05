#include "Network/SafetyIdentifier.h"

#include <windows.h>
#include <wincrypt.h>

#include <cstddef>

namespace liveai {
namespace network {

namespace {

constexpr DWORD kDigestBytes = 32; // SHA-256, by definition

std::string guidToUtf8(const wchar_t* wide, DWORD byteCount)
{
    // The MachineGuid is a lowercase GUID - pure ASCII by its documented
    // shape - so the narrowing conversion cannot mangle it; byteCount from
    // RegGetValueW includes the terminating NUL of the REG_SZ.
    const DWORD chars = byteCount / static_cast<DWORD>(sizeof(wchar_t));
    std::string out;
    out.reserve(chars);
    for (DWORD i = 0; i + 1 < chars; ++i)
        out.push_back(static_cast<char>(wide[i]));
    return out;
}

std::string readMachineGuid()
{
    wchar_t buffer[128] = {};
    DWORD size = sizeof(buffer);
    DWORD type = 0;
    // RRF_SUBKEY_WOW6464KEY names the 64-bit registry view explicitly: the
    // answer must not depend on how a future toolset happens to build this
    // binary.
    if (::RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Cryptography",
                       L"MachineGuid", RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY,
                       &type, buffer, &size) != ERROR_SUCCESS
        || type != REG_SZ
        || size < 2 * static_cast<DWORD>(sizeof(wchar_t)))
        return {};

    buffer[(size / static_cast<DWORD>(sizeof(wchar_t))) - 1] = L'\0'; // defensive NUL
    return guidToUtf8(buffer, size);
}

} // namespace

std::string sha256Hex(std::string_view bytes)
{
    HCRYPTPROV provider = 0;
    if (!::CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT))
        return {};

    std::string out;
    HCRYPTHASH hash = 0;
    bool ok = ::CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash) != FALSE;
    if (ok && !bytes.empty())
        ok = ::CryptHashData(hash, reinterpret_cast<const BYTE*>(bytes.data()),
                             static_cast<DWORD>(bytes.size()), 0) != FALSE;
    unsigned char digest[kDigestBytes] = {};
    DWORD digestSize = sizeof(digest);
    if (ok)
    {
        ok = ::CryptGetHashParam(hash, HP_HASHVAL, digest, &digestSize, 0) != FALSE
             && digestSize == kDigestBytes;
    }

    if (hash != 0)
        ::CryptDestroyHash(hash);
    ::CryptReleaseContext(provider, 0);

    if (!ok)
        return {};

    static constexpr char kHex[] = "0123456789abcdef";
    out.assign(2 * kDigestBytes, '0');
    for (DWORD i = 0; i < digestSize; ++i)
    {
        out[2 * static_cast<std::size_t>(i)] = kHex[digest[i] >> 4];
        out[2 * static_cast<std::size_t>(i) + 1] = kHex[digest[i] & 0x0F];
    }
    return out;
}

bool isSendableSafetyIdentifier(const std::string& value)
{
    if (value.empty() || value.size() > 128)
        return false;
    for (const char c : value)
    {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x21 || u > 0x7E) // no control bytes (CRLF injection), no space
            return false;
    }
    return true;
}

std::string deriveInstallationIdentifier()
{
    const std::string guid = readMachineGuid();
    if (guid.empty())
        return {}; // an absent identity is "no header", never a digest of nothing
    return sha256Hex(guid);
}

} // namespace network
} // namespace liveai
