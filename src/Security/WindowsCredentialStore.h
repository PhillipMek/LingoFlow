#pragma once
//
// WindowsCredentialStore - the production credential store (task 015).
//
// AGENTS.md 10: "production - store via Windows secure storage". That is the
// Windows Credential Manager: per-user generic credentials, encrypted at rest
// by the operating system, surviving process exit and restart, and inspectable
// by the operator in the Control Panel (a fact they can verify with their own
// eyes, which is worth more than a promise in a README).
//
// Namespacing: every target name is "LingoFlow/<identifier>", so this store's
// entries are recognisable, enumerable, and cannot collide with another
// program's credentials. The secret is the credential blob, byte-exact (an
// OpenAI-style key is printable ASCII, but byte-exactness is the honest
// contract, not an assumption about the alphabet).
//
// The rules that make this the security boundary it claims to be:
//   * no value is ever returned through identifiers();
//   * no value and no value length is ever logged - only operation outcomes
//     and OS error codes;
//   * nothing here writes a credential into an application file: the settings
//     area (ConfigManager) cannot see this class at all.
//
// Non-Windows builds keep the class compiling (the core stays portable) but
// every operation says "unavailable" - that is a fact about the platform, not
// a fake success (AGENTS.md 19).

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Security/ISecretStore.h"

namespace liveai {
namespace security {

class WindowsCredentialStore final : public ISecretStore
{
public:
    /// Target-name namespace prefix. Public because the self-test of the
    /// boundary (and only that) needs to reason about what it writes.
    static constexpr std::string_view kTargetPrefix = "LingoFlow/";

    std::string_view name() const noexcept override;

    SecretStatus store(std::string_view identifier, std::string_view secret) override;

    /// The exact bytes that were stored. Nothing is trimmed, cased or truncated.
    std::optional<std::string> load(std::string_view identifier) override;

    /// found = the credential existed and is gone; notFound = there was nothing
    /// to remove; denied/error = the OS refused and the log says which.
    SecretStatus remove(std::string_view identifier) override;

    std::vector<std::string> identifiers() const override;
};

} // namespace security
} // namespace liveai
