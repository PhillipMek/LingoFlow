#pragma once

// OpenAI-Safety-Identifier: the recommended optional header on the Realtime
// connection request (docs/openai-realtime-protocol.md section 3; added by
// code review P2 on 2026-10-05). The provider asks for "a stable,
// privacy-preserving value, such as a hashed internal user ID" and warns
// against sending identities in the clear. A single-operator live-show
// product has no per-end-user ID, so the stable subject is the installation:
// SHA-256 of the Windows installation GUID. The wire carries neither the
// GUID, nor a username, nor a hostname.
//
// Windows by intent (this whole target is WinHTTP-only), and nothing here is
// realtime-safe: the composition root calls it once when the production
// translation chain is installed.

#include <string>
#include <string_view>

namespace liveai {
namespace network {

/// Lowercase hex SHA-256 of the given bytes, computed by the OS crypto
/// provider (advapi32 CryptoAPI - a library this link already carries; no
/// hand-rolled digest, AGENTS.md 15). Returns an empty string on provider
/// failure; callers must treat that as "no value", never as "the hash of
/// nothing" (the empty input has its own well-known digest and is hashed
/// faithfully when explicitly requested).
std::string sha256Hex(std::string_view bytes);

/// Whether a value may go into the OpenAI-Safety-Identifier header as-is.
/// The gate is ours and strict by design (RFC 7230 field-vchar without SP
/// or HT): non-empty, one printable token, at most 128 characters - a
/// malformed option value must never be able to inject a second header line
/// into the upgrade request. The product's own digest (64 hex chars) always
/// passes. The provider publishes no ceiling for this optional header, so
/// the length bound is product hygiene, not a documented protocol limit.
bool isSendableSafetyIdentifier(const std::string& value);

/// The product's safety identifier: sha256Hex of the Windows installation
/// GUID (HKLM\SOFTWARE\Microsoft\Cryptography\MachineGuid - the documented
/// per-installation value, generated at OS setup). Empty when the registry
/// value is unreadable or not a REG_SZ; the caller then omits the optional
/// header entirely (the provider does not require it).
std::string deriveInstallationIdentifier();

} // namespace network
} // namespace liveai
