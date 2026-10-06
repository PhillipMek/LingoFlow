#include "Security/ChainedSecretStore.h"

#include <algorithm>
#include <optional>

namespace liveai {
namespace security {

ChainedSecretStore::ChainedSecretStore(ISecretStore& primary, ISecretStore& fallback)
    : primary_(&primary)
    , fallback_(&fallback)
    , name_(std::string(primary.name()) + " / fallback: " + std::string(fallback.name()))
{
}

std::string_view ChainedSecretStore::name() const noexcept
{
    return name_;
}

SecretStatus ChainedSecretStore::store(std::string_view identifier, std::string_view secret)
{
    // Writes belong to the primary alone: "saving the key" must produce a value
    // the application owns (secure storage), never a silent rewrite of somebody
    // else's environment.
    return primary_->store(identifier, secret);
}

std::optional<std::string> ChainedSecretStore::load(std::string_view identifier)
{
    if (auto value = primary_->load(identifier); value.has_value() && !value->empty())
        return value;

    return fallback_->load(identifier);
}

SecretStatus ChainedSecretStore::remove(std::string_view identifier)
{
    return primary_->remove(identifier);
}

std::vector<std::string> ChainedSecretStore::identifiers() const
{
    auto out = primary_->identifiers();

    for (auto&& id : fallback_->identifiers())
    {
        if (std::find(out.begin(), out.end(), id) == out.end())
            out.push_back(std::move(id));
    }

    return out;
}

ISecretStore::Location ChainedSecretStore::secretLocation(std::string_view identifier) const
{
    const std::string id(identifier);

    const auto primaryIds = primary_->identifiers();
    if (std::find(primaryIds.begin(), primaryIds.end(), id) != primaryIds.end())
        return Location::primary;

    const auto fallbackIds = fallback_->identifiers();
    if (std::find(fallbackIds.begin(), fallbackIds.end(), id) != fallbackIds.end())
        return Location::fallbackOnly;

    return Location::none;
}

} // namespace security
} // namespace liveai
