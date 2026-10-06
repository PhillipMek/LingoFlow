#pragma once
//
// ChainedSecretStore - one primary store with one read-only fallback.
//
// The product's credential order of truth:
//   * production: the operator's key lives in Windows secure storage;
//   * development: an environment variable may provide a key.
// A chain expresses exactly that: writes always go to the primary (the fallback
// is external state the application must not rewrite - an environment variable
// is the developer's, not the product's), and reads try the primary first, so
// a key stored from the UI wins over any environment value, and the environment
// keeps working as the documented development path until one is stored.
//
// The value semantics mirror ISecretStore: identifiers only in identifiers(),
// no value in any message, an empty string counts as "no credential" (a session
// would refuse on it anyway - better to fall through than to hand a lie to the
// caller).

#include <string>
#include <string_view>
#include <vector>

#include "Security/ISecretStore.h"

namespace liveai {
namespace security {

class ChainedSecretStore final : public ISecretStore
{
public:
    /// Both stores must outlive the chain. Neither may be null.
    ChainedSecretStore(ISecretStore& primary, ISecretStore& fallback);

    /// "primary / fallback: secondary" - built once in the constructor so
    /// name()'s string_view always refers to this object's own storage.
    std::string_view name() const noexcept override;

    /// Always the primary. The fallback is never written through this store.
    SecretStatus store(std::string_view identifier, std::string_view secret) override;

    /// Primary value when present and non-empty; otherwise the fallback's.
    std::optional<std::string> load(std::string_view identifier) override;

    /// Removes from the primary only, and reports exactly that (the fallback is
    /// not the application's to delete - the note text the UI shows says so).
    SecretStatus remove(std::string_view identifier) override;

    /// Union of both stores' identifiers, primary first, deduplicated.
    /// Names only, never values.
    std::vector<std::string> identifiers() const override;

    /// The question the chain exists to make answerable (the UI redesign §5): a key the
    /// primary holds is secure storage; one that only the environment provides
    /// is a development convenience the status line must say so about.
    Location secretLocation(std::string_view identifier) const override;

    /// Writing goes to the primary, so the primary is what a status line
    /// should name.
    std::string_view writableStoreName() const noexcept override { return primary_->name(); }

    ISecretStore& primary() const noexcept { return *primary_; }
    ISecretStore& fallback() const noexcept { return *fallback_; }

private:
    ISecretStore* primary_;
    ISecretStore* fallback_;
    std::string name_;
};

} // namespace security
} // namespace liveai
