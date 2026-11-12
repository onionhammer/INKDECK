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

}  // namespace rtc
