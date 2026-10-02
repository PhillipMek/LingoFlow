#include "Translation/ITranslationBackend.h"

namespace liveai {
namespace translation {

std::string_view nameOf(SessionState state) noexcept
{
    switch (state)
    {
        case SessionState::closed:      return "closed";
        case SessionState::connecting:  return "connecting";
        case SessionState::connected:   return "connected";
        case SessionState::reconnecting:return "reconnecting";
        case SessionState::faulted:     return "faulted";
    }
    return "faulted";
}

} // namespace translation
} // namespace liveai
