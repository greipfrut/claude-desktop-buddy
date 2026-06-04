# CLAUDE.md

Project guide for AI coding assistants working on claude-desktop-buddy.

## Project Overview

claude-desktop-buddy is an ESP32 BLE desk pet for Claude Desktop. It connects to the Claude macOS/Windows app over Bluetooth Low Energy (Nordic UART Service), displaying permission prompts, conversation state, and an animated ASCII or GIF pet character. The pet reacts to Claude sessions: sleeping when idle, sweating when busy, alerting on approval prompts, and celebrating on level-ups.

**Supported boards (one branch per board):**

| Branch | Board | Display | Touch |
|---|---|---|---|
| `main` | M5StickC Plus | 135x240 ST7789 SPI | Physical buttons |
| `waveshare-s3-touch-28` | Waveshare ESP32-S3-Touch-LCD-2.8 | 240x320 ST7789 SPI | CST328 capacitive (I2C) |
| `waveshare-s3-touch-lcd-4b` | Waveshare ESP32-S3-Touch-LCD-4B | 480x480 ST7701 RGB parallel | GT911 capacitive (I2C) |

All branches share the same BLE protocol, GIF character system, 18 ASCII buddy species, and state machine. Only display/touch/power drivers differ.

## Build & Flash

This project uses PlatformIO (Arduino framework). The PlatformIO CLI path on this machine is `C:\Users\tsphan-ahc\.platformio\penv\Scripts\pio.exe`.

**On Windows, use PowerShell (not Git Bash) for pio commands** -- Git Bash can mangle serial port paths and POSIX path translation breaks some PlatformIO operations.

```powershell
# Build
pio run

# Upload firmware (add --upload-port COMx on Windows, /dev/ttyUSBx on Linux if auto-detect fails)
pio run -t upload

# Upload LittleFS filesystem (GIF character packs)
pio run -t uploadfs

# Serial monitor
pio run -t monitor

# Full wipe + flash (for previously-flashed devices)
pio run -t erase && pio run -t upload
```

The build environment is defined in `platformio.ini`. Each branch has its own environment name (e.g., `[env:waveshare-s3-touch-lcd-4b]`).

## Architecture

### Rendering Pipeline

All drawing goes into an off-screen buffer (canvas/sprite), and a single `blit()` or `flush()` call pushes it to the display hardware. The GIF renderer, all 18 ASCII buddy species, BLE protocol handler, and every UI overlay draw into this buffer through the drawing API -- they never call display hardware directly.

On the 4B branch:
```
Arduino_Canvas (240x320, offset 120,80) --> canvas->flush() --> DMA --> RGB panel (480x480)
```

The 240x320 UI is centered on the 480x480 panel with black borders. This avoids redesigning the UI for the larger display.

### Hardware Abstraction

Hardware-specific code is isolated to a small set of files:

| File | Responsibility |
|---|---|
| `src/display.h` | Panel geometry constants, display object declarations, power/backlight API |
| `src/display.cpp` | Display init sequence, pin mappings, hardware object construction |
| `src/touch.h` | Touch API: `touchInit()`, `touchGetPoint()` |
| `src/touch.cpp` | Touch IC driver, coordinate translation to canvas space |
| `src/compat.h` | Compatibility shims when changing display libraries (color macros, text helpers) |
| `platformio.ini` | Board definition, libraries, build flags |

### Files That Never Change When Porting

These files are hardware-agnostic and should not need modification for a new board (after the initial API type translation if the display library changes):

- `src/ble_bridge.h` / `src/ble_bridge.cpp` -- Pure ESP32 BLE stack (Nordic UART Service)
- `src/data.h` -- JSON wire protocol state machine
- `src/stats.h` -- NVS-backed persistent stats and settings
- `src/xfer.h` -- LittleFS file transfer (GIF character push over BLE)
- `src/clock.h` -- POSIX software RTC
- `src/buddy_common.h` -- Shared geometry externs for buddy rendering
- `partitions.csv` -- Flash partition layout (NVS 20KB, app 2.5MB, SPIFFS/LittleFS 1.44MB)
- `characters/*` -- GIF character pack assets

### Key Source Files

- `src/main.cpp` -- Main loop, state machine, all UI screens (home, info, menu, approval, clock)
- `src/buddy.cpp` -- ASCII species dispatch and shared render helpers (text positioning, scaling)
- `src/buddies/*.cpp` -- One file per species (18 total), each with 7 animation functions
- `src/character.cpp` -- GIF decode and render via AnimatedGIF library

### BLE Protocol

The BLE protocol uses the Nordic UART Service and is fully board-independent. See `REFERENCE.md` for the wire protocol specification (UUIDs, JSON schemas, folder push transport). Any device that advertises Nordic UART and parses newline-delimited JSON will work.

## Porting to a New Board

Step-by-step workflow based on the experience porting from the 2.8" to the 4B.

### Step 1: Research Your Board

Gather from the manufacturer's demo code, datasheet, or wiki:
- Display controller IC (e.g., ST7789, ST7701, ILI9341)
- Display interface (SPI, RGB parallel, MIPI DSI) and pin assignments
- Display resolution and pixel format (RGB565, RGB666)
- Touch controller IC (e.g., CST328, GT911, FT6336) and I2C address
- Touch I2C pins (SDA, SCL) and whether they share a bus with other peripherals
- IO expanders if present (e.g., XCA9554) and their I2C addresses
- Power management (direct GPIO, PMIC like AXP2101)
- Battery monitoring (ADC pin with voltage divider, or PMIC)
- Backlight control (PWM GPIO, or PMIC rail)
- Flash and PSRAM size and type (QIO, OPI)
- Any other peripherals (IMU, RTC, audio codec)

The manufacturer's Arduino demo code is the most reliable source for pin mappings and init sequences. For Waveshare boards, look in the `Arduino-v3.x.x/examples/` directory of their demo package.

### Step 2: Compare Against the Closest Existing Port

Create a hardware comparison table (see the design spec in `docs/superpowers/specs/` for an example). Identify what differs:
- Same display interface? If not, you may need a different display library.
- Same touch IC? If not, you need a different touch driver.
- Same power management? If not, stub it for MVP and implement later.

### Step 3: Create a New Branch

Fork from the branch with the most similar hardware:
```bash
git checkout -b my-new-board-port waveshare-s3-touch-lcd-4b
```

### Step 4: Rewrite platformio.ini

Critical decisions:
- **Platform:** Use `https://github.com/pioarduino/platform-espressif32/releases/download/53.03.13/platform-espressif32.zip` (pioarduino fork) for Arduino core v3 support. The stock `espressif32` platform uses Arduino core v2 which has API differences (see Common Pitfalls).
- **Board definition:** Must match your board's actual flash/PSRAM configuration. For ESP32-S3 with 16MB flash + 8MB OPI PSRAM at 120MHz, use `esp32s3_120_16_8-qio_opi`. Using a generic board def like `esp32-s3-devkitc-1` may silently disable PSRAM, causing crashes.
- **Display library:** `moononournation/GFX Library for Arduino` for RGB parallel displays. `TFT_eSPI` for SPI displays. Match what the manufacturer demo uses.
- **Touch library:** `lewisxhe/SensorLib` supports GT911, CST328, FT6336, and many others.
- **Always include:** `bitbank2/AnimatedGIF @ ^2.1.1` and `bblanchon/ArduinoJson @ ^7.0.0`

### Step 5: Rewrite display.h and display.cpp

This is the most board-specific code. Key lessons learned:

**display.h:** Define panel geometry constants (`PANEL_WIDTH`, `PANEL_HEIGHT`, `LCD_WIDTH`, `LCD_HEIGHT`), canvas offset if the UI is smaller than the panel, and declare the global display objects.

**display.cpp:**
- **If using RGB parallel displays (ST7701, etc.):** Initialize display objects as `nullptr` globals and construct them inside `displayInit()`, not at file scope. The RGB panel driver calls `esp_lcd_new_rgb_panel()` which requires PSRAM to be initialized, and PSRAM is not ready during C++ static initialization.
- **Do NOT call both `gfx->begin()` and `canvas->begin()`.** `canvas->begin()` internally calls `gfx->begin()`. The ESP32-S3 has only one RGB panel slot -- calling begin twice exhausts it and crashes.
- Copy the IO expander reset sequence (if applicable) and RGB timing parameters directly from the manufacturer's demo code. These values are board-specific and getting them wrong produces a blank or garbled display.
- The init sequence for boards with IO expanders (like the 4B's XCA9554) typically needs: configure expander pins as outputs, assert resets (LCD and touch), delay, release resets, delay.

### Step 6: Rewrite touch.h and touch.cpp

Keep the same public API:
```cpp
bool touchInit();
bool touchGetPoint(uint16_t* x, uint16_t* y);
```

If the canvas is smaller than the panel, `touchGetPoint()` must translate from panel coordinates to canvas coordinates and reject touches outside the canvas region:
```cpp
int16_t cx = touchX - CANVAS_OFF_X;
int16_t cy = touchY - CANVAS_OFF_Y;
if (cx < 0 || cx >= LCD_WIDTH || cy < 0 || cy >= LCD_HEIGHT) return false;
```

If `Wire.begin()` is called in `displayInit()` (because the I2C bus is shared), do not call it again in `touchInit()`.

### Step 7: Create or Update compat.h

Needed when the display library changes (e.g., TFT_eSPI to Arduino_GFX). Provides:
- **Color constant macros:** `#define TFT_BLACK BLACK` etc., so existing code compiles without renaming every color reference.
- **Text datum stubs:** `#define MC_DATUM 4` etc., so straggler `setTextDatum()` calls compile (they become no-ops).
- **`drawCenteredString()` helper:** Arduino_GFX lacks `setTextDatum()` + `drawString()`. This helper uses `getTextBounds()` to compute centered positioning manually.

### Step 8: Edit main.cpp

- Replace includes (`TFT_eSPI.h` -> `Arduino_GFX_Library.h`, add `compat.h`).
- Replace display globals. The `#define spr (*canvas)` trick lets all existing `spr.method()` calls compile as `(*canvas).method()` without touching each call site.
- Stub or implement battery/power functions. For MVP, stub: `batteryMilliVolts()` returns 4200, `batteryUsbPresent()` returns true, `powerOff()` calls `ESP.restart()`.
- Replace `blit()` with `canvas->flush()`.
- Replace `spr.fillSprite()` with `spr.fillScreen()` (Arduino_GFX naming).
- Replace `setTextDatum(MC_DATUM)` + `drawString()` patterns with `drawCenteredString(canvas, text, x, y)`.
- Remove direct backlight LEDC calls (replace with `Set_Backlight()` stub or remove entirely).

### Step 9: Edit buddy.cpp, character.cpp, and buddies/*.cpp

These files need API type changes when the display library changes.

**buddy.cpp and character.cpp:**
- Change `extern TFT_eSprite spr;` to `extern Arduino_Canvas *canvas;`
- Change `TFT_eSPI*` to `Arduino_GFX*` in function signatures and local variables
- Change `spr.` calls to `canvas->` calls

**CRITICAL -- Lazy initialization of `_tgt` pointers:**

Both `buddy.cpp` and `character.cpp` have a static pointer `_tgt` that is used as the current render target. In the TFT_eSPI version, this was initialized from `&spr` (a stack/global object). With Arduino_GFX, `canvas` is a pointer that starts as `nullptr` (constructed in `displayInit()` at runtime). If you write `static Arduino_GFX* _tgt = canvas;` at file scope, `_tgt` captures `nullptr` because static initializers run before `displayInit()`.

The fix is lazy initialization:
```cpp
static Arduino_GFX* _tgt = nullptr;  // can't init from global ptr

// At the top of the main render function:
if (!_tgt) _tgt = canvas;   // lazy init -- canvas is null during static init
```

**buddies/*.cpp (18 files):**
- Change `#include <TFT_eSPI.h>` to `#include <Arduino_GFX_Library.h>`
- Change `extern TFT_eSprite spr;` to `extern Arduino_Canvas *canvas;`
- Change `spr.` to `canvas->` for any direct draw calls (most species use helper functions from buddy.cpp, but some like penguin.cpp and octopus.cpp have direct `spr.drawPixel()` or `spr.fillRect()` calls)

### Step 10: Check the Partition Table

If using Arduino core v3, LittleFS expects the data partition to be named `spiffs` in `partitions.csv` (not `littlefs`). The existing `partitions.csv` already uses `spiffs` as the name with `data/spiffs` type, which works for both SPIFFS and LittleFS.

### Step 11: Build, Flash, Debug Iteratively

```powershell
# Build and check for compile errors
pio run

# Flash and monitor serial output
pio run -t upload && pio run -t monitor
```

If you get crash backtraces (guru meditation errors), decode them:
```powershell
# Find the addr2line tool path in the PlatformIO toolchain
# Decode the PC address from the backtrace
xtensa-esp32s3-elf-addr2line -e .pio/build/ENV_NAME/firmware.elf 0xADDRESS
```

## Common Pitfalls

These are specific bugs encountered during the 2.8" to 4B port and their fixes.

### Double begin() Exhausting RGB Panel Slots

**Symptom:** Crash during display init with obscure ESP-IDF error about panel allocation.
**Cause:** Calling `gfx->begin()` and then `canvas->begin()`. The canvas internally calls `gfx->begin()`, and the ESP32-S3 only has one RGB panel slot.
**Fix:** Only call `canvas->begin()`. It handles the full chain.

### Static Init of _tgt From nullptr Canvas

**Symptom:** Crash (null pointer dereference) on first render call, backtrace points into buddy.cpp or character.cpp.
**Cause:** `static Arduino_GFX* _tgt = canvas;` at file scope captures `nullptr` because `canvas` is constructed later in `displayInit()`.
**Fix:** Initialize `_tgt` to `nullptr` and set it lazily: `if (!_tgt) _tgt = canvas;` at the top of `buddyRenderTo()` / `characterRender()`.

### Wrong Board Definition Hiding PSRAM

**Symptom:** Build succeeds but device crashes at runtime when allocating the framebuffer. Serial output may show heap allocation failures.
**Cause:** Using a generic board definition like `esp32-s3-devkitc-1` that doesn't enable or correctly configure PSRAM. RGB parallel displays need a large DMA framebuffer (480x480x2 = 450KB+) that only fits in PSRAM.
**Fix:** Use a board definition that matches your actual hardware. For ESP32-S3 with 16MB flash and 8MB OPI PSRAM: `esp32s3_120_16_8-qio_opi` (from the pioarduino platform).

### Partition Name Mismatch in Arduino Core v3

**Symptom:** LittleFS.begin() fails, GIF characters can't be loaded.
**Cause:** Arduino core v3's LittleFS implementation looks for a partition named `spiffs`, not `littlefs`.
**Fix:** In `partitions.csv`, name the data partition `spiffs` with SubType `spiffs`. This works for both LittleFS and SPIFFS.

### Arduino Core v2 vs v3 API Differences

**Symptom:** Compile errors in BLE code or other ESP-IDF wrapper code.
**Cause:** The pioarduino platform uses Arduino core v3, which has breaking changes from v2.
**Key difference:** `BLECharacteristic::getValue()` returns `String` in v3 (was `std::string` in v2). Check for `.c_str()` calls on BLE values and adapt accordingly.

### Display Library API Differences (TFT_eSPI vs Arduino_GFX)

| TFT_eSPI | Arduino_GFX | Notes |
|---|---|---|
| `spr.createSprite(w,h)` | `canvas->begin()` | Allocation happens at begin() |
| `spr.pushSprite(0,0)` | `canvas->flush()` | DMA write to panel |
| `spr.fillSprite(c)` | `canvas->fillScreen(c)` | Name change |
| `spr.getPointer()` | `canvas->getFramebuffer()` | Returns uint16_t* |
| `spr.setTextDatum(MC_DATUM)` + `drawString()` | `drawCenteredString()` helper | No built-in text alignment |
| `TFT_eSprite spr(...)` (stack/global) | `Arduino_Canvas *canvas` (heap pointer) | Access via `->` not `.` |

All other drawing methods (`drawPixel`, `fillRect`, `setTextColor`, `setCursor`, `setTextSize`, `print`, `width`, `height`) are identical between TFT_eSPI and Arduino_GFX.

## Project Layout

```
src/
  main.cpp           -- main loop, state machine, all UI screens
  display.h/cpp      -- display hardware driver (board-specific)
  touch.h/cpp        -- touch hardware driver (board-specific)
  compat.h           -- display library compatibility shims
  buddy.cpp/h        -- ASCII species dispatch + render helpers
  buddy_common.h     -- shared geometry constants
  buddies/           -- one .cpp per species (18 total), 7 animations each
  character.cpp/h    -- GIF decode + render via AnimatedGIF
  ble_bridge.cpp/h   -- Nordic UART Service BLE stack
  data.h             -- wire protocol JSON parser
  stats.h            -- NVS-backed persistent stats and settings
  xfer.h             -- BLE folder push receiver (GIF character transfer)
  clock.h            -- software RTC
characters/          -- example GIF character packs (e.g., bufo/)
tools/               -- prep_character.py, flash_character.py
docs/                -- design specs and implementation plans
platformio.ini       -- PlatformIO build configuration
partitions.csv       -- ESP32 flash partition table
REFERENCE.md         -- BLE wire protocol specification
```

## Future Enhancements (4B Branch)

These peripherals exist on the 4B hardware but are stubbed for MVP:

- **AXP2101 PMIC:** Real battery percentage, charging status, USB detection, backlight brightness control, proper power-off via PMIC I2C commands
- **QMI8658 IMU:** Shake/tilt gestures (dizzy on shake, nap when face-down, auto-rotate) -- was in original M5StickC code, removed during 2.8" port
- **PCF85063 RTC:** Hardware real-time clock so time persists across reboots without the desktop bridge
- **ES8311/ES7210 audio:** Beep feedback on approval, notification sounds, potentially voice interaction
- **480x480 native layout:** Redesign UI to use the full square display instead of centering 240x320
