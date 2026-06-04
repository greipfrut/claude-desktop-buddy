# Porting Claude Desktop Buddy to New Hardware

## Introduction

Claude Desktop Buddy was originally written for the M5StickC Plus (135x240,
ST7789 SPI, MPU6886 IMU, single button + power button). It was then ported to
the Waveshare ESP32-S3-Touch-LCD-2.8 (240x320, ST7789 SPI, CST328 touch), and
most recently to the Waveshare ESP32-S3-Touch-LCD-4B (480x480, ST7701 RGB
parallel, GT911 touch, AXP2101 PMIC, plus IMU/RTC/audio peripherals).

Each port taught lessons that make the next one easier. The codebase now has a
clean separation between hardware-specific code and portable application logic.
Porting to a new ESP32-based board means rewriting about three files, doing
mechanical find-and-replace across twenty more, and stubbing any peripherals you
do not have.

This guide walks through the process using the 2.8-inch to 4B port as its
running example.

## Hardware You Need

**Required:**
- An ESP32 or ESP32-S3 board with a color display (any resolution, any
  controller)
- Touch input or physical buttons (the UI needs at least "tap" and "long press")
- BLE support (all ESP32 variants have this)
- Enough flash for the firmware plus a LittleFS partition for GIF character
  packs (the partition table allocates 2.5 MB for the app and 1.44 MB for the
  filesystem)
- PSRAM is strongly recommended -- the GIF decoder and display framebuffer
  benefit from the extra memory

**Optional (stub and add later):**
- Battery and power management IC -- stub `batteryMilliVolts()` to return 4200
  and `batteryUsbPresent()` to return true
- IMU / accelerometer -- the shake-to-dizzy and face-down-nap gestures are fun
  but not essential
- RTC -- the software clock in `clock.h` works fine when the desktop bridge is
  connected
- Audio -- beep feedback on approval is a nice-to-have
- Backlight control -- if your display is always-on, make `Set_Backlight()` a
  no-op

## The Porting Process (Step by Step)

### 1. Gather hardware specs and demo code

Before writing any code, collect pin mappings and initialization sequences for
your board. The most reliable sources are, in order:

1. **Manufacturer demo code.** Waveshare ships Arduino examples for each board.
   The display init sequence, I2C addresses, pin assignments, and RGB timing
   parameters all came from `01_HelloWorld.ino` and `06_LVGL_Arduino_v9.ino` in
   the 4B demo archive. Do not try to reverse-engineer these from datasheets --
   the demo code has already solved the problem.

2. **Schematic PDF.** The schematic clarifies which GPIOs are directly wired vs.
   routed through an IO expander, and which I2C devices share a bus.

3. **Datasheet for the display controller.** You usually do not need this at all
   -- the demo code provides the init command sequence as an opaque byte array.
   But if the display has unusual timing or rotation behavior, the datasheet
   explains the register map.

For the 4B port, the critical information gathered was:

- Display: 480x480 ST7701S driven by an RGB parallel bus (16 data pins + 4 sync
  pins), with init commands sent over a software SPI bus routed through an
  XCA9554 IO expander at I2C address 0x20.
- Touch: GT911 at I2C address 0x5D on the same I2C bus (SDA=47, SCL=48), with
  its reset pin on XCA9554 pin 6 (not a direct GPIO).
- Power: AXP2101 PMIC at I2C address 0x34 handles battery, USB detection, and
  the backlight power rail.

### 2. Set up the build (platformio.ini)

Create a new PlatformIO environment for your board. The key decisions:

**Board definition.** Start with a generic board definition like
`esp32-s3-devkitc-1` if your exact board is not in the PlatformIO registry.
During the 4B port we started with `esp32-s3-devkitc-1` but switched to
`esp32s3_120_16_8-qio_opi` from the pioarduino platform fork because the stock
Espressif platform had issues with the Arduino core version needed by
Arduino_GFX.

**PSRAM.** If your board has PSRAM (most ESP32-S3 boards with 8 MB do), add:

```ini
board_build.arduino.memory_type = qio_opi
build_flags =
    -DBOARD_HAS_PSRAM
```

**Display library.** The choice of display library is driven by your display
controller. TFT_eSPI supports most SPI-connected displays (ST7789, ILI9341,
etc.) and is what the 2.8-inch branch uses. Arduino_GFX (GFX Library for
Arduino) supports RGB parallel displays like the ST7701 and is what the 4B
branch uses. LVGL is another option if you want a full UI framework, but the
buddy firmware does not use it.

**Libraries to swap.** Remove the old display library dependency and add the new
one. For the 4B port:

```ini
lib_deps =
    moononournation/GFX Library for Arduino @ ^1.5.6
    lewisxhe/SensorLib @ ^0.2.6        ; GT911 touch driver
    bitbank2/AnimatedGIF @ ^2.1.1       ; unchanged
    bblanchon/ArduinoJson @ ^7.0.0      ; unchanged
```

**Platform version.** This is the most likely source of build failures. The
stock `espressif32` platform may ship an Arduino core version that is
incompatible with your display library. The 4B port ended up using the
pioarduino community fork:

```ini
platform = https://github.com/pioarduino/platform-espressif32/releases/download/53.03.13/platform-espressif32.zip
```

If your build fails with errors deep inside ESP-IDF or the Arduino core, try a
different platform version before debugging further.

### 3. Write the display driver

The display driver is the biggest variable in any port. It lives in two files:
`src/display.h` (constants and declarations) and `src/display.cpp`
(initialization and hardware control).

**What the rest of the codebase expects.** All rendering goes through a single
off-screen buffer. On the 2.8-inch branch this is a `TFT_eSprite`; on the 4B
branch it is an `Arduino_Canvas`. The buffer has the logical UI resolution
(240x320) regardless of the physical panel size. A `blit()` function in
`main.cpp` pushes the buffer to the display hardware.

**display.h defines:**
- `LCD_WIDTH` and `LCD_HEIGHT` -- the logical UI resolution (240x320)
- Panel dimensions if different from the UI resolution
- Canvas offset constants if centering the UI on a larger panel
- Forward declarations of global display objects
- The `displayInit()` and `Set_Backlight()` function prototypes

Here is the 4B display.h, which centers a 240x320 canvas on a 480x480 panel:

```cpp
#pragma once
#include <Arduino.h>
#include <Arduino_GFX_Library.h>

#define PANEL_WIDTH   480
#define PANEL_HEIGHT  480
#define LCD_WIDTH     240
#define LCD_HEIGHT    320
#define CANVAS_OFF_X  ((PANEL_WIDTH  - LCD_WIDTH)  / 2)   // 120
#define CANVAS_OFF_Y  ((PANEL_HEIGHT - LCD_HEIGHT) / 2)   // 80

extern Arduino_XCA9554SWSPI *expander;
extern Arduino_ESP32RGBPanel *rgbpanel;
extern Arduino_RGB_Display *gfx;
extern Arduino_Canvas *canvas;

void displayInit();
void Set_Backlight(uint8_t pct);
```

**display.cpp** constructs the display driver chain and runs the initialization
sequence. The 4B has a four-layer chain: I2C IO expander provides software SPI
for sending ST7701 init commands, then the ESP32 RGB parallel peripheral takes
over for pixel data, and an off-screen canvas provides the sprite buffer:

```
Wire (I2C) --> XCA9554 expander --> software SPI --> ST7701 init commands
                                --> ESP32RGBPanel --> RGB parallel pixel data
                                                  --> Arduino_Canvas (240x320)
```

The initialization sequence follows the manufacturer demo code exactly. The
critical lesson from this port: **do not call `gfx->begin()` separately if you
are using `Arduino_Canvas`**. The canvas internally calls `gfx->begin()` during
its own `begin()`. The ESP32-S3 only supports one RGB panel instance, and
calling `begin()` twice exhausts it, causing a silent failure where the display
shows nothing.

Another critical lesson: **do not construct display objects at global scope**.
The RGB panel driver calls `esp_lcd_new_rgb_panel()` during construction, which
requires PSRAM to be initialized. PSRAM is not available during C++ static
initialization. Declare the global pointers as `nullptr` and construct the
objects inside `displayInit()`:

```cpp
// WRONG -- crashes before main() because PSRAM is not up yet
Arduino_Canvas *canvas = new Arduino_Canvas(240, 320, gfx, 120, 80);

// RIGHT -- construct after runtime init
Arduino_Canvas *canvas = nullptr;

void displayInit() {
    // ... I2C, expander, panel setup ...
    canvas = new Arduino_Canvas(LCD_WIDTH, LCD_HEIGHT, gfx,
                                CANVAS_OFF_X, CANVAS_OFF_Y);
    canvas->begin();
}
```

**If your display uses SPI** (like the original 2.8-inch ST7789), the driver is
much simpler. TFT_eSPI handles everything through build flags in
`platformio.ini`. Your `display.cpp` calls `tft.init()`, sets rotation, and
creates a sprite with `spr.createSprite(W, H)`.

### 4. Write the touch driver

The touch driver lives in `src/touch.h` and `src/touch.cpp`. The public API is
exactly two functions:

```cpp
bool touchInit();
bool touchGetPoint(uint16_t* x, uint16_t* y);
```

Every touch controller has its own I2C address, register protocol, and
coordinate system. The cleanest approach is to use a library that abstracts
these differences. The 4B port uses SensorLib's `TouchDrvGT911` class.

**Coordinate translation** is the key detail. If your physical panel is larger
than the logical UI resolution, you need to map touch coordinates from panel
space to canvas space. For the 4B port (480x480 panel, 240x320 canvas centered
at offset 120,80):

```cpp
bool touchGetPoint(uint16_t* x, uint16_t* y) {
  int16_t tx[1], ty[1];
  uint8_t touched = gt911.getPoint(tx, ty, 1);
  if (touched == 0) return false;

  // Panel coords --> canvas coords
  int16_t cx = tx[0] - CANVAS_OFF_X;
  int16_t cy = ty[0] - CANVAS_OFF_Y;

  // Reject touches outside the canvas region
  if (cx < 0 || cx >= LCD_WIDTH || cy < 0 || cy >= LCD_HEIGHT) return false;

  *x = (uint16_t)cx;
  *y = (uint16_t)cy;
  return true;
}
```

Touches outside the canvas area (the black borders) are silently rejected. The
rest of the application thinks the touch panel is exactly 240x320 pixels.

**If your board uses buttons instead of touch,** implement `touchGetPoint()` to
return false and handle button input separately. The original M5StickC Plus port
used physical buttons mapped to the same actions as touch regions.

### 5. Stub power and battery

For a first bring-up, stub everything. This gets the display and touch working
before you wrestle with a PMIC driver:

```cpp
int  batteryMilliVolts()    { return 4200; }
int  batteryPercent()       { return 100; }
int  batteryMilliAmps()     { return 0; }
bool batteryUsbPresent()    { return true; }
int  batteryUsbMilliVolts() { return 5000; }

static void powerOff() {
  ESP.restart();   // real power-off needs PMIC commands
}

void Set_Backlight(uint8_t pct) {
  (void)pct;       // always-on for now
}
```

The stubs disable auto-screen-off (the UI only dims when on battery) and make
power-off just reboot. This is fine for desk use. Implement real PMIC
integration later once everything else works.

### 6. The mechanical API translation

This is the tedious part. About 24 files reference the display library types
directly. The changes are repetitive but safe -- they are almost entirely
type-name substitutions and value-to-pointer syntax changes.

**Create a compatibility header (src/compat.h).** This file maps old names to
new ones so you do not have to touch every color constant and text alignment
call in the codebase:

```cpp
#pragma once
#include <Arduino_GFX_Library.h>

// Color aliases: TFT_eSPI uses TFT_BLACK, Arduino_GFX uses BLACK
#ifndef TFT_BLACK
#define TFT_BLACK   BLACK
#endif
// ... TFT_WHITE, TFT_RED, TFT_GREEN, TFT_BLUE ...

// Text alignment: Arduino_GFX has no setTextDatum(), so we compute it
inline void drawCenteredString(Arduino_GFX* gfx, const char* s,
                               int16_t cx, int16_t cy) {
  int16_t x1, y1;
  uint16_t tw, th;
  gfx->getTextBounds(s, 0, 0, &x1, &y1, &tw, &th);
  gfx->setCursor(cx - tw / 2, cy - th / 2);
  gfx->print(s);
}
```

**The spr macro trick in main.cpp.** The biggest file has hundreds of
`spr.method()` calls. Instead of changing every one to `canvas->method()`, the
4B port defines a macro:

```cpp
#define spr (*canvas)
```

This makes `spr.fillScreen(BLACK)` expand to `(*canvas).fillScreen(BLACK)`,
which is equivalent to `canvas->fillScreen(BLACK)`. One line eliminates hundreds
of edits in main.cpp.

**buddy.cpp, character.cpp, and the 18 species files** use the sprite through
an `extern` declaration and a static `_tgt` pointer. These files need three
changes each:

1. Replace `#include <TFT_eSPI.h>` with `#include <Arduino_GFX_Library.h>`
2. Replace `extern TFT_eSprite spr;` with `extern Arduino_Canvas *canvas;`
3. Replace `spr.` calls with `canvas->` calls

For buddy.cpp and character.cpp, also change:
- `static TFT_eSPI* _tgt = &spr;` to `static Arduino_GFX* _tgt = nullptr;`
  with lazy initialization in the tick function: `if (!_tgt) _tgt = canvas;`
- Function signatures from `TFT_eSPI*` to `Arduino_GFX*`

The lazy initialization pattern is necessary because `canvas` is null during
C++ static initialization (see the PSRAM timing issue in step 3).

**API differences to watch for:**

| TFT_eSPI | Arduino_GFX | Notes |
|---|---|---|
| `spr.createSprite(w, h)` | `canvas->begin()` | Allocation happens at begin() |
| `spr.pushSprite(0, 0)` | `canvas->flush()` | DMA write to panel |
| `spr.fillSprite(color)` | `canvas->fillScreen(color)` | Name change |
| `spr.getPointer()` | `canvas->getFramebuffer()` | Raw pixel buffer |
| `spr.setTextDatum(MC_DATUM)` | (removed, use drawCenteredString) | No equivalent |
| `spr.drawString(s, x, y)` | `drawCenteredString(gfx, s, x, y)` | Helper in compat.h |
| `TFT_eSprite spr(&tft)` | `Arduino_Canvas *canvas` | Value to pointer |

Most drawing methods (`drawPixel`, `fillRect`, `setTextColor`, `setCursor`,
`setTextSize`, `print`, `width`, `height`) are identical between TFT_eSPI and
Arduino_GFX.

### 7. Testing on hardware

After the build compiles clean, flash and watch the serial output.

**What to look for in serial output:**

```
[display] init ok          <-- display chain initialized, canvas allocated
[touch] GT911 init ok      <-- touch controller responded on I2C
[char] no characters installed  <-- normal if no GIF packs flashed
```

**Common failure modes:**

- **Black screen, no serial output:** Global object construction crashed before
  `setup()`. Check for PSRAM-dependent objects constructed at file scope.
- **Black screen, serial says "init ok":** The display initialized but
  `canvas->flush()` is not reaching the panel. Check RGB timing parameters,
  pin assignments, and whether `gfx->begin()` was called twice.
- **Display shows garbage:** RGB timing parameters (porch values, polarities)
  are wrong. Copy them exactly from the manufacturer demo code.
- **Touch does not respond:** Wrong I2C address or the touch controller reset
  was not released. Check the IO expander reset sequence.
- **Touch coordinates are offset:** The coordinate translation math is wrong.
  Print raw touch values to serial and verify against expected panel coordinates.

**BLE testing.** Once the display and touch work, BLE should work immediately --
the BLE stack has zero hardware dependencies beyond the ESP32's built-in radio.
Enable developer mode in Claude Desktop, open the Hardware Buddy window, and
pair.

## Lessons Learned

These are specific gotchas encountered during the 2.8-inch to 4B port.

### Global object construction timing with PSRAM

The ESP32-S3 RGB panel driver allocates its DMA framebuffer in PSRAM. PSRAM is
initialized by the Arduino runtime before `setup()` runs, but after C++ static
constructors execute. If you construct display objects as globals:

```cpp
// This crashes -- PSRAM is not ready during static init
Arduino_Canvas *canvas = new Arduino_Canvas(240, 320, gfx, 120, 80);
```

The fix is to declare globals as nullptr and construct them inside
`displayInit()`, which is called from `setup()`.

### Arduino_Canvas internally calls gfx->begin()

The plan originally had separate `gfx->begin()` and `canvas->begin()` calls.
On the ESP32-S3, `gfx->begin()` calls `esp_lcd_new_rgb_panel()`, which
allocates the single available RGB panel peripheral. When `canvas->begin()`
internally calls `gfx->begin()` again, the second allocation fails silently.
The display initializes but never shows anything.

The fix: call only `canvas->begin()` and let it handle `gfx->begin()`
internally.

### Static pointer initialization order

The `_tgt` pointer in `buddy.cpp` and `character.cpp` was originally
initialized to `&spr` (the sprite) or `canvas` at file scope. But `canvas` is
null at that point (see PSRAM issue above). The fix is lazy initialization:

```cpp
static Arduino_GFX* _tgt = nullptr;

void buddyTick(uint8_t personaState) {
  if (!_tgt) _tgt = canvas;   // canvas is valid by the time tick() runs
  // ...
}
```

### Arduino core version differences

The pioarduino platform fork provides newer Arduino core versions than the
stock Espressif platform. Arduino_GFX requires certain ESP-IDF APIs for the RGB
panel driver that are only present in newer cores. If your build fails with
missing symbol errors in `esp_lcd_panel_*` functions, you likely need a newer
platform version.

### Partition naming conventions

The partition table in `partitions.csv` names the filesystem partition `spiffs`
even though the firmware uses LittleFS. This is an ESP32 Arduino convention --
the partition name in the CSV does not determine which filesystem driver is used.
PlatformIO's `board_build.filesystem = littlefs` setting controls the
`uploadfs` target behavior, while the firmware code calls `LittleFS.begin()`
regardless of the partition name.

### Board definitions and PSRAM configuration

The `board_build.arduino.memory_type = qio_opi` setting must match your board's
actual PSRAM wiring. ESP32-S3 boards with octal PSRAM use `qio_opi`; boards
with quad PSRAM use `qio_qspi`. Using the wrong setting causes PSRAM to be
undetected, which causes display buffer allocation to fail, which causes a
black screen with no obvious error message.

## Architecture Overview

The codebase is designed to make porting straightforward by isolating hardware
dependencies.

### The sprite/canvas pattern

All rendering goes to a single off-screen buffer at the logical UI resolution
(240x320). The buffer is called `spr` (a `TFT_eSprite`) on TFT_eSPI branches
or `canvas` (an `Arduino_Canvas*`) on Arduino_GFX branches. A `blit()` function
in `main.cpp` pushes the buffer to the display hardware. On the 2.8-inch branch
this involves a byte-swap and raw SPI write; on the 4B branch it is a single
`canvas->flush()` call.

This pattern means that none of the UI code -- menus, info screens, approval
prompts, the clock, the HUD -- knows or cares what display controller is
attached.

### Hardware-specific code is isolated to three files

| File | Role |
|---|---|
| `src/display.h` / `src/display.cpp` | Display init, panel geometry, backlight, power stubs |
| `src/touch.h` / `src/touch.cpp` | Touch (or button) input, coordinate mapping |
| `src/compat.h` | Shim macros and helpers for library API differences |

Everything else is portable.

### Portable modules (no hardware dependencies)

| File | What it does |
|---|---|
| `src/ble_bridge.h/cpp` | Nordic UART BLE service, line-buffered TX/RX. Pure ESP32 BLE stack. |
| `src/data.h` | JSON wire protocol parser. |
| `src/stats.h` | NVS Preferences for settings, species choice, owner name. |
| `src/xfer.h` | LittleFS file transfer receiver for GIF character packs. |
| `src/clock.h` | POSIX software RTC. |
| `src/buddy_common.h` | Shared geometry constants for ASCII species layout. |
| `src/buddy.h/cpp` | ASCII species dispatch, render helpers, scale logic. |
| `src/character.h/cpp` | GIF decode and render via AnimatedGIF library. |
| `src/buddies/*.cpp` | 18 ASCII species, each with 7 animation state functions. |
| `characters/*` | GIF character pack assets. |

### The _tgt indirection pattern

`buddy.cpp` and `character.cpp` render through a `static Arduino_GFX* _tgt`
pointer that defaults to the sprite/canvas. The `buddyRenderTo()` and
`characterRenderTo()` functions temporarily retarget `_tgt` to a different
surface (e.g., the landscape clock renders the buddy at a different position and
scale). This indirection means species animation code and GIF rendering code
never reference the global sprite directly -- they draw through `_tgt`, and the
caller decides where pixels actually land.

## Reference: Hardware Comparison Table

| Aspect | M5StickC Plus | Waveshare 2.8" | Waveshare 4B |
|---|---|---|---|
| **SoC** | ESP32 (no -S3) | ESP32-S3 | ESP32-S3 |
| **Flash / PSRAM** | 4MB / 0 | 16MB / 8MB | 16MB / 8MB |
| **Display** | 135x240, ST7789, SPI | 240x320, ST7789, SPI | 480x480, ST7701, RGB parallel |
| **Display library** | TFT_eSPI (via M5) | TFT_eSPI | Arduino_GFX |
| **Pixel format** | RGB565 | RGB565 | RGB666 (canvas is RGB565) |
| **Touch** | None (buttons) | CST328 (I2C 0x1A) | GT911 (I2C 0x5D) |
| **Input** | 2 buttons + power | Capacitive touch | Capacitive touch |
| **IO Expander** | None | None | XCA9554 (I2C 0x20) |
| **Battery** | AXP192 PMIC | ADC on GPIO 8 | AXP2101 PMIC |
| **Power control** | AXP192 | GPIO 7 latch | AXP2101 |
| **Backlight** | AXP192 | GPIO 5 PWM | AXP2101 rail (always-on) |
| **IMU** | MPU6886 | None | QMI8658 (available) |
| **RTC** | BM8563 | Software only | PCF85063 (available) |
| **Audio** | Built-in buzzer | None | ES8311 DAC + ES7210 ADC |
| **BLE** | BLE 4.2 | BLE 5 | BLE 5 |

## Reference: File Change Map

### Files you rewrite (new hardware drivers)

| File | What changes |
|---|---|
| `src/display.h` | Panel geometry defines, display object declarations, API prototypes |
| `src/display.cpp` | Entire initialization sequence, pin assignments, driver chain |
| `src/touch.h` | Comment updates (same function signatures) |
| `src/touch.cpp` | Touch IC driver, I2C address, coordinate translation |
| `platformio.ini` | Board, platform, library dependencies |

### Files you create

| File | Purpose |
|---|---|
| `src/compat.h` | Color macros, text helpers, any API shims needed between display libraries |

### Files you edit mechanically (type and syntax substitutions)

| File | Nature of changes |
|---|---|
| `src/main.cpp` | Include swap, new globals, battery/power stubs, `fillSprite`->`fillScreen`, `setTextDatum`/`drawString`->`drawCenteredString`, blit rewrite |
| `src/buddy.h` | Forward declare new display base class |
| `src/buddy.cpp` | Include swap, extern type, `TFT_eSPI*`->`Arduino_GFX*`, `spr.`->`canvas->` |
| `src/character.h` | Forward declare new display base class |
| `src/character.cpp` | Include swap, extern type, `TFT_eSPI*`->`Arduino_GFX*`, `spr.`->`canvas->` |
| `src/buddies/*.cpp` (x18) | Include swap, extern type, `spr.`->`canvas->` |

### Files that do not change at all

| File | Why it is portable |
|---|---|
| `src/ble_bridge.h/cpp` | Pure ESP32 BLE, no display or touch references |
| `src/data.h` | JSON parser, no hardware calls |
| `src/stats.h` | NVS Preferences only |
| `src/xfer.h` | LittleFS file transfer, no display calls |
| `src/clock.h` | POSIX time functions |
| `src/buddy_common.h` | Constants and function prototypes, no includes of display headers |
| `partitions.csv` | Same flash layout works for any ESP32 with 16MB flash |
| `characters/*` | GIF assets, pure data |
