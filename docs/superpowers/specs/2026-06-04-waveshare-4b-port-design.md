# Claude Desktop Buddy: Port to Waveshare ESP32-S3-Touch-LCD-4B

**Date:** 2026-06-04
**Branch:** `waveshare-s3-touch-lcd-4b` (forked from `waveshare-s3-touch-28`)
**Repo:** https://github.com/greipfrut/claude-desktop-buddy

## Summary

Port the claude-desktop-buddy firmware from the Waveshare ESP32-S3-Touch-LCD-2.8
(240x320, ST7789 SPI, CST328 touch) to the Waveshare ESP32-S3-Touch-LCD-4B
(480x480, ST7701 RGB parallel, GT911 touch, AXP2101 PMIC, QMI8658 IMU,
PCF85063 RTC, ES8311/ES7210 audio).

**Approach:** Minimal viable port. Render the existing 240x320 UI centered on
the 480x480 screen with black borders. Swap hardware drivers only. Stub all
peripherals not present on the 2.8" (IMU, RTC, audio, PMIC battery
management). Get BLE + buddy working as quickly as possible; polish and
peripheral integration come later.

**Display library strategy:** Full swap from TFT_eSPI to Arduino_GFX
(GFX Library for Arduino). This is the same display stack proven in the 4B's
manufacturer demo code.

## Hardware Comparison

| Aspect | 2.8" (source branch) | 4B (target) |
|---|---|---|
| Display | 240x320, ST7789, SPI (FSPI, 4 GPIOs) | 480x480, ST7701, RGB parallel (16 data + 4 sync GPIOs) + software-SPI via XCA9554 for init |
| Pixel format | RGB565 (16-bit) | RGB666 (18-bit) |
| Touch IC | CST328 (addr 0x1A) | GT911 (addr 0x5D) |
| Touch I2C | Wire1 (SDA:1, SCL:3) | Wire (SDA:47, SCL:48) — shared bus |
| Touch RST/IRQ | Direct GPIOs (RST:2, INT:4) | RST via XCA9554 pin 6, IRQ not direct |
| IO Expander | None | XCA9554 at I2C 0x20 |
| Battery | ADC on GPIO 8, x3 divider | AXP2101 PMIC at I2C 0x34 |
| Power control | GPIO 7 latch (hold HIGH) | AXP2101 PMIC |
| Backlight | GPIO 5 PWM | Always-on (AXP2101 rail) |
| IMU | None (removed from M5 port) | QMI8658 (available, not used in MVP) |
| RTC | Software (POSIX time) | PCF85063 (available, not used in MVP) |
| Audio | None | ES8311 DAC + ES7210 ADC (not used in MVP) |
| ESP32-S3 | 16MB flash, 8MB PSRAM | 16MB flash, 8MB PSRAM (identical) |
| BLE | BLE 5 | BLE 5 (identical) |

## Architecture

### Rendering pipeline

The existing codebase has a clean separation: all drawing goes into a
TFT_eSprite buffer (240x320 RGB565), and a single `blit()` function in
main.cpp pushes that buffer to the display hardware. The GIF renderer, all 18
ASCII buddy species, BLE protocol, and every UI overlay write into the sprite
through the drawing API — they have zero direct hardware calls.

**Current pipeline (2.8"):**
```
TFT_eSprite spr(240x320) --> blit() byte-swaps --> hand-rolled SPI --> ST7789
```

**New pipeline (4B):**
```
Arduino_Canvas *spr(240x320, offset 120,80) --> spr->flush() --> DMA --> RGB panel (480x480)
```

The Arduino_Canvas constructor takes `output_x=120, output_y=80` parameters
that center the 240x320 canvas on the 480x480 panel. `flush()` writes into
the correct region of the DMA framebuffer automatically. The surrounding area
stays black.

### Display driver replacement (display.h / display.cpp)

Replace the entire ST7789 SPI driver with the Arduino_GFX display chain from
the 4B demo code (`01_HelloWorld.ino`):

```
Wire (SDA:47, SCL:48)
  --> XCA9554 IO expander (addr 0x20)
    --> Software SPI (pins 0=CS, 1=SCK, 2=MOSI) for ST7701 init
    --> Pin 5 = LCD RST, Pin 6 = Touch RST
  --> Arduino_ESP32RGBPanel (16-bit RGB parallel data bus)
    --> Arduino_RGB_Display (480x480, st7701_type1_init_operations)
      --> Arduino_Canvas (240x320, offset 120,80) = our sprite buffer
```

RGB parallel pin assignments (from 4B demo code):
- DE:17, VSYNC:3, HSYNC:46, PCLK:9
- Red: R0=40, R1=41, R2=42, R3=2, R4=1
- Green: G0=21, G1=8, G2=18, G3=45, G4=38, G5=39
- Blue: B0=10, B1=11, B2=12, B3=13, B4=14

RGB timing: hsync_front_porch=10, hsync_pulse_width=8, hsync_back_porch=50,
vsync_front_porch=10, vsync_pulse_width=8, vsync_back_porch=20. All
polarities active high.

XCA9554 reset sequence (from demo code):
1. expander->pinMode(5, OUTPUT)  // LCD RST
2. expander->pinMode(6, OUTPUT)  // Touch RST
3. expander->digitalWrite(6, LOW)  // Assert touch reset
4. delay(200)
5. expander->digitalWrite(5, LOW)  // Assert LCD reset
6. delay(200)
7. expander->digitalWrite(5, HIGH) // Release LCD reset
8. delay(200)

### Touch driver replacement (touch.h / touch.cpp)

Replace the custom CST328 I2C driver with GT911 via the SensorLib library
(`TouchDrvGT911`).

Public API remains identical:
```cpp
bool touchInit();                                // Wire.begin + GT911.begin
bool touchGetPoint(uint16_t* x, uint16_t* y);   // true + canvas-space coords
```

GT911 setup (from 4B demo `06_LVGL_Arduino_v9.ino`):
```cpp
Wire.begin(47, 48);
GT911.setPins(-1, -1);           // No direct RST/IRQ GPIO
GT911.begin(Wire, GT911_SLAVE_ADDRESS_L, 47, 48);  // addr 0x5D
GT911.setMaxTouchPoint(1);
```

Coordinate translation: GT911 returns 0-479 x 0-479. The canvas is at
offset (120, 80). Translation:
- canvasX = touchX - 120
- canvasY = touchY - 80
- Reject touches where canvasX < 0 or >= 240 or canvasY < 0 or >= 320

This keeps all existing touch logic in main.cpp (which thinks in 240x320
space) working without changes.

### TFT_eSPI to Arduino_GFX API translation

The drawing API is nearly identical between TFT_eSPI and Arduino_GFX.
Translation is mechanical:

| TFT_eSPI | Arduino_GFX | Notes |
|---|---|---|
| `TFT_eSprite spr(&tft)` | `Arduino_Canvas *spr = new Arduino_Canvas(W, H, gfx, offX, offY)` | Heap-allocated |
| `spr.createSprite(w, h)` | `spr->begin()` | Allocation at begin() |
| `spr.pushSprite(0, 0)` | `spr->flush()` | DMA write to panel |
| `spr.drawPixel(x,y,c)` | `spr->drawPixel(x,y,c)` | Identical |
| `spr.fillRect(x,y,w,h,c)` | `spr->fillRect(x,y,w,h,c)` | Identical |
| `spr.fillScreen(c)` | `spr->fillScreen(c)` | Identical |
| `spr.setTextColor(c)` | `spr->setTextColor(c)` | Identical |
| `spr.setTextColor(c,bg)` | `spr->setTextColor(c,bg)` | Identical |
| `spr.setCursor(x,y)` | `spr->setCursor(x,y)` | Identical |
| `spr.setTextSize(s)` | `spr->setTextSize(s)` | Identical |
| `spr.print(text)` | `spr->print(text)` | Identical |
| `spr.getPointer()` | `spr->getFramebuffer()` | Returns uint16_t* |
| `spr.width()` / `spr.height()` | `spr->width()` / `spr->height()` | Identical |
| `TFT_eSPI*` (render target) | `Arduino_GFX*` | Base class for _tgt pointer |

Two patterns require special handling:

**1. Value vs pointer:** All `spr.method()` calls become `spr->method()` since
Arduino_Canvas is heap-allocated (required by the Arduino_GFX library design).

**2. setTextDatum + drawString:** Arduino_GFX does not have `setTextDatum()` for
text alignment. The ~15 call sites in main.cpp (clock screen, approval
buttons, boot splash) use `MC_DATUM` (middle-center) alignment. We provide a
helper in `compat.h`:

```cpp
void drawCenteredString(Arduino_GFX *gfx, const char *s, int16_t x, int16_t y) {
    int16_t x1, y1;
    uint16_t w, h;
    gfx->getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
    gfx->setCursor(x - w / 2, y - h / 2);
    gfx->print(s);
}
```

**3. Color constants:** A `compat.h` header provides macros mapping TFT_eSPI
names to Arduino_GFX names:

```cpp
#define TFT_BLACK   BLACK
#define TFT_WHITE   WHITE
#define TFT_RED     RED
// etc.
```

### Power, battery, and backlight (MVP stubs)

All stubbed for MVP:

- **Battery:** `batteryMilliVolts()` returns 4200, `batteryUsbPresent()` returns
  true. Disables auto-screen-off and shows "plugged in, full" at all times.
- **Power off:** `powerOff()` calls `ESP.restart()`. Real power-off requires
  AXP2101 PMIC commands (future enhancement).
- **Backlight:** Always on at full brightness. `Set_Backlight()` is a no-op.
  The 4B backlight is powered by an AXP2101 rail that is enabled at boot.
  Brightness control would require PMIC integration (future enhancement).

### AnimatedGIF library compatibility

The AnimatedGIF library in `character.cpp` calls `gif.begin(LITTLE_ENDIAN_PIXELS)`
and renders via `_tgt->drawPixel()`. Since Arduino_GFX Canvas stores pixels in
little-endian RGB565 (same as TFT_eSprite), the GIF rendering pipeline works
without changes. The `gifDrawCb` callback writes pixels through the
`Arduino_GFX*` `_tgt` pointer, which has the same `drawPixel(x, y, color565)`
signature.

### New file: src/compat.h

Small compatibility header to minimize diffs across the codebase:
- TFT_eSPI color constant macros (TFT_BLACK -> BLACK, etc.)
- `drawCenteredString()` helper function
- Any other one-off shims discovered during implementation

### Files unchanged

These files require zero modifications:
- `src/ble_bridge.h` / `src/ble_bridge.cpp` — Pure ESP32 BLE stack
- `src/data.h` — JSON state machine
- `src/stats.h` — NVS Preferences
- `src/xfer.h` — LittleFS file transfer
- `src/clock.h` — POSIX software RTC
- `src/buddy_common.h` — Geometry externs
- `partitions.csv` — Same flash layout
- `characters/*` — GIF assets

### Build configuration (platformio.ini)

New environment `[env:waveshare-s3-touch-lcd-4b]`:
- Board: `esp32-s3-devkitc-1` (generic S3 board def)
- Framework: Arduino
- PSRAM: `-DBOARD_HAS_PSRAM`, `board_build.arduino.memory_type = qio_opi`
- USB: `-DARDUINO_USB_CDC_ON_BOOT=1`
- Libraries: `moononournation/GFX Library for Arduino`,
  `lewisxhe/SensorLib`, `bitbank2/AnimatedGIF @ ^2.1.1`,
  `bblanchon/ArduinoJson @ ^7.0.0`
- Filesystem: LittleFS
- Partitions: `partitions.csv` (unchanged)
- Remove: all TFT_eSPI `-D` flags and TFT_eSPI library dependency

## File Change Map

### Rewritten (new hardware drivers)

| File | Description |
|---|---|
| `src/display.h` | Pin defines, Arduino_GFX globals, stub power/backlight API |
| `src/display.cpp` | XCA9554 + ESP32RGBPanel + ST7701 init, canvas flush, stubs |
| `src/touch.h` | Same public API, GT911 types |
| `src/touch.cpp` | GT911 via SensorLib, coordinate translation |
| `platformio.ini` | New env, library swap, PSRAM config |

### Mechanically edited (API translation)

| File | Changes |
|---|---|
| `src/main.cpp` | New display globals, stub battery/power, `spr.`->`spr->`, drawCenteredString, remove blit byte-swap |
| `src/buddy.cpp` | Include swap, extern type, `.`->`->`, `TFT_eSPI*`->`Arduino_GFX*` |
| `src/buddy.h` | Forward declare Arduino_GFX |
| `src/character.cpp` | Include swap, extern type, `.`->`->`, `TFT_eSPI*`->`Arduino_GFX*` |
| `src/character.h` | Forward declare Arduino_GFX |
| `src/buddies/*.cpp` (x18) | Include swap, extern type, `spr.`->`spr->` |

### New files

| File | Purpose |
|---|---|
| `src/compat.h` | Color macros, drawCenteredString helper |

## Implementation Order

### Phase 1: Compile (no hardware needed)

1. Create branch `waveshare-s3-touch-lcd-4b` from `waveshare-s3-touch-28`
2. Rewrite `platformio.ini`
3. Create `src/compat.h`
4. Rewrite `src/display.h` + `src/display.cpp`
5. Rewrite `src/touch.h` + `src/touch.cpp`
6. Edit `src/main.cpp`
7. Edit `src/buddy.cpp/h`, `src/character.cpp/h`
8. Bulk edit 18 `src/buddies/*.cpp`
9. Verify: `pio run` compiles clean

### Phase 2: Flash and boot (needs 4B board)

1. `pio run -t upload` — flash firmware
2. Check serial output for boot messages
3. Verify display shows the buddy UI
4. Verify touch responds
5. Pair via BLE from Claude desktop

### Phase 3: Polish (iterate)

1. Adjust canvas offset if centering looks wrong
2. Tweak touch coordinate mapping
3. Test GIF character rendering
4. Test all screens (info, menu, approval, clock)

## Future Enhancements (post-MVP)

These are explicitly out of scope for the initial port but documented for
later work:

- **AXP2101 PMIC integration:** Real battery percentage, charging status,
  USB detection, backlight brightness control, proper power-off
- **QMI8658 IMU:** Re-enable shake/tilt gestures from the original M5StickC
  code (face-down nap, shake dizzy, auto-rotate)
- **PCF85063 RTC:** Hardware real-time clock so time persists across reboots
  without the desktop bridge
- **ES8311/ES7210 audio:** Beep feedback on approval, notification sounds,
  potentially voice interaction
- **480x480 native layout:** Redesign UI to use the full square display
  instead of centering 240x320
- **Scale-up mode:** Render at 480x480 with proportionally larger characters
  and text

## Reference Paths

- Buddy repo: `C:\Users\tsphan-ahc\Development\claude-desktop-buddy`
- 4B demo code: `C:\Users\tsphan-ahc\Downloads\ESP32-S3-Touch-LCD-4B-Demos`
- Arduino demos (pin mappings, display init): `...\Arduino-v3.2.0\examples\`
- PlatformIO: `C:\Users\tsphan-ahc\.platformio\penv\Scripts\pio.exe`
- Fork: https://github.com/greipfrut/claude-desktop-buddy
- Upstream: https://github.com/FradSer/claude-desktop-buddy
