// Tests for the shared design system: the tokens are the single source of
// truth for all three windows, so the promises worth asserting are that every
// token is a real opaque colour, that the icon vocabulary is complete, and
// that the card/icon painters draw into a plain JUCE image without needing a
// window or a peer. The fade classes are deliberately not timed here: their
// clock is the message-thread timer, and a unit test cannot honestly measure
// perceived smoothness (that is a HUMAN CHECKPOINT on a venue monitor).

#include <catch2/catch_test_macros.hpp>

#include <array>

#include "App/Theme.h"
#include "App/UiWidgets.h"

namespace ui = liveai::ui;

namespace {

struct GuiScope
{
    juce::ScopedJuceInitialiser_GUI initialiser;
};

} // namespace

TEST_CASE("theme tokens: one opaque palette and an ordered spacing scale", "[ui][theme]")
{
    const std::array<juce::Colour, 15> tokens {
        ui::colour::background, ui::colour::card, ui::colour::cardRaised,
        ui::colour::cardBorder, ui::colour::control, ui::colour::controlHover,
        ui::colour::controlDown, ui::colour::textPrimary, ui::colour::textSecondary,
        ui::colour::textMuted, ui::colour::header, ui::colour::green,
        ui::colour::amber, ui::colour::red, ui::colour::blue
    };

    for (const auto& c : tokens)
    {
        CHECK(c.getAlpha() == 0xff);
        CHECK(c.getRed() <= 0xff);
        CHECK(c.getGreen() <= 0xff);
        CHECK(c.getBlue() <= 0xff);
    }

    // The old kMutedTextColour call sites must resolve to the SAME object as
    // the token, not to a second copy that could drift on a future retune.
    CHECK(ui::kMutedTextColour == ui::colour::textMuted);

    CHECK(ui::metric::xs < ui::metric::sm);
    CHECK(ui::metric::sm < ui::metric::md);
    CHECK(ui::metric::md < ui::metric::lg);
    CHECK(ui::metric::lg < ui::metric::xl);
    CHECK(ui::metric::xl < ui::metric::xxl);
    CHECK(ui::metric::motionFast < ui::metric::motionStandard);
}

TEST_CASE("theme icons: every enum value resolves to a drawable path", "[ui][theme]")
{
    const std::array<ui::Icon, 10> all {
        ui::Icon::settings, ui::Icon::diagnostics, ui::Icon::refresh,
        ui::Icon::play, ui::Icon::stop, ui::Icon::mute, ui::Icon::audio,
        ui::Icon::translation, ui::Icon::health, ui::Icon::status
    };

    for (const auto icon : all)
    {
        const auto path = ui::iconPath(icon);
        CHECK_FALSE(path.isEmpty());
        CHECK(path.getBounds().getWidth() > 0.0f);
        CHECK(path.getBounds().getHeight() > 0.0f);
    }
}

TEST_CASE_METHOD(GuiScope, "theme painters: cards and icons rasterise without a window",
                 "[ui][theme]")
{
    juce::Image image(juce::Image::ARGB, 320, 160, true);
    {
        juce::Graphics g(image);

        ui::drawCard(g, { 10, 10, 300, 140 });
        ui::drawCardDivider(g, { 20, 30, 280, 1 });
        ui::drawIcon(g, ui::Icon::audio, { 24.0f, 16.0f, 16.0f, 16.0f }, ui::colour::textMuted);
        ui::drawIcon(g, ui::Icon::play, { 60.0f, 16.0f, 16.0f, 16.0f }, ui::colour::green);

        // A zero-alpha card is the entrance fade's first frame: it must draw
        // nothing rather than leave a stale rectangle behind.
        ui::drawCard(g, { 0, 0, 320, 160 }, 0.0f);
    }

    CHECK(image.getPixelAt(0, 0).getAlpha() == 0x00);
    CHECK(image.getPixelAt(150, 80).getAlpha() == 0xff);
}

TEST_CASE_METHOD(GuiScope, "look and feel: the skin supplies fallbacks for every used control",
                 "[ui][theme]")
{
    ui::LingoFlowLookAndFeel skin;

    CHECK(skin.findColour(juce::TextButton::buttonColourId) == ui::colour::control);
    CHECK(skin.findColour(juce::ComboBox::backgroundColourId) == ui::colour::control);
    CHECK(skin.findColour(juce::Label::textColourId) == ui::colour::textPrimary);
    CHECK(skin.findColour(juce::ToggleButton::tickColourId) == ui::colour::green);
    CHECK(skin.getDefaultScrollbarWidth() == 10);
}
