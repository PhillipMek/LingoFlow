#include "Subtitles/SubtitleStyle.h"

namespace liveai {
namespace subtitles {

std::string_view nameOf(OutputMode mode) noexcept
{
    switch (mode)
    {
        case OutputMode::video:    return "video";
        case OutputMode::metadata: return "metadata";
        case OutputMode::both:     return "both";
    }
    return "video";   // unreachable: the enum is total, the compiler needs a word
}

std::string_view nameOf(HorizontalAlign align) noexcept
{
    switch (align)
    {
        case HorizontalAlign::left:   return "left";
        case HorizontalAlign::center: return "center";
        case HorizontalAlign::right:  return "right";
    }
    return "center";
}

std::string_view nameOf(VerticalAnchor anchor) noexcept
{
    switch (anchor)
    {
        case VerticalAnchor::bottom: return "bottom";
        case VerticalAnchor::top:    return "top";
    }
    return "bottom";
}

std::optional<OutputMode> outputModeFromName(std::string_view name) noexcept
{
    if (name == "video")    return OutputMode::video;
    if (name == "metadata") return OutputMode::metadata;
    if (name == "both")     return OutputMode::both;
    return std::nullopt;
}

std::optional<HorizontalAlign> horizontalAlignFromName(std::string_view name) noexcept
{
    if (name == "left")   return HorizontalAlign::left;
    if (name == "center") return HorizontalAlign::center;
    if (name == "right")  return HorizontalAlign::right;
    return std::nullopt;
}

std::optional<VerticalAnchor> verticalAnchorFromName(std::string_view name) noexcept
{
    if (name == "bottom") return VerticalAnchor::bottom;
    if (name == "top")    return VerticalAnchor::top;
    return std::nullopt;
}

} // namespace subtitles
} // namespace liveai
