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

constexpr uint8_t kRegControl2 = 0x01;  // bit 7 AIE (alarm interrupt on), bit 6 AF (fired)
constexpr uint8_t kRegSeconds = 0x04;  // 0x04..0x0A: s, min, h, day, weekday, month, year
constexpr uint8_t kRegAlarm = 0x0B;    // 0x0B..0x0E: s, min, h, day; bit 7 set = not compared
constexpr int kMinValidYear = 2025;     // anything earlier was never set (see begin())

bool valid = false;

int bcd(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }

bool read(uint8_t reg, uint8_t *out, uint8_t n) {
  Wire.beginTransmission(board::I2C_ADDR_PCF85063);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(board::I2C_ADDR_PCF85063, n) != n) return false;
  for (uint8_t i = 0; i < n; ++i) out[i] = Wire.read();
  return true;
}

}  // namespace

bool begin() {
  setenv("TZ", "UTC0", 1);  // see rtc.h: wall time is kept as UTC
  tzset();

  uint8_t r[7];
  if (!read(kRegSeconds, r, sizeof r)) {
    log_e("RTC: no response at 0x%02X", board::I2C_ADDR_PCF85063);
    return false;
  }

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

Alarm alarm() {
  Alarm a;
  uint8_t ctrl, r[4];
  if (!read(kRegControl2, &ctrl, 1) || !read(kRegAlarm, r, sizeof r)) return a;
  a.ok = true;
  a.armed = ctrl & 0x80;
  a.fired = ctrl & 0x40;
  static constexpr uint8_t kMask[] = {0x7F, 0x7F, 0x3F, 0x3F};
  int *fields[] = {&a.second, &a.minute, &a.hour, &a.day};
  for (int i = 0; i < 4; ++i) {
    if (!(r[i] & 0x80)) *fields[i] = bcd(r[i] & kMask[i]);
  }
  return a;
}

}  // namespace rtc
