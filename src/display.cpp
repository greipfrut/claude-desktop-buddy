#include "display.h"
#include "compat.h"
#include <Wire.h>

// ── Hardware objects ───────────────────────────────────────────────────
// Pointers are null until displayInit() — the RGB panel driver calls
// esp_lcd_new_rgb_panel() which requires PSRAM to be up, so we cannot
// construct these at global-init time.
Arduino_XCA9554SWSPI *expander = nullptr;
Arduino_ESP32RGBPanel *rgbpanel = nullptr;
Arduino_RGB_Display  *gfx      = nullptr;
Arduino_Canvas       *canvas   = nullptr;

// ── Init ───────────────────────────────────────────────────────────────
void displayInit() {
  // Start I2C bus shared by expander, touch, and all other peripherals
  Wire.begin(47 /* SDA */, 48 /* SCL */);

  // Construct display objects now that the runtime is ready
  expander = new Arduino_XCA9554SWSPI(
      7  /* RST */,
      0  /* CS  */,
      2  /* MOSI */,
      1  /* SCK */,
      &Wire,
      0x20 /* XCA9554 I2C address */);

  rgbpanel = new Arduino_ESP32RGBPanel(
      17 /* DE */, 3 /* VSYNC */, 46 /* HSYNC */, 9 /* PCLK */,
      10 /* B0 */, 11 /* B1 */, 12 /* B2 */, 13 /* B3 */, 14 /* B4 */,
      21 /* G0 */,  8 /* G1 */, 18 /* G2 */, 45 /* G3 */, 38 /* G4 */, 39 /* G5 */,
      40 /* R0 */, 41 /* R1 */, 42 /* R2 */,  2 /* R3 */,  1 /* R4 */,
      1  /* hsync_polarity */,  10 /* hsync_front_porch */,
      8  /* hsync_pulse_width */, 50 /* hsync_back_porch */,
      1  /* vsync_polarity */,  10 /* vsync_front_porch */,
      8  /* vsync_pulse_width */, 20 /* vsync_back_porch */);

  gfx = new Arduino_RGB_Display(
      PANEL_WIDTH, PANEL_HEIGHT, rgbpanel,
      0 /* rotation */, true /* auto_flush */,
      expander, GFX_NOT_DEFINED /* RST */,
      st7701_type1_init_operations,
      sizeof(st7701_type1_init_operations));

  canvas = new Arduino_Canvas(
      LCD_WIDTH, LCD_HEIGHT, gfx,
      CANVAS_OFF_X, CANVAS_OFF_Y);

  // Reset sequence via IO expander (from 4B demo code)
  expander->pinMode(5, OUTPUT);    // LCD RST
  expander->pinMode(6, OUTPUT);    // Touch RST
  expander->digitalWrite(6, LOW);  // Assert touch reset
  delay(200);
  expander->digitalWrite(5, LOW);  // Assert LCD reset
  delay(200);
  expander->digitalWrite(5, HIGH); // Release LCD reset
  delay(200);

  // Init the canvas — this internally calls gfx->begin() which sends the
  // ST7701 init commands via software SPI and allocates the RGB panel.
  // Do NOT call gfx->begin() separately — ESP32-S3 only has 1 RGB panel
  // slot, and a double begin() would exhaust it.
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
