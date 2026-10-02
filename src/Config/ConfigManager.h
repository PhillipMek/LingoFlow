#pragma once
//
// ConfigManager - single Settings area (SPEC "Configuration").
//
// This task only establishes the boundary: who may read settings, how a change is
// announced, and where secrets are NOT allowed to live. Atomic persistence,
// schema/versioning and corruption recovery are task 003; the API key is task 015
// and lives in the credential store, never in AppConfig.

#include <cstdint>
#include <string>
#include <vector>
#include <vector>

namespace liveai {

/// Plain data settings. Implementation details of audio/network backends are not
/// stored here (AGENTS.md 7: Config contains no backend internals).
struct AppConfig
{
    // Audio
    int sampleRate = 48000;
    int bufferFrames = 480;
    int inputChannel = 1;
    int outputChannel = 1;
    float inputGainDb = 0.0f;
    float outputGainDb = 0.0f;

    // Translation
    std::string inputLanguage = "en";
    std::string outputLanguage = "ru";
    std::string interpreterInstructions;   // SPEC "Translation instructions"

    // NDI
    bool ndiEnabled = false;
    std::string ndiStreamName = "LiveAI Interpreter";

    // Latency/buffer tuning
    int jitterBufferMs = 120;

    std::uint32_t schemaVersion = 1;
};

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
    ConfigManager();

    const AppConfig& current() const noexcept { return current_; }

    /// Validates then applies; returns false and leaves state untouched when the
    /// value is invalid. Notifies listeners after a successful apply.
    bool update(AppConfig candidate, std::string& error);

    /// Registers a listener; ownership stays with the caller.
    void addListener(IConfigListener& listener);

    /// Test helper.
    void resetForTests() noexcept;

    /// Validates a candidate configuration. Task 003 extends this with the full
    /// schema; boundary values are already enforced here.
    static bool validate(const AppConfig& candidate, std::string& error);

private:
    AppConfig current_;
    std::vector<IConfigListener*> listeners_;
};

} // namespace liveai
