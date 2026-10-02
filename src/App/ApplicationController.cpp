#include "App/ApplicationController.h"

#include "Utils/Log.h"

namespace liveai {

std::string_view nameOf(ApplicationState state)
{
    switch (state)
    {
        case ApplicationState::stopped:  return "stopped";
        case ApplicationState::starting: return "starting";
        case ApplicationState::running:  return "running";
        case ApplicationState::stopping: return "stopping";
        case ApplicationState::faulted:  return "faulted";
    }
    return "faulted";
}

bool ApplicationController::start()
{
    if (state_ == ApplicationState::running || state_ == ApplicationState::starting)
    {
        log::warning("app", "start() ignored: already " + std::string(nameOf(state_)));
        return false;
    }

    if (state_ == ApplicationState::faulted)
    {
        log::error("app", "start() refused from faulted state: " + faultReason_);
        return false;
    }

    // Subsystems (audio engine, translation backend, NDI) are attached here in
    // later tasks; bootstrap only proves the lifecycle and logging path.
    state_ = ApplicationState::starting;
    log::info("app", "starting");

    state_ = ApplicationState::running;
    log::info("app", "running");
    return true;
}

void ApplicationController::stop()
{
    if (state_ == ApplicationState::stopped)
        return;

    state_ = ApplicationState::stopping;
    log::info("app", "stopping");

    state_ = ApplicationState::stopped;
    faultReason_.clear();
    log::info("app", "stopped");
}

} // namespace liveai
