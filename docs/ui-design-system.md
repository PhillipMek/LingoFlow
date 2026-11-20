# LingoFlow UI design system

This document is the presentation-layer contract for the three operator-facing
surfaces (Operator, Settings, Diagnostics). It records what `src/App/Theme.h`
and `src/App/UiWidgets.h` implement. It contains no product behaviour: every
value shown on screen still comes from `App/UiModel` through
`ApplicationController`.

## 1. Why a design system here

The three windows used to carry three private palettes (`ink::`, `sink::`,
`dink::`) and three copies of the font/control helpers. The information
architecture was already consistent; the *vocabulary* was not. `Theme.h` is now
the single source of truth for colour, spacing, radii, typography, icons and
decorative motion. A retune happens in one file, not in three paint methods.

No external UI framework was introduced. The project already vendors JUCE
9.0.3; `LingoFlowLookAndFeel` derives from `LookAndFeel_V4`, and the only newly
linked module is `juce_animation` (already in the vendored tree). This keeps the
AGPL/licensing chain, the manual `ScrollablePage` layout contract and the
tested `UiModel` boundary exactly as they were.

## 2. Tokens

| Group | Tokens | Rule |
|---|---|---|
| Surfaces | `background`, `card`, `cardRaised`, `cardBorder`, `control`, `controlHover`, `controlDown` | Neutral only. `cardRaised` is one step lighter and used for active tabs and tooltips. |
| Text | `textPrimary`, `textSecondary`, `textMuted`, `header` | Four levels of hierarchy; no other greys. |
| State | `green`, `amber`, `red`, `blue` | Colour means state and nothing else - dots, state words and the buttons that mean them. Healthy rows stay neutral. |
| Focus | `focus` | A 1.6 px ring on keyboard focus for buttons, combos and text editors. |
| Spacing | `xs/sm/md/lg/xl/xxl` = `4/8/12/16/24/32` | Component heights are fixed constants, not per-widget negotiations. |
| Radii | `radiusSm/Md/Lg` = `4/7/10` px | Cards use `Md`, controls `Sm`. |
| Motion | `motionFast` 120 ms, `motionStandard` 180 ms | Decorative only; see §5. |

`ui::kMutedTextColour` remains as an alias of `colour::textMuted` for older
call sites. It is the same object, not a second copy of the value.

## 3. Look and feel

`LingoFlowLookAndFeel` is installed once in `Main.cpp` for the process lifetime
and cleared before it is destroyed. It overrides only the widgets the product
uses: buttons (rounded, hover/pressed/disabled, focus ring), tick boxes,
combos, linear sliders, tab buttons (with a 16 px gutter so six Settings tabs
do not run together), scrollbars, tooltips and text editors. Popup menus are
themed through the skin's colour map. Because per-widget colours still come
from the tokens, the skin adds *treatment* without changing any value.

## 4. Shared primitives

* `ui::drawCard(g, bounds, alpha)` - one rounded surface, one-pixel border and
  a quiet top highlight instead of a shadow. The `alpha` parameter is the
  entrance fade and never affects data.
* `ui::drawCardDivider` - a hairline under a card title.
* `ui::Icon` / `ui::drawIcon` - original SVG path data parsed once into a
  `juce::Path` and rasterised by JUCE. Vector glyphs stay sharp at any Windows
  DPI scaling; no bitmap assets and no icon-font dependency.
* `ui::ColourFade` - cross-fades a single colour and writes each frame into an
  owner-provided callback. The four operator state words use it, so a
  green→amber→red change eases instead of snapping.
* `ui::EntranceFade` - a single 180 ms reveal of the operator card column on
  first layout.

## 5. Motion and threading

Both fades are driven by one `juce::AnimatorUpdater` behind a message-thread
timer (`Motion::updater()`, 60 Hz). Animators hold only weak references in the
updater; a fades' owner owns its animator and unregisters it on destruction,
with a shared `alive` flag guarding late callbacks. All of this lives on the
message thread. **No animation code is reachable from the audio callback** and
no animation touches audio, network or credential state.

## 6. Accessibility and operator UX

* `setExplicitFocusOrder` gives the operator screen a deterministic Tab path
  through Settings → device → channels → gains → target → diagnostics →
  START/STOP. Settings and Diagnostics order their own actions the same way.
* One application-wide `TooltipWindow` (via `SharedResourcePointer`) explains
  short or consequential labels. Credential-store, NDI-test and export buttons
  state their real effect in plain language.
* Tooltips and focus rings inherit the token palette, so they are themed too.

## 7. What remains a HUMAN CHECKPOINT

Unit tests cover tokens, icon paths, painters and the skin's colour map. They
cannot honestly verify perceived polish or a live venue workflow. On a real
rig a human must still check:

* 1280x720 and DPI-scaled displays, including maximised frame/taskbar overlap;
* the card reveal and state fades at 100 ms data refresh - decorative motion
  must not read as latency;
* START/STOP reachability and keyboard-only operation during a live show;
* that no audio, OpenAI or NDI behaviour regressed (this change is presentation
  only).
