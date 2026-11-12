// PCF85063A real-time clock (I2C 0x51 on the shared Wire bus). Read-only for now: nothing
// sets it yet (NTP comes with Wi-Fi), so the time is whatever was last written to it.
//
// The RTC holds local wall-clock time with no zone. The system clock is run as UTC with the
// same wall time, so localtime()/gmtime() give the RTC's reading.
#pragma once

namespace rtc {

// Reads the RTC (one burst, as the datasheet requires) and, if it holds a time that was
// actually set (OS flag clear and not the 2000-01-01 reset epoch), sets the system clock from
// it. Begin Wire first. Returns timeValid().
bool begin();

// True once the system clock holds a real time.
bool timeValid();

// The alarm registers (read-only). armed = Control_2 AIE; fired = AF (INT is held low, and on
// this board that holds the PMIC's PWRON low too). Fields the alarm doesn't compare are -1.
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
