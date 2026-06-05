#pragma once
#include <stdint.h>
#include <time.h>
#include <sys/time.h>

struct RTC_TimeTypeDef {
  uint8_t Hours;
  uint8_t Minutes;
  uint8_t Seconds;
};

struct RTC_DateTypeDef {
  uint8_t  WeekDay;
  uint8_t  Month;
  uint8_t  Date;
  uint16_t Year;
};

// Feed with (utc_epoch + tz_offset_seconds) from the bridge. We store it
// as if it were UTC so gmtime_r returns already-adjusted local fields,
// avoiding newlib's tz database dependency.
inline void clockSetFromLocal(time_t localEpoch) {
  struct timeval tv = { localEpoch, 0 };
  settimeofday(&tv, nullptr);
}

inline void clockGetTime(RTC_TimeTypeDef* out) {
  time_t now; time(&now);
  struct tm lt; gmtime_r(&now, &lt);
  out->Hours   = (uint8_t)lt.tm_hour;
  out->Minutes = (uint8_t)lt.tm_min;
  out->Seconds = (uint8_t)lt.tm_sec;
}

inline void clockGetDate(RTC_DateTypeDef* out) {
  time_t now; time(&now);
  struct tm lt; gmtime_r(&now, &lt);
  out->WeekDay = (uint8_t)lt.tm_wday;
  out->Month   = (uint8_t)(lt.tm_mon + 1);
  out->Date    = (uint8_t)lt.tm_mday;
  out->Year    = (uint16_t)(lt.tm_year + 1900);
}

// ── PCF85063 hardware RTC (persists across reboots) ────────────────────
#include <Wire.h>
#include "SensorPCF85063.hpp"

static SensorPCF85063 _hwRtc;
static bool _hwRtcOk = false;

inline bool hwRtcInit() {
  _hwRtcOk = _hwRtc.begin(Wire, PCF85063_SLAVE_ADDRESS, 47, 48);
  if (!_hwRtcOk) { Serial.println("[rtc] PCF85063 not found"); return false; }
  Serial.println("[rtc] PCF85063 ready");
  return true;
}

inline bool hwRtcLoad() {
  if (!_hwRtcOk) return false;
  RTC_DateTime dt = _hwRtc.getDateTime();
  if (dt.year < 2025) return false;
  struct tm t = {};
  t.tm_year = dt.year - 1900;
  t.tm_mon  = dt.month - 1;
  t.tm_mday = dt.day;
  t.tm_hour = dt.hour;
  t.tm_min  = dt.minute;
  t.tm_sec  = dt.second;
  time_t epoch = mktime(&t);
  clockSetFromLocal(epoch);
  Serial.printf("[rtc] loaded %04d-%02d-%02d %02d:%02d:%02d\n",
    dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second);
  return true;
}

inline void hwRtcSave(time_t localEpoch) {
  if (!_hwRtcOk) return;
  struct tm lt; gmtime_r(&localEpoch, &lt);
  _hwRtc.setDateTime(
    lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday,
    lt.tm_hour, lt.tm_min, lt.tm_sec);
  Serial.printf("[rtc] saved %04d-%02d-%02d %02d:%02d:%02d\n",
    lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday,
    lt.tm_hour, lt.tm_min, lt.tm_sec);
}
