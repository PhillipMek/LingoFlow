#pragma once
//
// ConfigManager - the single Settings area in memory (spec "Configuration").
//
// Owns the current AppConfig, refuses invalid updates, notifies listeners and
// delegates persistence to ConfigStore. Secrets are not part of AppConfig; they
// belong to the credential store interface (src/Security), so nothing here can
// write a credential into config.json.

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "Config/AppConfig.h"
#include "Config/ConfigStore.h"

namespace liveai {

/// Implemented by anything that must react to a settings change. Callbacks run on
/// the thread that called update(); they must not block on realtime work.
class IConfigListener
{
public:
    virtual ~IConfigListener() = default;
    virtual void onConfigChanged(const AppConfig& updated) = 0;
};

class ConfigManager
{
public:
    /// Starts from the operator defaults. Use load() to read the persisted file.
    ConfigManager();

    /// With a store, load()/save() know where the file lives.
    explicit ConfigManager(config::ConfigStore store);

    /// Points the manager at a config file without reading it yet.
    void setStore(config::ConfigStore store);
    const config::ConfigStore* store() const noexcept { return store_ ? &*store_ : nullptr; }

    const AppConfig& current() const noexcept { return current_; }
    const config::LoadResult& lastLoad() const noexcept { return lastLoad_; }
    bool hasStore() const noexcept { return store_.has_value(); }

    /// Reads the file. Always leaves a usable configuration: on damage the store
    /// quarantines the file and returns defaults or the backup, and the outcome is
    /// reported through lastLoad(). Returns false only when nothing could be read.
    bool load(std::string& note);

    /// Writes the current configuration atomically. Returns false with a message
    /// when validation or the filesystem fails; the previous file stays intact.
    bool save(std::string& error);

    /// Validates then applies. An invalid candidate is refused in full: no part of
    /// it is applied, so the UI cannot end up with a half-written state.
    bool update(AppConfig candidate, std::string& error);

    /// update() + save() in one step, for settings dialogs.
    bool updateAndSave(AppConfig candidate, std::string& error);

    /// Applies values that came from another component (e.g. a device selected in
    /// the UI) and notifies listeners. Validation is performed on the merged result.
    bool updateWith(std::string& error, const std::function<void(AppConfig&)>& mutate);

    void addListener(IConfigListener& listener);

    /// Test helper: defaults, no listeners, no store.
    void resetForTests();

private:
    void notify();

    std::optional<config::ConfigStore> store_;
    AppConfig current_;
    config::LoadResult lastLoad_;
    std::vector<IConfigListener*> listeners_;
};

} // namespace liveai
