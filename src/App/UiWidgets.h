#pragma once
//
// UiWidgets - the small shared vocabulary of the three LingoFlow windows
// (UI-01 information architecture). Fonts, section captions and the one
// metric-list renderer the Diagnostics surface uses. Widget helpers only:
// nothing here knows the application, the engine or the config - every value
// that reaches these helpers was built by App/UiModel, which is where the
// tested formatting lives.
//
// The two older windows keep their private copies of the font helpers for now
// (a style pass will fold them in later); new surfaces use these from the
// first pixel so the pattern has a home.

#include <JuceHeader.h>

#include <string>
#include <utility>
#include <vector>

namespace liveai {
namespace ui {

inline juce::Font uiFont(float height = 15.0f)
{
    return juce::Font(juce::FontOptions().withHeight(height));
}

inline juce::Font monoFont(float height = 13.0f)
{
    return juce::Font(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(),
                                       height, juce::Font::plain));
}

/// A section or field caption in the house grey. Registers with the parent.
inline juce::Label& caption(juce::Label& label, const juce::String& text,
                            juce::Component& parent, float height = 12.0f)
{
    label.setFont(uiFont(height));
    label.setColour(juce::Label::textColourId, juce::Colour(0xff8b949eu));
    label.setText(text, juce::NotificationType::dontSendNotification);
    parent.addAndMakeVisible(label);
    return label;
}

/// A section header: brighter and larger than a field caption - the hierarchy
/// marker UI-01 introduced (SYSTEM STATUS / AUDIO / TRANSLATION / LIVE HEALTH).
inline juce::Label& sectionHeader(juce::Label& label, const juce::String& text,
                                  juce::Component& parent)
{
    label.setFont(uiFont(13.0f));
    label.setColour(juce::Label::textColourId, juce::Colour(0xffc9d1d9u));
    label.setText(text, juce::NotificationType::dontSendNotification);
    parent.addAndMakeVisible(label);
    return label;
}

/// "label ......... value" rows, monospace, oldest format the operator windows
/// already used - so a row that moved from the main screen to Diagnostics
/// reads identically to the operator's screenshots from before the move.
inline std::string formatMetricRows(
    const std::vector<std::pair<std::string, std::string>>& rows, int labelWidth = 36)
{
    std::string out;
    for (const auto& [name, value] : rows)
    {
        out += std::format("{:<{}}{:>12}\n", name, labelWidth, value);
        if (name.size() > static_cast<std::size_t>(labelWidth))
            out += "\n";   // a long name wraps under itself, never over the column
    }
    return out;
}

} // namespace ui
} // namespace liveai
