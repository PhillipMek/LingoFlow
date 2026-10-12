#pragma once
//
// OperatorWindow - the JUCE shell of the operator screen, laid out for the UI redesign's operator
// screen. It owns WIDGETS and nothing else: every value it paints comes from
// buildOperatorPanel() (App/UiModel), and every action it takes is exactly one
// call into ApplicationController. No device is opened, no audio computed, no
// socket touched from this file - the FAIL criterion "UI owns backend/audio
// logic" is kept out by construction.
//
// The visual contract (it, on top of the earlier information architecture):
//   header -> [scroll: SYSTEM STATUS card -> AUDIO card (paired INPUT/OUTPUT
//   columns) -> TRANSLATION card -> HEALTH card] -> note -> command bar
//   (START/STOP). Colour is state only; the spacing scale is
//   4/8/12/16/24/32; no monospace and no engineering explanations on this
//   screen - the applied-glide detail and the arithmetic behind the latency
//   line live in the Diagnostics window.
//
// Venue task 4 §1 compacted the column (status and health became 2x2 grids,
// captions moved inline with their controls, the translation rows merged) and
// pinned the header and the command bar outside the scroll - the buttons the
// show runs on are never below the fold; the cards that inform are the ones
// that may scroll.
//
// Threading: everything runs on the message thread. The timer (100 ms) is a
// repaint clock over atomic snapshots - a slow paint stalls paint only, never
// audio. The updatingWidgets_ guard is why a programmatic refresh cannot echo
// back as an operator action.
//
// Option lists (devices, languages, channels) live only as caches mirrored from
// the panel - the window holds no list of its own, so there is nothing that
// could drift out of sync with the registry or the schema.
//
// Slider wiring (this JUCE version exposes onValueChange/onDragStart/onDragEnd):
// only the gains remain sliders on this screen - dragging fires the LIVE call
// only (no disk); the commit that validates and persists happens once at drag
// end - or immediately for a typed text value, which arrives while no drag is
// active. Channels are discrete combo choices; they commit directly on change.

#include <chrono>
#include <functional>
#include <string>
#include <vector>

#include <JuceHeader.h>

#include "App/ApplicationController.h"
#include "App/UiModel.h"
#include "App/UiWidgets.h"
#include "App/WindowGeometry.h"
#include "Audio/LevelMeter.h"

namespace liveai {

class SettingsWindow;      // the Settings dialog, owned while hidden and shown
class DiagnosticsWindow;   // the UI redesign engineering surface, same ownership rule

/// A horizontal peak/RMS meter with a clipping lamp. Pure paint: it receives a
/// UiMeterView and draws it; it owns no audio state. The numeric readout lives
/// beside it (a Label the content window fills from the same view), never
/// inside the bar.
class MeterBar final : public juce::Component
{
public:
    void setLevels(const UiMeterView& view);
    void paint(juce::Graphics& g) override;

private:
    static float positionForDb(float db) noexcept;   ///< -60..0 dBFS across the bar

    UiMeterView view_;
};

/// The operator screen itself: the UI redesign information architecture wearing the
/// the UI redesign visual hierarchy. Raw counters, the full latency accounting and the
/// technical gain-glide detail are the Diagnostics window's business; this
/// screen answers "ready? routed? levels? translating? act?" in one glance.
///
/// Venue task 4 §1 gave the screen its vertical shape: the header (identity +
/// Settings) and the command bar (note + START/STOP) are PINNED to the window,
/// the four information cards scroll between them inside a ui::ScrollablePage,
/// and the cards themselves got their compact forms (status and health as
/// 2x2 grids, captions inline with their controls, the merged translation
/// row). The command buttons used to sit below the fold of a laptop screen -
/// on a stage that is the one control that must always be reachable.
class OperatorContent final : public juce::Component, private juce::Timer
{
public:
    explicit OperatorContent(ApplicationController& controller);
    ~OperatorContent() override;   ///< defined where SettingsWindow is complete

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    /// The scrollable card column. It positions the owner's card widgets for
    /// the width the viewport offers and returns the height the cards need
    /// (the ui::ScrollablePage measure contract - measurement and placement
    /// are one pass, so they cannot disagree), and it paints the card panels
    /// under the rects that same pass computed.
    class CardsSurface final : public ui::ScrollablePage::Body
    {
    public:
        explicit CardsSurface(OperatorContent& owner) : owner_(owner) {}

        int layout(int width);
        void paintContent(juce::Graphics& g, const juce::Rectangle<int>& area) override;

    private:
        OperatorContent& owner_;   ///< outlives the surface: it owns the page

        // Card rectangles, computed by layout() and consumed by paintContent().
        juce::Rectangle<int> statusCard_, audioCard_, translationCard_, healthCard_;
    };
    // ------------------------------------------------------------ refresh
    void timerCallback() override;
    void rebuild();

    /// Replaces a ComboBox's items only when the list actually changed, so the
    /// 100 ms clock never disturbs an open dropdown mid-click. Selects
    /// selectedIndex (or clears the selection when it is -1).
    void syncOptions(juce::ComboBox& box, std::vector<UiOption>& cache,
                     const std::vector<UiOption>& fresh, int selectedIndex);

    // ------------------------------------------------------------- commands
    void startPressed();
    void stopPressed();
    void settingsPressed();
    void diagnosticsPressed();
    void refreshDevicesPressed();
    void deviceSelected();
    void targetSelected();
    void channelChanged();
    void gainMoved();
    void gainCommit();
    void muteToggled();

    /// Copy current settings, mutate, and push through controller.updateSettings;
    /// whatever it says becomes the visible action note. This is the ONLY way the
    /// window changes configuration - one funnel, no half-applied edits.
    void commitSettings(std::function<void(AppConfig&)> mutate);

    static juce::Colour stateColour(std::string_view state) noexcept;
    static juce::String stateGlyph(std::string_view state) noexcept;

    ApplicationController& controller_;
    std::string actionNote_;

    // ------------------------------------------------------- the scrollable middle
    /// The card column inside its ui::ScrollablePage (venue task 4 §1): the
    /// header above and the command bar below are pinned, the cards scroll.
    std::unique_ptr<ui::ScrollablePage> cardsPage_;
    CardsSurface* cards_ = nullptr;   ///< owned by the page's viewport

    // ---------------------------------------------------------------- header
    juce::Label titleLabel_;
    juce::Label devBadge_;         ///< compact "DEVELOPER MODE" chip, hidden in production
    juce::TextButton settingsButton_ { "Settings" };

    // ---------------------------------------------------------- system status
    juce::Label statusHeader_;
    juce::Label appCaption_, audioCaption_, sessionCaption_, ndiCaption_;
    juce::Label appValue_, audioValue_, sessionValue_, ndiValue_;
    juce::Label detailLabel_;          ///< the actionable warning line (amber/red), empty when healthy
    juce::Label credentialLabel_;
    juce::Label devDetailLabel_;       ///< the plan's full truth, small, only when developer mode is on

    // ------------------------------------------------------------------ audio
    juce::Label audioHeader_;
    juce::Label deviceCaption_, deviceNoteLabel_;
    juce::ComboBox deviceChoice_;
    juce::TextButton refreshDevicesButton_ { "Refresh" };
    /// Discrete by nature (the UI redesign Р’В§4): channel is an index, so it gets a list,
    /// not a fader - and a visibly different control from the gain slider.
    juce::Label inputChannelCaption_, outputChannelCaption_, channelNoteLabel_;
    juce::ComboBox inputChannelChoice_, outputChannelChoice_;

    juce::Label inputMeterCaption_, outputMeterCaption_;
    MeterBar inputMeter_, outputMeter_;

    /// Venue task 3 §3: the bar and the numeric readout show attack/release
    /// smoothed levels, while the measurement itself stays the engine's raw
    /// fact (LevelMeter, realtime side; the export keeps printing raw numbers).
    /// The smoother state belongs to THIS view - one owner, the message thread,
    /// no locks; the clock decides each step's coefficient, so the motion does
    /// not depend on how often rebuild() happens to run; and the meter's
    /// blocksMeasured() progress tells "no new data" from "quiet": when the
    /// data stops, the display glides to silence instead of freezing.
    audio::MeterSmoother inputMeterSmoothing_;
    audio::MeterSmoother outputMeterSmoothing_;
    std::uint64_t lastInputMeterBlocks_ = 0;
    std::uint64_t lastOutputMeterBlocks_ = 0;
    std::chrono::steady_clock::time_point lastMeterUpdate_ {};
    juce::Label inputLevelLabel_, outputLevelLabel_;   ///< numeric level + peak, from the same view
    juce::Label inputGainCaption_, outputGainCaption_;
    juce::Slider inputGainSlider_, outputGainSlider_;
    juce::TextButton inputMuteButton_ { "MUTE INPUT" };
    juce::TextButton outputMuteButton_ { "MUTE OUTPUT" };

    // ------------------------------------------------------------- translation
    juce::Label translationHeader_;
    juce::Label sourceCaption_, targetCaption_, pairWarningLabel_;
    juce::Label sourceValueLabel_;       ///< static "Automatic detection" - the wire's truth (R8)
    juce::ComboBox targetChoice_;
    juce::Label sessionLineLabel_;         ///< "Connected" beside the pair - the same state word
    juce::Label currentSubtitleLabel_;     ///< the live line, quoted
    juce::Label historyLabel_;             ///< compact tail of recent lines

    // -------------------------------------------------------------- live health
    juce::Label healthHeader_;
    juce::Label latencyCaption_, latencyValue_;
    juce::Label jitterCaption_, jitterValue_;
    juce::Label underrunCaption_, underrunValue_;
    juce::Label reconnectCaption_, reconnectValue_;
    juce::TextButton diagnosticsButton_ { "Diagnostics" };

    juce::Label noteLabel_;                ///< the operator's own last result

    // ------------------------------------------------------------- command bar
    juce::TextButton startButton_ { "Start" };
    juce::TextButton stopButton_ { "Stop" };

    // Option caches: the window's only lists, mirrored from the panel.
    std::vector<UiOption> deviceCache_, targetCache_;
    std::vector<UiOption> inputChannelCache_, outputChannelCache_;

    /// The Settings dialog: created on first open, hidden on close, always
    /// re-read before it is shown again - the settings funnel stays singular.
    std::unique_ptr<SettingsWindow> settingsWindow_;

    /// The UI redesign's diagnostics surface, same lifetime rule.
    std::unique_ptr<DiagnosticsWindow> diagnosticsWindow_;

    bool updatingWidgets_ = false;
    bool gainDragging_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(OperatorContent)
};

/// The DocumentWindow wrapper: nothing but a frame, the title bar and the quit
/// route - all substance lives in OperatorContent. Its one own decision is
/// venue task 5: the frame REMEMBERS its normal-state rectangle (resized/moved
/// on the message thread, memory only, never disk) so the application can
/// persist it on the way out and come back to the same size and place.
class OperatorWindow final : public juce::DocumentWindow
{
public:
    explicit OperatorWindow(ApplicationController& controller);

    void closeButtonPressed() override;

    /// The rectangle to persist at shutdown: the last NON-maximised geometry
    /// of this run - closing while maximised therefore returns the window at
    /// the size the operator had before the maximize. The maximise state
    /// itself is not persisted (and not stored in the settings file): JUCE
    /// 9.0.3 exposes no public query for it, and the one OS question this
    /// class asks - "is the frame zoomed right now" - exists to decide
    /// whether the current rectangle may be recorded at all, nothing else.
    WindowSettings geometryToPersist() const noexcept { return geometry_; }

protected:
    /// Layout first (the superclass owns the decorations), then the geometry
    /// record - the DocumentWindow docs are explicit that overriding these
    /// means calling the superclass version.
    void resized() override;
    void moved() override;

private:
    void recordNormalGeometry();

    /// Seeded with the default size; the constructor's placement fires
    /// resized(), so the first real value is recorded before the window is
    /// ever visible.
    WindowSettings geometry_ { 0, 0, kMainWindowDefaultWidth, kMainWindowDefaultHeight };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(OperatorWindow)
};

} // namespace liveai
