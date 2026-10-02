#pragma once
//
// ISecretStore - where credentials live instead of config.json.
//
// This is the boundary required by AGENTS.md 10 and SPEC "Secrets separate":
// configuration (ConfigManager) and credentials (this interface) are separate
// concerns, and the application must compile, run and test without ever holding
// an API key in a settings struct or in a log line.
//
// The Windows implementation (task 015) uses secure credential storage. During
// development an environment-variable store is allowed. No implementation here
// may write a secret to disk in plain text or return it through AppConfig.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace liveai {
namespace security {

/// Well-known secret identifiers. The value of a secret is never a settings field.
inline constexpr std::string_view kOpenAiApiKey = "openai_api_key";

/// Result of a store operation; messages must never contain the secret itself.
enum class SecretStatus
{
    stored = 0,      ///< written successfully
    found,           ///< a value exists
    notFound,        ///< nothing stored under this identifier
    unavailable,     ///< no secure storage backend is present
    denied,          ///< storage refused (permissions, policy)
    error            ///< unspecified failure
};

std::string_view nameOf(SecretStatus status) noexcept;

class ISecretStore
{
public:
    virtual ~ISecretStore() = default;

    /// Human-readable backend label, e.g. "Windows Credential Manager".
    virtual std::string_view name() const noexcept = 0;

    /// Stores a secret. Implementations must not log the value.
    virtual SecretStatus store(std::string_view identifier, std::string_view secret) = 0;

    /// Returns the secret when present; nullopt otherwise.
    virtual std::optional<std::string> load(std::string_view identifier) = 0;

    /// Removes a secret. A null store knows it holds nothing, so it answers notFound;
    /// a real backend that cannot be reached answers unavailable or error.
    virtual SecretStatus remove(std::string_view identifier) = 0;

    /// Identifiers only - never values.
    virtual std::vector<std::string> identifiers() const = 0;

    /// True when a value exists, without copying it out.
    bool contains(std::string_view identifier);
};

/// A store that holds nothing: keeps the application runnable when secure storage
/// is unavailable and makes "no credentials" an explicit, testable state.
class NullSecretStore final : public ISecretStore
{
public:
    std::string_view name() const noexcept override { return "Null"; }

    SecretStatus store(std::string_view identifier, std::string_view secret) override;
    std::optional<std::string> load(std::string_view identifier) override;
    SecretStatus remove(std::string_view identifier) override;
    std::vector<std::string> identifiers() const override;
};

} // namespace security
} // namespace liveai
