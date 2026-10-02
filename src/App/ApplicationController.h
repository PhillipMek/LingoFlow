#pragma once
//
// ApplicationController: the object the UI talks to and that owns the product
// subsystems. At bootstrap it only manages its own lifecycle state and logging.
// Audio/translation/NDI are attached in later tasks through the interfaces in
// docs/architecture.md (IAudioBackend, ITranslationBackend, INdiOutput).
//
// No JUCE, no audio, no network here.

#include <string>
#include <string_view>

namespace liveai {

enum class ApplicationState
{
    stopped,     ///< nothing is running
    starting,    ///< subsystems are being brought up
    running,     ///< normal operation
    stopping,    ///< subsystems are being shut down
    faulted      ///< an unrecoverable condition was observed
};

std::string_view nameOf(ApplicationState state);

class ApplicationController
{
public:
    ApplicationController() = default;

    /// Brings the application up. Returns false when already running or faulted.
    bool start();

    /// Stops the application. Always succeeds from a non-faulted state.
    void stop();

    ApplicationState state() const noexcept { return state_; }
    bool isRunning() const noexcept { return state_ == ApplicationState::running; }

    /// Reason recorded when entering the faulted state (empty otherwise).
    std::string_view faultReason() const noexcept { return faultReason_; }

private:
    ApplicationState state_ = ApplicationState::stopped;
    std::string faultReason_;
};

} // namespace liveai
