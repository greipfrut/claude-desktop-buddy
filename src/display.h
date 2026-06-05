#pragma once
#include <Arduino.h>
#include <Arduino_GFX_Library.h>

// ── Panel geometry ─────────────────────────────────────────────────────
#define PANEL_WIDTH   480
#define PANEL_HEIGHT  480

// Canvas (sprite) dimensions — full native 480x480 panel
#define LCD_WIDTH     480
#define LCD_HEIGHT    480

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
void backlightInit();              // LEDC PWM setup on GPIO 4; call after displayInit()
void Set_Backlight(uint8_t pct);   // 0 = off, 1..100 = brightness via PWM
