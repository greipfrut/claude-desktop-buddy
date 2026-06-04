#include "display.h"
#include "compat.h"
#include <Wire.h>

// ── Hardware objects ───────────────────────────────────────────────────
// XCA9554 IO expander: provides software SPI for ST7701 init commands.
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
