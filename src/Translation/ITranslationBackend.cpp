#include "Translation/ITranslationBackend.h"

namespace liveai {
namespace translation {

std::string_view nameOf(SessionState state) noexcept
{
    switch (state)
    {
        case SessionState::closed:       return "closed";
        case SessionState::connecting:   return "connecting";
        case SessionState::connected:    return "connected";
        case SessionState::reconnecting: return "reconnecting";
        case SessionState::faulted:      return "faulted";
    }
    return "unknown";
}

std::string_view nameOf(TranslationErrorCategory category) noexcept
{
    switch (category)
    {
        case TranslationErrorCategory::connection:        return "connection";
        case TranslationErrorCategory::rejectedRequest:   return "rejected-request";
        case TranslationErrorCategory::audioFormat:       return "audio-format";
        case TranslationErrorCategory::protocol:          return "protocol";
        case TranslationErrorCategory::rateLimited:       return "rate-limited";
        case TranslationErrorCategory::serviceOverloaded: return "service-overloaded";
        case TranslationErrorCategory::authentication:    return "authentication";
        case TranslationErrorCategory::internal:          return "internal";
    }
    return "unknown";
}

} // namespace translation
} // namespace liveai
