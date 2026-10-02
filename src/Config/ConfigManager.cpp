#include "Config/ConfigManager.h"

#include <utility>

namespace liveai {

ConfigManager::ConfigManager() = default;

ConfigManager::ConfigManager(config::ConfigStore store)
    : store_(std::move(store))
{
}

void ConfigManager::setStore(config::ConfigStore store)
{
    store_ = std::move(store);
}

bool ConfigManager::load(std::string& note)
{
    if (!store_.has_value())
    {
        note = "no configuration store is configured";
        return false;
    }

    lastLoad_ = store_->load();
    current_ = lastLoad_.config;

    note = std::string(config::nameOf(lastLoad_.outcome));
    if (!lastLoad_.message.empty())
        note += ": " + lastLoad_.message;

    // Even a repaired configuration is applied: the operator must not lose sound
    // because one field in a text file is wrong.
    notify();
    return true;
}

bool ConfigManager::save(std::string& error)
{
    if (!store_.has_value())
    {
        error = "no configuration store is configured";
        return false;
    }

    return store_->save(current_, error);
}

bool ConfigManager::update(AppConfig candidate, std::string& error)
{
    if (const auto problems = config::validate(candidate); !problems.empty())
    {
        error.clear();
        for (const auto& problem : problems)
        {
            if (!error.empty())
                error += "; ";
            error += problem.field + ": " + problem.message;
        }
        return false;
    }

    error.clear();
    current_ = std::move(candidate);
    notify();
    return true;
}

bool ConfigManager::updateWith(std::string& error, const std::function<void(AppConfig&)>& mutate)
{
    AppConfig candidate = current_;
    mutate(candidate);
    return update(std::move(candidate), error);
}

bool ConfigManager::updateAndSave(AppConfig candidate, std::string& error)
{
    if (!update(std::move(candidate), error))
        return false;

    return save(error);
}

void ConfigManager::addListener(IConfigListener& listener)
{
    listeners_.push_back(&listener);
}

void ConfigManager::notify()
{
    for (auto* listener : listeners_)
    {
        if (listener != nullptr)
            listener->onConfigChanged(current_);
    }
}

void ConfigManager::resetForTests()
{
    current_ = config::defaults();
    lastLoad_ = config::LoadResult{};
    listeners_.clear();
    store_.reset();
}

} // namespace liveai
