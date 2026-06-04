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
