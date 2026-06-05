# Settings & Menu UX Improvements

**Date:** 2026-06-05
**Branch:** main (Waveshare ESP32-S3-Touch-LCD-4B)
**Scope:** Two independent UX changes to the settings panel and main menu

## Feature 1: Live Pet Preview in Settings

### Problem

Cycling the "pet" setting (18 ASCII species + GIF) requires exiting the
settings menu to see what each species looks like. The settings panel covers
most of the screen, hiding the buddy animation underneath.

### Design

Redesign `drawSettings()` as a compact bottom-anchored sheet. The pet
animates at full 3x scale behind it, updating immediately on each "pet" tap.

**Panel layout:**
- Bottom-anchored, full width (360px), showing 4 visible rows at a time
- Panel height: 4 rows * 36px + top padding + bottom hint = ~176px
- Panel top edge at approximately y=144 (H=320, panel ~176px tall)
- Selected row highlighted; panel scrolls when selection moves beyond the
  visible window (rows above/below the 4-row viewport scroll into view)
- "hold to close" hint remains at the bottom of the panel

**Scrolling behavior:**
- 9 settings items, 4 visible at a time
- `settingsSel` drives a scroll offset: keep the selected row within the
  visible 4-row window, adjusting the offset when selection would fall outside
- Tap a visible row to select + activate it (same as today)
- Tapping outside the panel has no effect (same as today -- long-press closes)

**Pet animation behind the panel:**
- When `settingsOpen` is true, render the buddy at full 3x scale (not peek 2x)
- `buddyTick()` already clears a 400px strip and redraws the species each
  frame. The bottom-sheet paints on top of this after the buddy renders.
- `nextPet()` already calls `characterInvalidate()` and `buddyInvalidate()`,
  so the species switch triggers an immediate redraw behind the panel with no
  new plumbing needed.
- GIF characters also animate via `characterTick()` in the same pipeline.

**Code changes required:**
- `drawSettings()`: rewrite panel geometry -- bottom-anchored, 4-row viewport,
  scroll offset derived from `settingsSel`
- `hitSettings()`: update hit-test geometry to match new panel position and
  scrolling offset
- Remove `buddySetPeek(true)` / `characterSetPeek(true)` when settings is open
  (or leave displayMode at DISP_NORMAL so peek is not engaged). The buddy
  should render at home scale (3x) behind the settings sheet.
- Audio and reset sub-panels: also bottom-anchor to match the new settings
  layout, using the same compact style.

### Edge cases

- If a GIF character is loaded and selected, `characterTick()` renders it
  behind the panel. GIF frames are larger but the same layering applies.
- Screen-off timer: touch interaction with settings resets `lastInteractMs`
  via `wake()`, so the screen won't blank while browsing settings.
- The value labels (right-aligned "3/5", "on", etc.) need to fit within the
  compact rows. Current layout uses ~70px for values at textSize 2; this
  still fits in the 360px width.

## Feature 2: Trim Main Menu

### Problem

The menu has 6 items: settings, turn off, help, about, demo, close. "Turn off"
is dangerous (accidental taps shut down the device, which has a physical power
button). "Demo" is a debug tool. "Help" is reachable through the info page tap
cycle. The menu is taller than it needs to be.

### Design

Reduce to 3 items: **settings**, **about**, **close**.

**Removed items:**
- `turn off` -- device has a physical power button; accidental menu taps
  should not power off the device
- `demo on/off` -- debug tool, not a user feature. Can still be toggled via
  serial commands or by re-adding it behind a build flag if needed for
  development.
- `help` -- touch controls info is in the info page tap cycle (page 1,
  "TOUCH"). Removing from menu reduces clutter without losing discoverability.

**Code changes required:**
- `menuItems[]`: update to `{ "settings", "about", "close" }`
- `MENU_N`: change from 6 to 3
- `menuConfirm()`: remap indices:
  - 0 (settings): open settings (unchanged)
  - 1 (about): navigate to info page credits (was index 3)
  - 2 (close): close menu (was index 5)
- `drawMenu()`: remove the demo on/off label logic (was index 4)
- `hitMenu()`: geometry updates automatically since it uses `MENU_N`
- Menu panel height shrinks from ~302px to ~170px (3 items * 44px + chrome)

### Cleanup

- `dataSetDemo()` / `dataDemo()` functions remain in data.h for potential
  serial-command use but are no longer called from the UI
- `powerOff()` remains available in power.h but is no longer menu-reachable

## Implementation Order

These two features are independent. Recommended order:

1. **Menu trim** (smaller change, immediate improvement, no layout rework)
2. **Settings bottom-sheet** (larger change, touches drawing + hit-testing +
   scroll state)

Both changes are purely in `src/main.cpp`. No other source files are affected.
