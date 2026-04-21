#pragma once
#include <stdint.h>
#include <time.h>
#include <sys/time.h>

// Software replacement for M5.Rtc backed by POSIX time.h. Type names
// mirror the M5 header so call sites need no further changes.
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
