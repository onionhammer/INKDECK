// Registers: .claude/skills/waveshare-epaper-reference/references/power.md §3 and its porting
// notes (burst read, check OS).
#include "rtc.h"

#include <Arduino.h>
#include <Wire.h>
#include <sys/time.h>
#include <time.h>

#include "board_pins.h"

namespace rtc {
namespace {

constexpr uint8_t kRegSeconds = 0x04;  // 0x04..0x0A: s, min, h, day, weekday, month, year
constexpr int kMinValidYear = 2025;     // anything earlier was never set (see begin())

bool valid = false;

int bcd(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }

}  // namespace

bool begin() {
  setenv("TZ", "UTC0", 1);  // see rtc.h: wall time is kept as UTC
  tzset();

  uint8_t r[7];
  Wire.beginTransmission(board::I2C_ADDR_PCF85063);
  Wire.write(kRegSeconds);
  if (Wire.endTransmission(false) != 0 ||
      Wire.requestFrom(board::I2C_ADDR_PCF85063, static_cast<uint8_t>(sizeof r)) != sizeof r) {
    log_e("RTC: no response at 0x%02X", board::I2C_ADDR_PCF85063);
    return false;
  }
  for (auto &b : r) b = Wire.read();

  if (r[0] & 0x80) {
    log_w("RTC: oscillator-stop flag set, time not valid (needs setting)");
    return false;
  }
  tm t = {};
  t.tm_sec = bcd(r[0] & 0x7F);
  t.tm_min = bcd(r[1] & 0x7F);
  t.tm_hour = bcd(r[2] & 0x3F);
  t.tm_mday = bcd(r[3] & 0x3F);
  t.tm_mon = bcd(r[5] & 0x1F) - 1;
  t.tm_year = 100 + bcd(r[6]);
  if (t.tm_sec > 59 || t.tm_min > 59 || t.tm_hour > 23 || t.tm_mday < 1 || t.tm_mon < 0 ||
      t.tm_mon > 11) {
    log_w("RTC: implausible time, ignoring");
    return false;
  }
  // Verified on hardware: after losing power the RTC restarts at its reset value, 2000-01-01,
  // with OS clear, so the flag alone doesn't prove the time was ever set.
  if (t.tm_year + 1900 < kMinValidYear) {
    log_w("RTC: %04d-%02d-%02d %02d:%02d is before %d, time not set", t.tm_year + 1900,
          t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, kMinValidYear);
    return false;
  }
  const timeval tv = {mktime(&t), 0};
  settimeofday(&tv, nullptr);
  valid = true;
  log_i("RTC: %04d-%02d-%02d %02d:%02d:%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
        t.tm_hour, t.tm_min, t.tm_sec);
  return true;
}

bool timeValid() { return valid; }

}  // namespace rtc
