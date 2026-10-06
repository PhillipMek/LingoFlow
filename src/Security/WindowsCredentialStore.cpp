#include "Security/WindowsCredentialStore.h"

#include <string>
#include <vector>

#include "Utils/Log.h"

#ifdef _WIN32
#include <windows.h>
#include <wincred.h>
#endif

namespace liveai {
namespace security {

#ifdef _WIN32
namespace {

constexpr std::string_view kLogComponent = "security.credstore";

std::wstring toWide(std::string_view text)
{
    if (text.empty())
        return {};

    const int needed = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0);
    if (needed <= 0)
        return {};

    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), needed);
    return out;
}

std::string toUtf8(const wchar_t* text)
{
    if (text == nullptr)
        return {};

    const int needed = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1)   // -1 input means null-terminated; 1 = the terminator alone
        return {};

    std::string out(static_cast<std::size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), needed, nullptr, nullptr);
    return out;
}

/// The honest mapping from "the OS said no" to the store's vocabulary. Only the
/// error code is logged - never an identifier's value, never a length.
SecretStatus statusFromLastError(DWORD code)
{
    switch (code)
    {
        case ERROR_NOT_FOUND:
            return SecretStatus::notFound;
        case ERROR_ACCESS_DENIED:
        case ERROR_PRIVILEGE_NOT_HELD:
            return SecretStatus::denied;
        default:
            return SecretStatus::error;
    }
}

std::wstring targetFor(std::string_view identifier)
{
    return toWide(std::string(WindowsCredentialStore::kTargetPrefix) + std::string(identifier));
}

void logOsFailure(std::string_view operation, std::string_view identifier, DWORD code)
{
    const std::string message = "credential '" + std::string(identifier) + "' "
                                + std::string(operation) + " failed with OS code "
                                + std::to_string(code);
    if (operation == "store")
        log::error(kLogComponent, message);
    else
        log::warning(kLogComponent, message);
}

} // namespace

std::string_view WindowsCredentialStore::name() const noexcept
{
    return "Windows Credential Manager";
}

SecretStatus WindowsCredentialStore::store(std::string_view identifier, std::string_view secret)
{
    if (identifier.empty())
    {
        log::error(kLogComponent, "store refused: empty identifier");
        return SecretStatus::error;
    }

    const std::wstring target = targetFor(identifier);

    // A local copy of the bytes: CredWriteW reads the blob synchronously and
    // wants a mutable BYTE*, so the vector only has to live for the call.
    std::vector<BYTE> blob(secret.begin(), secret.end());

    CREDENTIALW credential {};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = const_cast<LPWSTR>(target.c_str());
    credential.CredentialBlobSize = static_cast<DWORD>(blob.size());
    credential.CredentialBlob = blob.data();
    // The credential belongs to the user's profile, not to a network realm: it
    // survives process exit and restart on this account. persistence rule
    // ("survives restart securely") lives right here - in the OS store, not in
    // our files. ENTERPRISE over LOCAL_MACHINE so a domain account keeps its
    // own credentials; on a single venue machine both simply persist.
    credential.Persist = CRED_PERSIST_ENTERPRISE;

    if (CredWriteW(&credential, 0))
    {
        // "stored" and nothing else: from this line on, only identifiers are named.
        log::info(kLogComponent,
                  "secret stored for '" + std::string(identifier) + "' (value never logged)");
        return SecretStatus::stored;
    }

    const DWORD code = GetLastError();
    logOsFailure("store", identifier, code);
    return statusFromLastError(code);
}

std::optional<std::string> WindowsCredentialStore::load(std::string_view identifier)
{
    if (identifier.empty())
        return std::nullopt;

    const std::wstring target = targetFor(identifier);

    CREDENTIALW* read = nullptr;
    if (!CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &read))
    {
        const DWORD code = GetLastError();
        if (code != ERROR_NOT_FOUND)
            logOsFailure("load", identifier, code);
        return std::nullopt;
    }

    // The blob is copied byte-exact; nothing beyond the returned string ever
    // sees these bytes, and the OS buffer is freed on the way out.
    std::optional<std::string> value;
    if (read->CredentialBlob != nullptr && read->CredentialBlobSize > 0)
        value.emplace(reinterpret_cast<const char*>(read->CredentialBlob),
                      static_cast<std::size_t>(read->CredentialBlobSize));

    CredFree(read);

    if (!value.has_value())
        log::warning(kLogComponent,
                     "credential '" + std::string(identifier) + "' exists but holds no value");

    return value;
}

SecretStatus WindowsCredentialStore::remove(std::string_view identifier)
{
    if (identifier.empty())
        return SecretStatus::notFound;

    const std::wstring target = targetFor(identifier);

    if (CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0))
    {
        log::info(kLogComponent, "credential '" + std::string(identifier) + "' removed");
        return SecretStatus::found;   // it existed, and it is gone now
    }

    const DWORD code = GetLastError();
    if (code != ERROR_NOT_FOUND)
        logOsFailure("remove", identifier, code);

    return statusFromLastError(code);
}

std::vector<std::string> WindowsCredentialStore::identifiers() const
{
    const std::wstring filter = toWide(kTargetPrefix) + L"*";

    DWORD count = 0;
    PCREDENTIALW* array = nullptr;

    if (!CredEnumerateW(filter.c_str(), 0, &count, &array))
    {
        const DWORD code = GetLastError();
        if (code != ERROR_NOT_FOUND)
            log::warning(kLogComponent, "enumeration failed with OS code " + std::to_string(code));
        return {};
    }

    std::vector<std::string> out;
    out.reserve(count);

    const std::string prefix(kTargetPrefix);

    for (DWORD i = 0; i < count; ++i)
    {
        if (array[i] != nullptr && array[i]->TargetName != nullptr)
        {
            std::string target = toUtf8(array[i]->TargetName);

            // The filter matched these names, so a prefix miss is impossible in
            // practice - the code still does not rely on that, and it never
            // looks at the blob.
            if (target.rfind(prefix, 0) == 0)
                out.push_back(target.substr(prefix.size()));
        }

        CredFree(array[i]);
    }

    LocalFree(array);
    return out;
}

#else // !_WIN32 - the OS store does not exist here; the class says so honestly

std::string_view WindowsCredentialStore::name() const noexcept
{
    return "Windows Credential Manager (not Windows: unavailable)";
}

SecretStatus WindowsCredentialStore::store(std::string_view, std::string_view)
{
    return SecretStatus::unavailable;
}

std::optional<std::string> WindowsCredentialStore::load(std::string_view)
{
    return std::nullopt;
}

SecretStatus WindowsCredentialStore::remove(std::string_view)
{
    return SecretStatus::unavailable;
}

std::vector<std::string> WindowsCredentialStore::identifiers() const
{
    return {};
}

#endif // _WIN32

} // namespace security
} // namespace liveai
