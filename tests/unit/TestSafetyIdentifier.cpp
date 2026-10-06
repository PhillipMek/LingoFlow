// Tests for Network/SafetyIdentifier.h.
//
// Three layers, each proven separately: the OS digest against the published
// FIPS 180-4 test vectors (we do not test our hex loop, we test that the
// provider digest IS the standard one); the sendability gate as injection
// hygiene; and the product wiring - the derived value must equal the digest
// of the live registry GUID, which is read again here by an independent path
// so the test proves the chain, not just its pieces.

#include <catch2/catch_test_macros.hpp>

#include <windows.h>

#include <string>

#include "Network/SafetyIdentifier.h"

using liveai::network::deriveInstallationIdentifier;
using liveai::network::isSendableSafetyIdentifier;
using liveai::network::sha256Hex;

TEST_CASE("SafetyIdentifier: the OS digest is SHA-256 per FIPS 180-4", "[network][safety]")
{
    // Published example digests, not values this file invented.
    CHECK(sha256Hex("")
          == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(sha256Hex("abc")
          == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    const std::string digest = sha256Hex("LingoFlow installation identity input");
    REQUIRE(digest.size() == 64);
    CHECK(digest == sha256Hex("LingoFlow installation identity input")); // deterministic
    for (const char c : digest)
        CHECK(((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))); // lowercase hex only
}

TEST_CASE("SafetyIdentifier: the sendability gate rejects anything but one token",
          "[network][safety]")
{
    CHECK(isSendableSafetyIdentifier(std::string(64, 'a')));   // the product shape
    CHECK_FALSE(isSendableSafetyIdentifier(""));               // absent is not a value
    CHECK_FALSE(isSendableSafetyIdentifier("has space"));      // SP would split the header
    CHECK_FALSE(isSendableSafetyIdentifier("line\r\nInjected: yes")); // response splitting
    CHECK_FALSE(isSendableSafetyIdentifier("tab\there"));
    CHECK_FALSE(isSendableSafetyIdentifier(std::string(129, 'a')));   // our documented cap
    CHECK(isSendableSafetyIdentifier(std::string(128, 'a')));
}

TEST_CASE("SafetyIdentifier: the installation digest is stable, hashed, and wired",
          "[network][safety]")
{
    const std::string first = deriveInstallationIdentifier();
    const std::string second = deriveInstallationIdentifier();

    // A machine that does not expose the registry value legitimately produces
    // the empty answer (the caller then omits the optional header); every
    // Windows client this product targets carries it.
    if (first.empty())
    {
        CHECK(second.empty());
        return;
    }

    CHECK(first.size() == 64);
    CHECK(first == second);                                   // stable per installation
    CHECK(isSendableSafetyIdentifier(first));
    CHECK(first != sha256Hex("")); // "no identity" is absent, never a digest of nothing

    // The wiring: read the documented value by an independent path (the test
    // does not call readMachineGuid()) and require the product answer to be
    // its digest. The MachineGuid is a lowercase GUID - ASCII by shape.
    wchar_t raw[128] = {};
    DWORD size = sizeof(raw);
    DWORD type = 0;
    REQUIRE(RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Cryptography",
                         L"MachineGuid", RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY,
                         &type, raw, &size) == ERROR_SUCCESS);
    REQUIRE(type == REG_SZ);
    std::string guid;
    for (DWORD i = 0; i + 1 < size / static_cast<DWORD>(sizeof(wchar_t)); ++i)
        guid.push_back(static_cast<char>(raw[i]));
    REQUIRE_FALSE(guid.empty());

    CHECK(first == sha256Hex(guid));
    CHECK(first.find(guid) == std::string::npos); // the GUID itself never rides the wire
}
