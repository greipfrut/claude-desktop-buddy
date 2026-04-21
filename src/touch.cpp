#include "touch.h"
#include <Arduino.h>
#include <Wire.h>

static constexpr uint8_t  CST328_ADDR    = 0x1A;
static constexpr int      PIN_SDA        = 1;
static constexpr int      PIN_SCL        = 3;
static constexpr int      PIN_INT        = 4;
static constexpr int      PIN_RST        = 2;
static constexpr uint32_t I2C_FREQ       = 400000;

static constexpr uint16_t REG_NUM        = 0xD005;
static constexpr uint16_t REG_XY         = 0xD000;
static constexpr uint16_t REG_BOOT_TIME  = 0xD1FC;
static constexpr uint16_t REG_NORMAL     = 0xD109;
static constexpr uint16_t REG_DEBUG_INFO = 0xD101;

static volatile bool _touched = false;

static void IRAM_ATTR touchISR() { _touched = true; }

static bool i2cRead(uint16_t reg, uint8_t* buf, size_t len) {
  Wire1.beginTransmission(CST328_ADDR);
  Wire1.write((uint8_t)(reg >> 8));
  Wire1.write((uint8_t)reg);
  if (Wire1.endTransmission(true)) return false;
  size_t got = Wire1.requestFrom((int)CST328_ADDR, (int)len);
  for (size_t i = 0; i < got; i++) buf[i] = Wire1.read();
  return got == len;
}

static bool i2cWrite(uint16_t reg, const uint8_t* buf, size_t len) {
  Wire1.beginTransmission(CST328_ADDR);
  Wire1.write((uint8_t)(reg >> 8));
  Wire1.write((uint8_t)reg);
  for (size_t i = 0; i < len; i++) Wire1.write(buf[i]);
  return Wire1.endTransmission(true) == 0;
}

bool touchInit() {
  Wire1.begin(PIN_SDA, PIN_SCL, I2C_FREQ);
  pinMode(PIN_INT, INPUT);
  pinMode(PIN_RST, OUTPUT);

  digitalWrite(PIN_RST, HIGH); delay(50);
  digitalWrite(PIN_RST, LOW);  delay(5);
  digitalWrite(PIN_RST, HIGH); delay(50);

  uint8_t buf[4];
  i2cWrite(REG_DEBUG_INFO, buf, 0);
  bool ok = i2cRead(REG_BOOT_TIME, buf, 4);
  i2cWrite(REG_NORMAL, buf, 0);

  attachInterrupt(PIN_INT, touchISR, RISING);
  return ok;
}

bool touchGetPoint(uint16_t* x, uint16_t* y) {
  if (!_touched) return false;
  _touched = false;

  uint8_t buf[28];
  if (!i2cRead(REG_NUM, buf, 1)) return false;
  uint8_t n = buf[0] & 0x0F;
  uint8_t clear = 0;
  if (n == 0) {
    i2cWrite(REG_NUM, &clear, 1);
    return false;
  }
  if (!i2cRead(REG_XY, buf + 1, 27)) return false;
  i2cWrite(REG_NUM, &clear, 1);

  *x = (uint16_t)(((uint16_t)buf[2] << 4) | ((buf[4] & 0xF0) >> 4));
  *y = (uint16_t)(((uint16_t)buf[3] << 4) |  (buf[4] & 0x0F));
  return true;
}
