#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>

#include "Config/ConfigSchema.h"
#include "Security/ISecretStore.h"

using namespace liveai;
using liveai::security::ISecretStore;
using liveai::security::NullSecretStore;
using liveai::security::SecretStatus;

TEST_CASE("SecretStatus names are stable", "[security]")
{
    CHECK(liveai::security::nameOf(SecretStatus::stored) == "stored");
    CHECK(liveai::security::nameOf(SecretStatus::found) == "found");
    CHECK(liveai::security::nameOf(SecretStatus::notFound) == "not-found");
    CHECK(liveai::security::nameOf(SecretStatus::unavailable) == "unavailable");
    CHECK(liveai::security::nameOf(SecretStatus::denied) == "denied");
    CHECK(liveai::security::nameOf(SecretStatus::error) == "error");
}

TEST_CASE("NullSecretStore reports the truth: it stores nothing", "[security]")
{
    NullSecretStore store;

    CHECK(store.name() == "Null");

    // Reporting "stored" here would be fake success: nothing is persisted.
    CHECK(store.store(security::kOpenAiApiKey, "sk-whatever") == SecretStatus::unavailable);
    CHECK_FALSE(store.load(security::kOpenAiApiKey).has_value());
    CHECK_FALSE(store.contains(security::kOpenAiApiKey));
    CHECK(store.identifiers().empty());

    // A null store holds nothing by definition, so removal is "not found"; writing
    // is what is genuinely impossible.
    CHECK(store.remove(security::kOpenAiApiKey) == SecretStatus::notFound);
}

TEST_CASE("NullSecretStore through the interface does not leak a value", "[security]")
{
    auto owned = std::make_unique<NullSecretStore>();
    ISecretStore& store = *owned;

    store.store(security::kOpenAiApiKey, "sk-must-not-be-kept");
    const auto value = store.load(security::kOpenAiApiKey);

    CHECK_FALSE(value.has_value());
    CHECK(store.identifiers().empty());
}

TEST_CASE("The serialized configuration can never express a credential", "[security][config]")
{
    const auto json = config::toJsonText(config::defaults());

    // No field name in config.json may look like a credential, and the well-known
    // identifier of the OpenAI key must not appear at all.
    CHECK(json.find(std::string(security::kOpenAiApiKey)) == std::string::npos);
    CHECK(config::isSecretFieldName(security::kOpenAiApiKey));

    for (const std::string_view marker : { "api_key", "apikey", "token", "secret", "password", "credential" })
        CHECK(json.find(marker) == std::string::npos);
}
