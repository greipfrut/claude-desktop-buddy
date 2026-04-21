#pragma once
#include <stdint.h>

// CST328 capacitive touch driver for the Waveshare ESP32-S3 Touch LCD 2.8.
// Wire1 I2C, pins SDA=1 SCL=3, INT=4, RST=2. Returns raw coords matching
// the native portrait orientation (x 0..239, y 0..319).
bool touchInit();
bool touchGetPoint(uint16_t* x, uint16_t* y);
