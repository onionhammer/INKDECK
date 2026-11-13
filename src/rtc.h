// PCF85063A real-time clock (I2C 0x51 on the shared Wire bus).
//
// The RTC holds UTC, and so does the system clock. Local time comes from the time zone (a POSIX
// TZ string: `timezone` in SD:/inkdeck.ini, else TIMEZONE from .env; settings.h), which begin()
// applies, so localtime() follows daylight saving. The vendor firmware keeps local time in the RTC
// instead, with no DST. Alarm registers therefore compare UTC too.
#pragma once
#include <time.h>

namespace rtc {

// Applies the time zone, then reads the RTC (one burst, as the datasheet requires) and, if it
// holds a time that was actually set (OS flag clear and not the 2000-01-01 reset epoch), sets
// the system clock from it. Begin Wire first. Returns timeValid().
bool begin();

// True once the system clock holds a real time (from the RTC at boot, or from set()).
bool timeValid();

// Writes a UTC time to the RTC in one burst, which also clears the oscillator-stop flag.
// It doesn't set the system clock (SNTP already has), but it marks the time valid. Safe to call
// from any task (Wire locks the bus).
bool set(time_t utc);

// Testing: puts the RTC back to its power-loss reset value, 2000-01-01 00:00, which begin()
// treats as unset, as if the battery had been out. The system clock keeps running.
bool forget();

// Wake alarm: compares day of month, hour, minute and second (UTC), so it fires once, at
// `utc`, which must be in the future and within the month (a time that has passed fires next
// month). Sets AIE and clears AF. While armed and fired, INT holds the PMIC's PWRON low, which
// powers the board on from off, but forces it off after 4 s if it is running: arm it only on
// the way to a power-off, and disarm() early at boot.
bool arm(time_t utc);
// AIE off, AF cleared (releases INT), every alarm field disabled.
bool disarm();

// The alarm registers (read-only). armed = Control_2 AIE; fired = AF (INT is held low, and on
// this board that holds the PMIC's PWRON low too). Fields the alarm doesn't compare are -1.
// The times are UTC.
struct Alarm {
  bool ok = false;  // the RTC answered
  bool armed = false;
  bool fired = false;
  int day = -1;
  int hour = -1;
  int minute = -1;
  int second = -1;
};
Alarm alarm();

}  // namespace rtc
