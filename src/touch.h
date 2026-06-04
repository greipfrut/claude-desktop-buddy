#pragma once
#include <stdint.h>

// GT911 capacitive touch driver for the Waveshare ESP32-S3-Touch-LCD-4B.
// I2C on Wire (SDA=47, SCL=48), addr 0x5D. RST via XCA9554 pin 6.
// Returns coordinates mapped to the 240x320 canvas space (NOT native
// 480x480 panel coords). Touches outside the canvas region are rejected.
bool touchInit();
bool touchGetPoint(uint16_t* x, uint16_t* y);
