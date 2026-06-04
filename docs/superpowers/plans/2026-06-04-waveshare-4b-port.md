# Waveshare ESP32-S3-Touch-LCD-4B Port Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Port claude-desktop-buddy from the Waveshare 2.8" (ST7789 SPI, CST328 touch) to the Waveshare 4B (ST7701 RGB parallel, GT911 touch), producing a working BLE desk buddy on the 4B hardware.

**Architecture:** Replace TFT_eSPI with Arduino_GFX (GFX Library for Arduino). Render the existing 240x320 UI into an Arduino_Canvas centered on the 480x480 RGB panel. Replace the CST328 touch driver with GT911 via SensorLib. Stub battery/power/backlight for MVP.

**Tech Stack:** PlatformIO, Arduino framework, Arduino_GFX (display), SensorLib (touch), AnimatedGIF, ArduinoJson, ESP32 BLE

**Spec:** `docs/superpowers/specs/2026-06-04-waveshare-4b-port-design.md`

**PlatformIO path:** `C:\Users\tsphan-ahc\.platformio\penv\Scripts\pio.exe`

**4B demo code:** `C:\Users\tsphan-ahc\Downloads\ESP32-S3-Touch-LCD-4B-Demos`

---

## File Structure

### Files to create

| File | Responsibility |
|---|---|
| `src/compat.h` | TFT_eSPI compatibility shims: color macros, `fillSprite` alias, `drawCenteredString` helper |

### Files to rewrite

| File | Responsibility |
|---|---|
| `platformio.ini` | New build env with Arduino_GFX + SensorLib, remove TFT_eSPI |
| `src/display.h` | Display constants (W/H), Arduino_GFX forward declarations, stub power/backlight API |
| `src/display.cpp` | XCA9554 + ESP32RGBPanel + ST7701 init chain, canvas creation, stub implementations |
| `src/touch.h` | Same public API (touchInit, touchGetPoint), updated comment |
| `src/touch.cpp` | GT911 driver via SensorLib with coordinate translation |

### Files to edit (mechanical API translation)

| File | Changes |
|---|---|
| `src/main.cpp` | New globals, stub battery/power, `spr.`->`spr->`, `fillSprite`->`fillScreen`, replace `setTextDatum`+`drawString` with `drawCenteredString`, remove `blit()` |
| `src/buddy.h` | Forward declare `Arduino_GFX` instead of `TFT_eSPI` |
| `src/buddy.cpp` | Include swap, extern type change, `.`->`->`, `TFT_eSPI*`->`Arduino_GFX*` |
| `src/character.h` | Forward declare `Arduino_GFX` instead of `TFT_eSPI` |
| `src/character.cpp` | Include swap, extern type change, `.`->`->`, `TFT_eSPI*`->`Arduino_GFX*`, `fillSprite`->`fillScreen` |
| `src/buddies/*.cpp` (x18) | Include swap, extern type change, `spr.`->`spr->` |

### Files unchanged

`src/ble_bridge.h`, `src/ble_bridge.cpp`, `src/data.h`, `src/stats.h`, `src/xfer.h`, `src/clock.h`, `src/buddy_common.h`, `partitions.csv`, `characters/*`

---

## Task 1: Create branch and rewrite platformio.ini

**Files:**
- Modify: `platformio.ini`

- [ ] **Step 1: Create the new branch**

```bash
cd C:\Users\tsphan-ahc\Development\claude-desktop-buddy
git checkout -b waveshare-s3-touch-lcd-4b
```

- [ ] **Step 2: Rewrite platformio.ini**

Replace the entire contents of `platformio.ini` with:

```ini
[env:waveshare-s3-touch-lcd-4b]
platform = espressif32
board = esp32-s3-devkitc-1
framework = arduino
monitor_speed = 115200
upload_speed = 921600
board_build.filesystem = littlefs
board_build.partitions = partitions.csv
board_build.arduino.memory_type = qio_opi
build_src_filter =
    +<*>
    +<buddies/>
build_flags =
    -DCORE_DEBUG_LEVEL=0
    -DARDUINO_USB_CDC_ON_BOOT=1
    -DBOARD_HAS_PSRAM
lib_deps =
    moononournation/GFX Library for Arduino @ ^1.5.6
    lewisxhe/SensorLib @ ^0.2.6
    bitbank2/AnimatedGIF @ ^2.1.1
    bblanchon/ArduinoJson @ ^7.0.0
```

- [ ] **Step 3: Commit**

```bash
git add platformio.ini
git commit -m "chore: switch platformio.ini to 4B board with Arduino_GFX + SensorLib"
```

---

## Task 2: Create src/compat.h

**Files:**
- Create: `src/compat.h`

- [ ] **Step 1: Create the compatibility header**

Create `src/compat.h` with this content:

```cpp
#pragma once
// Compatibility shims for TFT_eSPI -> Arduino_GFX migration.
// Keeps diffs minimal across the 20+ files that reference TFT_eSPI types.

#include <Arduino_GFX_Library.h>

// ── Color constant aliases ─────────────────────────────────────────────
// TFT_eSPI defines TFT_BLACK etc. Arduino_GFX defines BLACK etc.
// Only TFT_BLACK is actually used in the codebase, but define the common
// set for safety.
#ifndef TFT_BLACK
#define TFT_BLACK   BLACK
#endif
#ifndef TFT_WHITE
#define TFT_WHITE   WHITE
#endif
#ifndef TFT_RED
#define TFT_RED     RED
#endif
#ifndef TFT_GREEN
#define TFT_GREEN   GREEN
#endif
#ifndef TFT_BLUE
#define TFT_BLUE    BLUE
#endif

// ── Text datum constants ───────────────────────────────────────────────
// TFT_eSPI text alignment values — used in setTextDatum() calls that we
// replace with drawCenteredString(). Define them so any straggler compiles.
#ifndef MC_DATUM
#define MC_DATUM 4
#endif
#ifndef TL_DATUM
#define TL_DATUM 0
#endif

// ── Centered text helper ───────────────────────────────────────────────
// Replaces setTextDatum(MC_DATUM) + drawString(). Arduino_GFX does not
// have text alignment control, so we compute it manually.
inline void drawCenteredString(Arduino_GFX* gfx, const char* s,
                               int16_t cx, int16_t cy) {
  int16_t x1, y1;
  uint16_t tw, th;
  gfx->getTextBounds(s, 0, 0, &x1, &y1, &tw, &th);
  gfx->setCursor(cx - tw / 2, cy - th / 2);
  gfx->print(s);
}
```

- [ ] **Step 2: Commit**

```bash
git add src/compat.h
git commit -m "feat: add compat.h with TFT_eSPI -> Arduino_GFX shims"
```

---

## Task 3: Rewrite display.h and display.cpp

**Files:**
- Rewrite: `src/display.h`
- Rewrite: `src/display.cpp`

- [ ] **Step 1: Rewrite src/display.h**

Replace the entire contents of `src/display.h` with:

```cpp
#pragma once
#include <Arduino.h>
#include <Arduino_GFX_Library.h>

// ── Panel geometry ─────────────────────────────────────────────────────
#define PANEL_WIDTH   480
#define PANEL_HEIGHT  480

// Canvas (sprite) dimensions — matches the existing 240x320 UI
#define LCD_WIDTH     240
#define LCD_HEIGHT    320

// Canvas offset to center 240x320 on the 480x480 panel
#define CANVAS_OFF_X  ((PANEL_WIDTH  - LCD_WIDTH)  / 2)   // 120
#define CANVAS_OFF_Y  ((PANEL_HEIGHT - LCD_HEIGHT) / 2)   // 80

// ── Global display objects (defined in display.cpp) ────────────────────
extern Arduino_XCA9554SWSPI *expander;
extern Arduino_ESP32RGBPanel *rgbpanel;
extern Arduino_RGB_Display *gfx;
extern Arduino_Canvas *canvas;

// ── Public API ─────────────────────────────────────────────────────────
void displayInit();          // full hardware init: I2C, expander, RGB panel, canvas
void Set_Backlight(uint8_t pct);   // stub — always full brightness on 4B
```

- [ ] **Step 2: Rewrite src/display.cpp**

Replace the entire contents of `src/display.cpp` with:

```cpp
#include "display.h"
#include <Wire.h>

// ── Hardware objects ───────────────────────────────────────────────────
// XCA9554 IO expander: provides software SPI for ST7701 init commands.
// Constructor args: TCA8418_address, cs_pin, sck_pin, mosi_pin, Wire*, i2c_addr
Arduino_XCA9554SWSPI *expander = new Arduino_XCA9554SWSPI(
    7  /* RST */,
    0  /* CS  */,
    2  /* MOSI */,
    1  /* SCK */,
    &Wire,
    0x20 /* XCA9554 I2C address */);

// ESP32-S3 RGB parallel panel — directly drives the ST7701 data bus.
Arduino_ESP32RGBPanel *rgbpanel = new Arduino_ESP32RGBPanel(
    17 /* DE */, 3 /* VSYNC */, 46 /* HSYNC */, 9 /* PCLK */,
    10 /* B0 */, 11 /* B1 */, 12 /* B2 */, 13 /* B3 */, 14 /* B4 */,
    21 /* G0 */,  8 /* G1 */, 18 /* G2 */, 45 /* G3 */, 38 /* G4 */, 39 /* G5 */,
    40 /* R0 */, 41 /* R1 */, 42 /* R2 */,  2 /* R3 */,  1 /* R4 */,
    1  /* hsync_polarity */,  10 /* hsync_front_porch */,
    8  /* hsync_pulse_width */, 50 /* hsync_back_porch */,
    1  /* vsync_polarity */,  10 /* vsync_front_porch */,
    8  /* vsync_pulse_width */, 20 /* vsync_back_porch */);

// RGB display — sends ST7701 init commands through the expander's
// software SPI, then hands off to the RGB panel for pixel data.
Arduino_RGB_Display *gfx = new Arduino_RGB_Display(
    PANEL_WIDTH, PANEL_HEIGHT, rgbpanel,
    0 /* rotation */, true /* auto_flush */,
    expander, GFX_NOT_DEFINED /* RST */,
    st7701_type1_init_operations,
    sizeof(st7701_type1_init_operations));

// Off-screen canvas (sprite) — 240x320 centered on the 480x480 panel.
// All buddy drawing happens here; flush() pushes to the RGB framebuffer.
Arduino_Canvas *canvas = new Arduino_Canvas(
    LCD_WIDTH, LCD_HEIGHT, gfx,
    CANVAS_OFF_X, CANVAS_OFF_Y);

// ── Init ───────────────────────────────────────────────────────────────
void displayInit() {
  // Start I2C bus shared by expander, touch, and all other peripherals
  Wire.begin(47 /* SDA */, 48 /* SCL */);

  // Reset sequence via IO expander (from 4B demo code)
  expander->pinMode(5, OUTPUT);    // LCD RST
  expander->pinMode(6, OUTPUT);    // Touch RST
  expander->digitalWrite(6, LOW);  // Assert touch reset
  delay(200);
  expander->digitalWrite(5, LOW);  // Assert LCD reset
  delay(200);
  expander->digitalWrite(5, HIGH); // Release LCD reset
  delay(200);

  // Init the RGB display (sends ST7701 init commands via software SPI)
  if (!gfx->begin()) {
    Serial.println("[display] gfx->begin() failed!");
  }
  gfx->fillScreen(BLACK);

  // Init the off-screen canvas
  if (!canvas->begin()) {
    Serial.println("[display] canvas->begin() failed!");
  }
  canvas->fillScreen(BLACK);
  canvas->flush();

  Serial.println("[display] init ok");
}

// ── Stubs ──────────────────────────────────────────────────────────────
// Backlight is always-on via AXP2101 rail. Real brightness control
// requires PMIC integration (future enhancement).
void Set_Backlight(uint8_t pct) {
  (void)pct;
}
```

- [ ] **Step 3: Commit**

```bash
git add src/display.h src/display.cpp
git commit -m "feat: rewrite display driver for 4B (XCA9554 + ST7701 RGB panel + Canvas)"
```

---

## Task 4: Rewrite touch.h and touch.cpp

**Files:**
- Rewrite: `src/touch.h`
- Rewrite: `src/touch.cpp`

- [ ] **Step 1: Rewrite src/touch.h**

Replace the entire contents of `src/touch.h` with:

```cpp
#pragma once
#include <stdint.h>

// GT911 capacitive touch driver for the Waveshare ESP32-S3-Touch-LCD-4B.
// I2C on Wire (SDA=47, SCL=48), addr 0x5D. RST via XCA9554 pin 6.
// Returns coordinates mapped to the 240x320 canvas space (NOT native
// 480x480 panel coords). Touches outside the canvas region are rejected.
bool touchInit();
bool touchGetPoint(uint16_t* x, uint16_t* y);
```

- [ ] **Step 2: Rewrite src/touch.cpp**

Replace the entire contents of `src/touch.cpp` with:

```cpp
#include "touch.h"
#include "display.h"       // CANVAS_OFF_X, CANVAS_OFF_Y, LCD_WIDTH, LCD_HEIGHT
#include <Arduino.h>
#include <Wire.h>
#include "TouchDrvGT911.hpp"

static TouchDrvGT911 gt911;

bool touchInit() {
  // Wire.begin() is already called by displayInit() — don't call again.
  // RST was already asserted/released by the expander reset sequence in
  // displayInit(). GT911 RST/IRQ are not on direct GPIOs, so pass -1.
  gt911.setPins(-1, -1);
  if (!gt911.begin(Wire, GT911_SLAVE_ADDRESS_L, 47, 48)) {
    Serial.println("[touch] GT911 init failed!");
    return false;
  }
  gt911.setMaxCoordinates(PANEL_WIDTH, PANEL_HEIGHT);
  gt911.setMaxTouchPoint(1);
  Serial.println("[touch] GT911 init ok");
  return true;
}

bool touchGetPoint(uint16_t* x, uint16_t* y) {
  int16_t tx[1], ty[1];
  uint8_t touched = gt911.getPoint(tx, ty, 1);
  if (touched == 0) return false;

  // Translate from panel coords (0..479) to canvas coords (0..239 x 0..319)
  int16_t cx = tx[0] - CANVAS_OFF_X;
  int16_t cy = ty[0] - CANVAS_OFF_Y;

  // Reject touches outside the canvas region
  if (cx < 0 || cx >= LCD_WIDTH || cy < 0 || cy >= LCD_HEIGHT) return false;

  *x = (uint16_t)cx;
  *y = (uint16_t)cy;
  return true;
}
```

- [ ] **Step 3: Commit**

```bash
git add src/touch.h src/touch.cpp
git commit -m "feat: rewrite touch driver for 4B (GT911 via SensorLib + coord translation)"
```

---

## Task 5: Edit src/main.cpp

This is the largest edit — replacing display globals, battery/power stubs, the blit function, and all `spr.` -> `spr->` calls plus `setTextDatum`/`drawString` replacements.

**Files:**
- Modify: `src/main.cpp`

- [ ] **Step 1: Replace includes and globals (top of file)**

Replace lines 1-31 (the includes through the TFT_eSPI globals):

Old:
```cpp
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <FS.h>
#include <LittleFS.h>
#include <stdarg.h>
using fs::File;

#include "display.h"
#include "touch.h"
#include "clock.h"
#include "ble_bridge.h"
#include "data.h"
#include "buddy.h"
#include "character.h"
#include "stats.h"

// ───────────────────────────────────────────────────────────────────────────
// Sprite + panel geometry
// ───────────────────────────────────────────────────────────────────────────
// Full-panel sprite — 240×320 pixels maps 1:1 to the ST7789 panel. Species
// buddies center horizontally via BUDDY_X_CENTER=120; menus, info pages
// and HUD all use W/H so they re-center themselves. The sprite buffer is
// 150 KB, fits in internal SRAM alongside BLE/TFT/LittleFS allocations.
const int W = LCD_WIDTH;    // 240
const int H = LCD_HEIGHT;   // 320
const int CX = W / 2;
const int SPR_X = 0;
const int SPR_Y = 0;

TFT_eSPI    tft;
TFT_eSprite spr = TFT_eSprite(&tft);
```

New:
```cpp
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <FS.h>
#include <LittleFS.h>
#include <stdarg.h>
using fs::File;

#include "compat.h"
#include "display.h"
#include "touch.h"
#include "clock.h"
#include "ble_bridge.h"
#include "data.h"
#include "buddy.h"
#include "character.h"
#include "stats.h"

// ───────────────────────────────────────────────────────────────────────────
// Sprite + panel geometry
// ───────────────────────────────────────────────────────────────────────────
// Off-screen canvas — 240x320 pixels centered on the 480x480 RGB panel.
// All drawing goes here; canvas->flush() pushes to the panel via DMA.
// The canvas pointer is defined in display.cpp; we alias it as 'spr'
// so the bulk of the UI code needs only . -> -> changes.
const int W  = LCD_WIDTH;    // 240
const int H  = LCD_HEIGHT;   // 320
const int CX = W / 2;

#define spr (*canvas)
```

Note: The `#define spr (*canvas)` trick means all existing `spr.method()` calls compile without change — `(*canvas).method()` is equivalent to `canvas->method()`. This eliminates the need to do `spr.` -> `spr->` across the entire file. The buddies and character files will use a different approach (extern pointer).

- [ ] **Step 2: Replace battery and power section (lines ~33-79)**

Replace the entire battery/power section with stubs:

Old (lines 33-79, the battery + power helpers section):
```cpp
// ───────────────────────────────────────────────────────────────────────────
// Battery + power helpers (Waveshare S3 Touch 2.8)
// ───────────────────────────────────────────────────────────────────────────
...through to...
  ESP.restart();   // on USB the latch doesn't cut power; reboot is the fallback
}
```

New:
```cpp
// ───────────────────────────────────────────────────────────────────────────
// Battery + power stubs (Waveshare S3 Touch LCD 4B)
// ───────────────────────────────────────────────────────────────────────────
// The 4B has an AXP2101 PMIC for battery management. For MVP, we stub
// everything: always report full battery on USB. Real PMIC integration
// is a future enhancement.
int  batteryMilliVolts()    { return 4200; }
int  batteryPercent()       { return 100; }
int  batteryMilliAmps()     { return 0; }
bool batteryUsbPresent()    { return true; }
int  batteryUsbMilliVolts() { return 5000; }

static void powerOff() {
  // AXP2101 power-off requires PMIC I2C commands. For MVP, just restart.
  ESP.restart();
}
```

- [ ] **Step 3: Remove the blit() function**

Delete the entire `blit()` function (the byte-swap + LCD_addWindow section around line 95-104):

Old:
```cpp
// ───────────────────────────────────────────────────────────────────────────
// Push sprite → panel via byte-swap + LCD_addWindow
// ───────────────────────────────────────────────────────────────────────────
// TFT_eSprite stores 16-bit pixels in native (little-endian) order; the
// ST7789 expects MSB-first. Swap in place, send, swap back so the buffer
// stays consistent for the next drawing pass.
static void blit() {
  uint16_t* buf = (uint16_t*)spr.getPointer();
  uint32_t n = (uint32_t)W * H;
  for (uint32_t i = 0; i < n; i++) buf[i] = __builtin_bswap16(buf[i]);
  LCD_addWindow(0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1, buf);
  for (uint32_t i = 0; i < n; i++) buf[i] = __builtin_bswap16(buf[i]);
}
```

New:
```cpp
// ───────────────────────────────────────────────────────────────────────────
// Push canvas → RGB panel via DMA flush
// ───────────────────────────────────────────────────────────────────────────
static void blit() {
  canvas->flush();
}
```

- [ ] **Step 4: Replace setup() hardware init**

In `setup()`, replace the hardware init calls. Find:

```cpp
  PWR_Init();
  Backlight_Init();
  LCD_Init();

  spr.setColorDepth(16);
  if (!spr.createSprite(W, H)) {
    Serial.println("sprite alloc failed");
  }
  spr.fillSprite(TFT_BLACK);
  blit();   // paint the full panel black once before first content
```

Replace with:
```cpp
  displayInit();    // I2C, expander, RGB panel, canvas — all in one call
```

Note: `displayInit()` already calls `canvas->begin()`, fills with black, and flushes. No need for separate sprite creation or initial blit.

- [ ] **Step 5: Replace all setTextDatum + drawString patterns**

There are ~6 blocks in main.cpp that follow this pattern:
```cpp
spr.setTextDatum(MC_DATUM);
spr.drawString(text, x, y);
spr.setTextDatum(TL_DATUM);
```

Replace each `spr.drawString(text, x, y)` preceded by `setTextDatum(MC_DATUM)` with `drawCenteredString(&spr, text, x, y)`. Remove the `setTextDatum` calls around them.

Example — the boot splash (in `setup()`). Find:
```cpp
    spr.setTextDatum(MC_DATUM);
    spr.setTextSize(4);
    if (ownerName()[0]) {
      char line[40];
      snprintf(line, sizeof(line), "%s's", ownerName());
      spr.setTextColor(p.text, p.bg);   spr.drawString(line, W/2, H/2 - 24);
      spr.setTextColor(p.body, p.bg);   spr.drawString(petName(), W/2, H/2 + 24);
    } else {
      spr.setTextColor(p.body, p.bg);   spr.drawString("Hello!", W/2, H/2 - 24);
      spr.setTextSize(2);
      spr.setTextColor(p.textDim, p.bg);
      spr.drawString("a buddy appears", W/2, H/2 + 20);
    }
    spr.setTextDatum(TL_DATUM); spr.setTextSize(1);
```

Replace with:
```cpp
    spr.setTextSize(4);
    if (ownerName()[0]) {
      char line[40];
      snprintf(line, sizeof(line), "%s's", ownerName());
      spr.setTextColor(p.text, p.bg);   drawCenteredString(canvas, line, W/2, H/2 - 24);
      spr.setTextColor(p.body, p.bg);   drawCenteredString(canvas, petName(), W/2, H/2 + 24);
    } else {
      spr.setTextColor(p.body, p.bg);   drawCenteredString(canvas, "Hello!", W/2, H/2 - 24);
      spr.setTextSize(2);
      spr.setTextColor(p.textDim, p.bg);
      drawCenteredString(canvas, "a buddy appears", W/2, H/2 + 20);
    }
    spr.setTextSize(1);
```

Apply the same pattern to ALL other `setTextDatum(MC_DATUM)` + `drawString` blocks:
- Clock screen (~line 431-435)
- Approval "sent" label (~line 671-674)
- Approval OK/NO buttons (~line 678-682)

For each: remove `setTextDatum(MC_DATUM)` and `setTextDatum(TL_DATUM)` lines, change `spr.drawString(text, x, y)` to `drawCenteredString(canvas, text, x, y)`.

- [ ] **Step 6: Replace fillSprite with fillScreen**

Arduino_GFX Canvas uses `fillScreen()` not `fillSprite()`. Since we used `#define spr (*canvas)`, `spr.fillSprite(color)` would fail. Do a find-and-replace in main.cpp:

- `spr.fillSprite(` -> `spr.fillScreen(`

There are ~6 instances in main.cpp.

- [ ] **Step 7: Remove the backlight direct LEDC calls**

Find any direct `ledcWrite(BL_PWM_CHANNEL, ...)` calls in main.cpp and remove or replace them. Search for:
- `ledcWrite(BL_PWM_CHANNEL` — these were for direct backlight control
- `BL_PWM_CHANNEL` references

In `powerOff()`, the `ledcWrite` line was already removed in Step 2. Check `applyBrightness()` — it calls `Set_Backlight()` which is now a no-op stub, so it's fine.

- [ ] **Step 8: Commit**

```bash
git add src/main.cpp
git commit -m "feat: port main.cpp to Arduino_GFX canvas with battery/power stubs"
```

---

## Task 6: Edit src/buddy.h and src/buddy.cpp

**Files:**
- Modify: `src/buddy.h`
- Modify: `src/buddy.cpp`

- [ ] **Step 1: Edit src/buddy.h**

Change the forward declaration. Find:
```cpp
class TFT_eSPI;
void buddyRenderTo(TFT_eSPI* tgt, uint8_t personaState);
```

Replace with:
```cpp
class Arduino_GFX;
void buddyRenderTo(Arduino_GFX* tgt, uint8_t personaState);
```

- [ ] **Step 2: Edit src/buddy.cpp includes and extern**

Find (at top of file):
```cpp
#include <TFT_eSPI.h>
```

Replace with:
```cpp
#include <Arduino_GFX_Library.h>
```

Find:
```cpp
extern TFT_eSprite spr;
```

Replace with:
```cpp
extern Arduino_Canvas *canvas;  // defined in display.cpp
```

- [ ] **Step 3: Edit the _tgt pointer type and spr references**

Find:
```cpp
static TFT_eSPI* _tgt = &spr;
```

Replace with:
```cpp
static Arduino_GFX* _tgt = canvas;
```

- [ ] **Step 4: Edit buddyRenderTo signature**

Find:
```cpp
void buddyRenderTo(TFT_eSPI* tgt, uint8_t personaState) {
```

Replace with:
```cpp
void buddyRenderTo(Arduino_GFX* tgt, uint8_t personaState) {
```

Also find the local variable:
```cpp
  TFT_eSPI* prev = _tgt;
```

Replace with:
```cpp
  Arduino_GFX* prev = _tgt;
```

- [ ] **Step 5: Replace all spr. with canvas-> in buddy.cpp**

Since the extern is now `canvas` (a pointer), replace all `spr.` calls with `canvas->`. This includes:
- `spr.fillRect(` -> `canvas->fillRect(`
- `spr.setTextColor(` -> `canvas->setTextColor(`
- `spr.setTextSize(` -> `canvas->setTextSize(`
- `spr.setCursor(` -> `canvas->setCursor(`
- `spr.print(` -> `canvas->print(`
- `spr.width()` -> `canvas->width()`
- Any other `spr.` references

- [ ] **Step 6: Commit**

```bash
git add src/buddy.h src/buddy.cpp
git commit -m "feat: port buddy.h/cpp to Arduino_GFX types"
```

---

## Task 7: Edit src/character.h and src/character.cpp

**Files:**
- Modify: `src/character.h`
- Modify: `src/character.cpp`

- [ ] **Step 1: Edit src/character.h**

Find:
```cpp
class TFT_eSPI;
void characterRenderTo(TFT_eSPI* tgt, int cx, int cy);
```

Replace with:
```cpp
class Arduino_GFX;
void characterRenderTo(Arduino_GFX* tgt, int cx, int cy);
```

- [ ] **Step 2: Edit src/character.cpp includes and extern**

Find:
```cpp
#include <TFT_eSPI.h>
```

Replace with:
```cpp
#include <Arduino_GFX_Library.h>
```

Find:
```cpp
extern TFT_eSprite spr;
```

Replace with:
```cpp
extern Arduino_Canvas *canvas;  // defined in display.cpp
```

- [ ] **Step 3: Edit the _tgt pointer type**

Find:
```cpp
static TFT_eSPI*   _tgt = &spr;
```

Replace with:
```cpp
static Arduino_GFX*   _tgt = canvas;
```

- [ ] **Step 4: Edit characterRenderTo signature**

Find:
```cpp
void characterRenderTo(TFT_eSPI* tgt, int cx, int cy) {
```

Replace with:
```cpp
void characterRenderTo(Arduino_GFX* tgt, int cx, int cy) {
```

Find inside that function:
```cpp
  TFT_eSPI* prevT = _tgt;
```

Replace with:
```cpp
  Arduino_GFX* prevT = _tgt;
```

- [ ] **Step 5: Replace all spr. with canvas-> in character.cpp**

Replace all `spr.` calls with `canvas->`:
- `spr.fillSprite(` -> `canvas->fillScreen(` (note: also fillSprite -> fillScreen)
- `spr.drawPixel(` -> `canvas->drawPixel(`
- `spr.fillRect(` -> `canvas->fillRect(`
- `spr.setTextColor(` -> `canvas->setTextColor(`
- `spr.setTextSize(` -> `canvas->setTextSize(`
- `spr.setCursor(` -> `canvas->setCursor(`
- `spr.print(` -> `canvas->print(`
- `spr.width()` -> `canvas->width()`
- `spr.height()` -> `canvas->height()`

- [ ] **Step 6: Commit**

```bash
git add src/character.h src/character.cpp
git commit -m "feat: port character.h/cpp to Arduino_GFX types"
```

---

## Task 8: Bulk edit all 18 src/buddies/*.cpp files

Every species file has the identical boilerplate at the top. The change is mechanical and identical across all 18 files.

**Files:**
- Modify: `src/buddies/axolotl.cpp`
- Modify: `src/buddies/blob.cpp`
- Modify: `src/buddies/cactus.cpp`
- Modify: `src/buddies/capybara.cpp`
- Modify: `src/buddies/cat.cpp`
- Modify: `src/buddies/chonk.cpp`
- Modify: `src/buddies/dragon.cpp`
- Modify: `src/buddies/duck.cpp`
- Modify: `src/buddies/ghost.cpp`
- Modify: `src/buddies/goose.cpp`
- Modify: `src/buddies/mushroom.cpp`
- Modify: `src/buddies/octopus.cpp`
- Modify: `src/buddies/owl.cpp`
- Modify: `src/buddies/penguin.cpp`
- Modify: `src/buddies/rabbit.cpp`
- Modify: `src/buddies/robot.cpp`
- Modify: `src/buddies/snail.cpp`
- Modify: `src/buddies/turtle.cpp`

- [ ] **Step 1: In every file, replace the TFT_eSPI include**

Each file has:
```cpp
#include <TFT_eSPI.h>
```

Replace with:
```cpp
#include <Arduino_GFX_Library.h>
```

- [ ] **Step 2: In every file, replace the extern declaration**

Each file has:
```cpp
extern TFT_eSprite spr;
```

Replace with:
```cpp
extern Arduino_Canvas *canvas;  // defined in display.cpp
```

- [ ] **Step 3: In every file, replace spr. with canvas->**

The species files only use `spr` in a few places (typically inherited from `buddy_common.h` macros or direct calls). Search each file for `spr.` and replace with `canvas->`.

Also replace any bare `spr` references (passed as argument) with `canvas`.

Note: Most species files do NOT directly call `spr.` methods — they use `buddyPrintSprite()`, `buddySetColor()`, `buddySetCursor()`, `buddyPrint()` which are functions in `buddy.cpp` that use the `_tgt` pointer. But a few files (penguin.cpp, blob.cpp, octopus.cpp) have direct `spr.drawPixel()` or `spr.fillRect()` calls. Check each file.

- [ ] **Step 4: Commit**

```bash
git add src/buddies/*.cpp
git commit -m "feat: port all 18 buddy species to Arduino_GFX types"
```

---

## Task 9: Compile and fix errors

**Files:**
- Potentially any file from Tasks 1-8

- [ ] **Step 1: Run the build**

```bash
C:\Users\tsphan-ahc\.platformio\penv\Scripts\pio.exe run
```

Expected: Either a clean build, or compile errors from missed API differences.

- [ ] **Step 2: Fix any compile errors**

Common issues to watch for:
- **Missing `fillSprite`**: Any remaining `fillSprite` calls need to become `fillScreen`
- **`getPointer()` vs `getFramebuffer()`**: If any code accesses the raw pixel buffer
- **`setTextDatum` / `drawString`**: Any straggler calls not caught in Task 5
- **`TFT_eSPI` type references**: Any remaining type names not updated
- **`BL_PWM_CHANNEL`**: References to the old backlight PWM channel constant
- **`PWR_Control_PIN` / `PWR_KEY_Input_PIN`**: References to old power GPIO defines
- **`LCD_addWindow` / `LCD_SetCursor`**: Old display API calls
- **`SPR_X` / `SPR_Y`**: Old constants removed in Task 5

Fix each error, re-run the build, iterate until clean.

- [ ] **Step 3: Commit fixes**

```bash
git add -u
git commit -m "fix: resolve compile errors from Arduino_GFX port"
```

---

## Task 10: Push to fork

- [ ] **Step 1: Push the new branch to the fork**

```bash
git push -u origin waveshare-s3-touch-lcd-4b
```

Expected: Branch appears at https://github.com/greipfrut/claude-desktop-buddy/tree/waveshare-s3-touch-lcd-4b

- [ ] **Step 2: Verify on GitHub**

Confirm the branch is visible and contains all commits from Tasks 1-9.
