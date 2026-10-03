#include "NDI/Null/NullNdiOutput.h"

namespace liveai {
namespace ndi {

bool NullNdiOutput::start(std::string_view streamName, std::string& error)
{
    error.clear();

    if (streamName.empty())
    {
        error = "NDI output name must not be empty";
        state_ = OutputState::faulted;
        return false;
    }

    streamName_.assign(streamName);
    state_ = OutputState::ready;
    return true;
}

void NullNdiOutput::stop() noexcept
{
    state_ = OutputState::disabled;
    streamName_.clear();
}

bool NullNdiOutput::publish(const SubtitleFrame& frame, std::string& error)
{
    error.clear();

    if (state_ != OutputState::ready && state_ != OutputState::publishing)
    {
        error = "NDI output is not started";
        return false;
    }

    if (frame.text.empty())
        return false;   // nothing to show; not an error

    published_.fetch_add(1, std::memory_order_relaxed);
    state_ = OutputState::publishing;
    return true;
}

} // namespace ndi
} // namespace liveai
