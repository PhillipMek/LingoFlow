#include "Audio/AudioTypes.h"

namespace liveai {
namespace audio {

std::string_view nameOf(SampleFormat format) noexcept
{
    switch (format)
    {
        case SampleFormat::float32:   return "float32";
        case SampleFormat::int16:     return "int16";
        case SampleFormat::int24In32: return "int24in32";
        case SampleFormat::int32:     return "int32";
        case SampleFormat::unknown:   break;
    }
    return "unknown";
}

std::string_view nameOf(BackendState state) noexcept
{
    switch (state)
    {
        case BackendState::closed:  return "closed";
        case BackendState::opened:  return "opened";
        case BackendState::running: return "running";
        case BackendState::faulted: return "faulted";
    }
    return "faulted";
}

} // namespace audio
} // namespace liveai
