#include "NDI/INdiOutput.h"

namespace liveai {
namespace ndi {

std::string_view nameOf(OutputState state) noexcept
{
    switch (state)
    {
        case OutputState::disabled:  return "disabled";
        case OutputState::ready:     return "ready";
        case OutputState::publishing:return "publishing";
        case OutputState::faulted:   return "faulted";
    }
    return "disabled";
}

} // namespace ndi
} // namespace liveai
