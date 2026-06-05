# Claude Desktop Buddy 4B: 480×480 Native Layout + Battery Support

**Date:** 2026-06-05
**Branch:** `waveshare-s3-touch-lcd-4b`
**Repo:** https://github.com/greipfrut/claude-desktop-buddy

## Summary

Two bundled enhancements for the Waveshare ESP32-S3-Touch-LCD-4B buddy:

1. **Battery support** — replace the hardcoded battery/power stubs with real
   reads from the on-board **AXP2101 PMIC** (voltage, percent, charging state,
   USB presence), a real PMIC power-off, and real values in the BLE status JSON.
2. **480×480 native layout** — the UI is currently a 240×320 canvas centered on
   the 480×480 panel with large black borders (an MVP shortcut from the 2.8"
   port). Redesign for the full square: a "Big Buddy" home screen, a roomy
   approval screen with a scrollable hint (fixing today's 19-char truncation),
   battery/charging detail surfaced, and the buddy scaled up to fill the space.

The two are bundled because the charging detail and battery indicator are part
of the new layout, but they split cleanly into phases: the battery **data layer**
is layout-independent and ships first; the **UI redesign** follows.

## Background / current state

- **Battery/power is stubbed** (`src/main.cpp`): `batteryMilliVolts()` returns
  4200, `batteryPercent()` 100, `batteryUsbPresent()` true, `batteryMilliAmps()`
  0, `batteryUsbMilliVolts()` 5000; `powerOff()` just calls `ESP.restart()`.
  These already feed the on-screen battery glyph and the BLE `bat{}` status field
  — so making them real lights both up with no call-site changes.
- The 4B has an **AXP2101 PMIC** (`#define XPOWERS_CHIP_AXP2101` in the demo pin
  config). Reference: demo `05_LVGL_AXP2101_ADC_Data`. The demo configures only
  charging target, an IRQ, and ADC enables — it does **not** touch the DCDC/LDO
  power rails, so reading the PMIC is safe (no brown-out risk to the display/ESP).
  `lewisxhe/XPowersLib` is the driver; not yet a dependency.
- **Canvas geometry** (`src/display.h`): `PANEL_WIDTH/HEIGHT=480`, but
  `LCD_WIDTH=240`/`LCD_HEIGHT=320` with `CANVAS_OFF_X/Y` centering the UI (120,80).
  `src/display.cpp` constructs `canvas` at that size/offset; `src/touch.cpp`
  subtracts the offset and rejects touches outside the 240×320 region.
- **Buddy geometry** (`src/buddy_common.h` externs, defined in `src/buddy.cpp`):
  `BUDDY_X_CENTER`, `BUDDY_CANVAS_W`, `BUDDY_Y_BASE`, `BUDDY_CHAR_W/H` — all sized
  for the 240×320 canvas.
- **Gestures** (`src/main.cpp` `pollGesture()`): only tap (`G_TAP`) and
  long-press (`G_LONG`). No drag/scroll.
- **Approval screen** (`drawApproval()`): shows the tool name + a hint
  **truncated to 19 characters** + green OK / red NO. This is the readability
  pain the redesign fixes.

## Scope

**In scope:**
- Real AXP2101 battery reads + power-off + BLE status (Phase 1).
- 480×480 canvas + 1:1 touch (Phase 2).
- Redesigned screens: home (Big Buddy), approval (scrollable hint), info
  (+ battery detail), menu/settings/audio/reset submenus, clock, pet (Phase 2).
- Buddy scaling for ASCII (18 species) and GIF (Phase 2).
- A new vertical **drag gesture** for scrolling the approval hint.

**Explicitly out of scope (separate future projects, each needs host-side work):**
- **Multiple-choice prompts** (title/description per option + Other). Requires
  extending the desktop bridge AND the BLE protocol — not firmware-only.
- **Claude usage display** (session/weekly utilization + reset times, à la
  Clawdmeter). Requires a host-side daemon that reads the Claude OAuth token and
  the Anthropic API `anthropic-ratelimit-*` response headers, then pushes over
  BLE. The 480×480 layout should *leave room* for a future "usage" Info page, but
  we do not build it here.
- **Physical buttons** (the 2 buttons; one is the AXP2101 PWRKEY). Separate
  feature; enabling the PWRKEY IRQ is left for then.
- **Low-battery auto-handling** (warning/auto-sleep at a threshold) — not requested.

## Architecture

### Canvas geometry (Phase 2)

Change the UI from a centered 240×320 strip to a full-panel 480×480 surface:

- `src/display.h`: `LCD_WIDTH 480`, `LCD_HEIGHT 480`. `CANVAS_OFF_X/Y` then
  compute to `0` (formula `(PANEL - LCD)/2` still holds), so dependent code is
  unchanged.
- `src/display.cpp`: construct `canvas = new Arduino_Canvas(480, 480, gfx, 0, 0)`.
- Framebuffer grows 240×320×2 = 150 KB → 480×480×2 = 460 KB, allocated in PSRAM
  (8 MB available — trivial).
- `src/touch.cpp`: with `CANVAS_OFF_X/Y == 0` the existing translation becomes
  identity and the bounds check becomes `0…479` — touch maps 1:1, no code change
  needed beyond the constants (verify the reject-out-of-bounds still behaves).
- The `#define spr (*canvas)` indirection and `W = LCD_WIDTH` / `H = LCD_HEIGHT`
  in `main.cpp` are unchanged in form; `W`/`H` simply become 480, and every
  screen's layout math reflows against the new values (each screen is re-laid-out
  in Phase 2 — see Screen designs).

### Battery data layer (Phase 1) — new `src/power.h` / `src/power.cpp`

Mirrors the existing `display.*` / `touch.*` / `audio.*` module pattern. Owns the
single `XPowersPMU` instance and exposes the power API the rest of the firmware
already calls.

**Public API** (`power.h`):

```cpp
bool powerInit();              // PMIC begin on shared Wire; enable ADCs; no rail changes
void powerPoll();              // refresh cached readings (call ~every 3-5 s from loop)
int  batteryMilliVolts();      // cached
int  batteryPercent();         // cached (PMIC gauge, voltage-curve fallback)
int  batteryMilliAmps();       // cached charge/discharge current (signed; 0 if unknown)
bool batteryUsbPresent();      // cached VBUS state
int  batteryUsbMilliVolts();   // cached VBUS voltage (kept for API parity)
bool batteryCharging();        // cached: actively charging
bool batteryFull();            // cached: charge done
void powerOff();               // real AXP2101 shutdown
```

- **Init:** `pmu.begin(Wire, AXP2101_SLAVE_ADDRESS, 47, 48)` (Wire already begun
  in `displayInit()`); enable battery/VBUS/system voltage ADC measurement and
  battery detection; **do not configure DCDC/LDO rails** (the board is already
  powered through them). Returns false on failure; readers then return safe
  defaults and `beep`-style graceful degradation applies.
- **Caching:** `powerPoll()` reads the PMIC over I²C and stores values in module
  statics; the getters return the cache. Called on a timer in `loop()` (~3-5 s) so
  no I²C happens in the render hot path (same discipline as `xferRefreshFsUsed()`).
- **Percent:** `pmu.getBatteryPercent()` when `pmu.isBatteryConnect()` returns a
  valid (non-negative) value; otherwise map battery voltage through a small LiPo
  curve (≈3.30 V → 0 %, 4.20 V → 100 %, with a few interior points).
- **Charging state:** `pmu.isCharging()` / charger-status enum →
  `batteryCharging()` / `batteryFull()`.
- **Power-off:** `pmu.shutdown()` (device powers down, wakes on PWRKEY/USB),
  replacing `ESP.restart()`.
- **main.cpp:** delete the stub functions; `#include "power.h"`; call
  `powerInit()` in `setup()` (after `displayInit()` so Wire/expander are up) and
  `powerPoll()` on a timer in `loop()`. All existing call sites (battery glyph,
  BLE status, auto-screen-off-on-battery logic, `powerOff()` menu action) keep
  working against the same names.
- **BLE status:** the `bat{pct,mV,mA,usb}` field in the status JSON (`xfer.h`)
  already reads these getters — it begins reporting real values automatically.
  `mA` uses the PMIC charge/discharge current when available, else 0.

### Gestures (Phase 2) — drag/scroll

Extend `pollGesture()` with a drag notion for the approval hint:
- While a touch is held and moves more than a small threshold vertically before
  release, treat it as a **drag** (report a y-delta) rather than a tap.
- Scoped use: only the approval screen's hint region consumes drag deltas to
  scroll its text; elsewhere behavior is unchanged (a drag that isn't consumed
  falls back to no-op, not a stray tap).
- Implementation stays inside `pollGesture()` + the approval hit-test; no new
  module.

## Screen designs (Phase 2)

All screens draw against `W = H = 480` and share one language: ~40 px edge bars,
generous margins, larger text than the 240×320 originals.

- **Home (Big Buddy)** — `DISP_NORMAL`:
  - Top bar (~40 px): clock (left) · pet name (center) · battery (right) as
    **glyph + percent + ⚡ bolt when charging**.
  - Center: the buddy (ASCII or GIF), scaled to fill the region between the bars.
  - Bottom bar (~40 px): a dim line (sessions running/waiting · level) + the live
    status message line.
- **Approval** — `drawApproval()`:
  - Timer (`approve? Ns`, turns hot past 10 s) · full tool name (large) ·
    **wrapped, scrollable hint** showing the *entire* hint (no 19-char cut) with a
    scrollbar indicator; vertical drag/swipe on the hint area scrolls it · large
    ✓ OK (green) / ✕ NO (red) buttons pinned to the bottom.
  - Hit-testing: OK/NO occupy the bottom button strip as today; the hint area
    above is the scroll target.
- **Info** — `DISP_INFO` pages: larger text, more rows per page, **plus a battery
  detail section** (state word, voltage, current, percent). This page group is the
  natural home for a future "usage" view (out of scope now).
- **Menu / Settings / Audio / Reset submenus:** same list logic, taller rows, more
  rows visible at once; hit-test math follows `W`/`H`.
- **Clock** — large centered time filling the square.
- **Pet pages** — `DISP_PET`: scaled to the larger canvas.

## Buddy scaling (Phase 2)

- **ASCII (18 species):** update the geometry constants defined in `buddy.cpp`
  (`BUDDY_X_CENTER` 120→240, `BUDDY_CANVAS_W`, `BUDDY_Y_BASE`, `BUDDY_CHAR_W/H`)
  so species render larger and centered in the new canvas. Species files call the
  shared helpers (`buddyPrintLine`/`buddyPrintSprite`), so most need no change;
  the few with direct `canvas->` draws (e.g. penguin, octopus) get coordinate
  review.
- **GIF (`character.cpp`):** integer / nearest-neighbor upscale to fill the center
  region, preserving pixel-art crispness (avoid smooth scaling). If a GIF is
  already large enough, center it.

## File change map

### New files
| File | Purpose |
|---|---|
| `src/power.h` | Battery/power API (`powerInit`, `powerPoll`, getters, `powerOff`) |
| `src/power.cpp` | AXP2101 (XPowersLib) ownership, cached reads, real power-off |

### Modified files
| File | Changes |
|---|---|
| `platformio.ini` | Add `lewisxhe/XPowersLib` to `lib_deps`; flip the temporary `ARDUINO_USB_CDC_ON_BOOT=0` back to `1` (diagnostic flag from the audio work) |
| `src/main.cpp` | Remove battery/power stubs; `#include "power.h"`; `powerInit()` in setup + `powerPoll()` timer in loop; redesign every screen for 480×480; add drag gesture; battery detail on info |
| `src/display.h` | `LCD_WIDTH/HEIGHT` → 480 (offset constants then resolve to 0) |
| `src/display.cpp` | Construct `canvas` at 480×480 offset (0,0) |
| `src/touch.cpp` | Verify 1:1 mapping with zero offset (likely no change beyond constants) |
| `src/buddy.cpp` | Enlarge buddy geometry constants for the 480×480 center |
| `src/buddies/*.cpp` | Coordinate review for the few species with direct draws |
| `src/character.cpp` | Nearest-neighbor GIF upscale to fill the center |

### Unchanged
`ble_bridge.*`, `data.h` (wire protocol — battery fields already present),
`stats.h`, `xfer.h`, `clock.h`, `audio.*`, `es8311.*`, GIF assets.

## Implementation order

### Phase 1 — Battery data layer (layout-independent, ship first)
1. Add `XPowersLib`; write `power.h`/`power.cpp` (init, poll, getters, power-off).
2. Replace the stubs in `main.cpp`; wire `powerInit()`/`powerPoll()`.
3. Build, flash, verify on device: real percent/voltage that *changes* on
   USB-unplug vs charging; charging flag flips when USB attached; menu power-off
   actually shuts down; BLE status `bat{}` shows real numbers.

### Phase 2 — 480×480 native UI
4. Canvas geometry (`display.h/.cpp`) + touch 1:1 (`touch.cpp`); confirm the
   existing UI renders (mis-placed but alive) and touch lands correctly.
5. Buddy scaling (`buddy.cpp` geometry + `character.cpp` GIF upscale).
6. Redesign screens one at a time, verifying each on device: home (Big Buddy) →
   approval (+ drag-scroll hint) → info (+ battery detail) → menu/settings/submenus
   → clock → pet.
7. Flip `ARDUINO_USB_CDC_ON_BOOT` back to 1; final build, flash, full pass.

## Risks / open items

- **AXP2101 percent accuracy:** the PMIC fuel gauge may be uncalibrated; the
  voltage-curve fallback covers it but is approximate. Acceptable for a desk pet.
- **PMIC I²C contention:** `powerPoll()` shares Wire with touch (GT911) and the
  expander. Polling at ~3-5 s with TwoWire's per-bus lock keeps contention
  negligible (same approach as audio's expander access).
- **Touch after geometry change:** must confirm the GT911 reports full-panel
  0…479 coordinates and the zero-offset path doesn't reject valid touches.
- **GIF upscale cost:** nearest-neighbor upscaling per frame adds work; verify
  frame rate holds on the larger canvas (the GIF-from-PSRAM follow-up may help if
  it regresses).
- **Per-screen reflow volume:** Phase 2 touches every screen's coordinates — the
  largest single UI change in the project; done incrementally with on-device
  verification per screen.

## Reference paths

- Buddy repo: `C:\Users\tsphan-ahc\Development\claude-desktop-buddy`
- AXP2101 demo: `...\ESP32-S3-Touch-LCD-4B-Demos\Arduino-v3.2.0\examples\05_LVGL_AXP2101_ADC_Data`
- XPowersLib: `lewisxhe/XPowersLib` (PlatformIO registry)
- Brainstorm mockups: `.superpowers/brainstorm/271-1780635170/content/` (home-layout, home-bigbuddy, approval)
- Audio milestone (prior work): commit `1386534`; reflash image
  `snapshots/2026-06-05-audio-working/firmware-merged.bin`
- PlatformIO: `C:\Users\tsphan-ahc\.platformio\penv\Scripts\pio.exe` (COM4, PowerShell)
