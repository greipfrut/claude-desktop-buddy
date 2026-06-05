#pragma once
#include <Arduino.h>

// AXP2101 PMIC battery + power module (Waveshare ESP32-S3-Touch-LCD-4B).
// Owns the single XPowersPMU instance. All reads are cached: call powerPoll()
// on a slow timer from loop(); the getters return the cached snapshot so no
// I2C happens in the render hot path. On init failure the getters return safe
// defaults and powerOff() falls back to a restart.

bool powerInit();              // PMIC begin on shared Wire (SDA47/SCL48); enable ADCs. No rail changes.
void powerPoll();              // refresh cached readings; call ~every 3 s from loop()

int  batteryMilliVolts();      // battery voltage, mV
int  batteryPercent();         // 0..100 (PMIC gauge, LiPo voltage-curve fallback)
int  batteryMilliAmps();       // best-effort; 0 if the PMIC doesn't expose it
bool batteryUsbPresent();      // VBUS present
int  batteryUsbMilliVolts();   // VBUS voltage, mV
bool batteryCharging();        // actively charging
bool batteryFull();            // charge complete
void powerOff();               // real AXP2101 shutdown (wakes on PWRKEY / USB)
