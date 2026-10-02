//
// NullSecretStore - deliberately stores nothing.
//
// Reporting "stored" for a write that was not persisted would be fake success
// (AGENTS.md 19), so every operation answers "unavailable". The application must
// treat that as "translation backend cannot be started yet", not as an error that
// stops audio (AGENTS.md 12).

#include <optional>
#include <string>
#include <vector>

#include "Security/ISecretStore.h"

namespace liveai {
namespace security {

std::string_view nameOf(SecretStatus status) noexcept
{
    switch (status)
    {
        case SecretStatus::stored:      return "stored";
        case SecretStatus::found:       return "found";
        case SecretStatus::notFound:    return "not-found";
        case SecretStatus::unavailable: return "unavailable";
        case SecretStatus::denied:      return "denied";
        case SecretStatus::error:       return "error";
    }
    return "error";
}

bool ISecretStore::contains(std::string_view identifier)
{
    const auto value = load(identifier);
    return value.has_value() && !value->empty();
}

SecretStatus NullSecretStore::store(std::string_view identifier, std::string_view secret)
{
    (void)identifier;
    (void)secret;
    return SecretStatus::unavailable;
}

std::optional<std::string> NullSecretStore::load(std::string_view identifier)
{
    (void)identifier;
    return std::nullopt;
}

SecretStatus NullSecretStore::remove(std::string_view identifier)
{
    (void)identifier;
    // This store holds nothing by definition, so the identifier is certainly not
    // present: that is notFound, not a failure. Writing, in contrast, is genuinely
    // impossible and answers unavailable.
    return SecretStatus::notFound;
}

std::vector<std::string> NullSecretStore::identifiers() const
{
    return {};
}

} // namespace security
} // namespace liveai
