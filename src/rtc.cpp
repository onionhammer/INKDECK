// Registers: .claude/skills/waveshare-epaper-reference/references/power.md §3 and its porting
// notes (burst read, check OS).
#include "rtc.h"

#include <Arduino.h>
#include <Wire.h>
#include <sys/time.h>
#include <time.h>

#include <atomic>

#include "board_pins.h"
#include "inkdeck_secrets.h"  // TIMEZONE, generated from .env by scripts/gen_secrets.py

namespace rtc {
namespace {

constexpr uint8_t kRegControl2 = 0x01;  // bit 7 AIE (alarm interrupt on), bit 6 AF (fired)
constexpr uint8_t kRegSeconds = 0x04;  // 0x04..0x0A: s, min, h, day, weekday, month, year
constexpr uint8_t kRegAlarm = 0x0B;    // 0x0B..0x0E: s, min, h, day; bit 7 set = not compared
constexpr int kMinValidYear = 2025;     // anything earlier was never set (see begin())

std::atomic<bool> valid{false};  // set() runs on the time-sync task

int bcd(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }
uint8_t toBcd(int v) { return static_cast<uint8_t>((v / 10) << 4 | v % 10); }

// Seconds since the epoch for a UTC date and time; mktime() would apply the time zone.
// Howard Hinnant's days_from_civil.
time_t utcSeconds(const tm &t) {
  const int m = t.tm_mon + 1;
  const int y = t.tm_year + 1900 - (m <= 2);
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + t.tm_mday - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  const long days = era * 146097L + static_cast<long>(doe) - 719468;
  return static_cast<time_t>(days) * 86400 + t.tm_hour * 3600 + t.tm_min * 60 + t.tm_sec;
}

bool read(uint8_t reg, uint8_t *out, uint8_t n) {
  Wire.beginTransmission(board::I2C_ADDR_PCF85063);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(board::I2C_ADDR_PCF85063, n) != n) return false;
  for (uint8_t i = 0; i < n; ++i) out[i] = Wire.read();
  return true;
}

// Seconds (OS flag clear) to years, BCD, in one burst: the counters are blocked during the
// access.
bool writeTime(const uint8_t (&r)[7]) {
  Wire.beginTransmission(board::I2C_ADDR_PCF85063);
  Wire.write(kRegSeconds);
  Wire.write(r, sizeof r);
  if (Wire.endTransmission() != 0) {
    log_e("RTC: write failed");
    return false;
  }
  return true;
}

}  // namespace

bool begin() {
  setenv("TZ", TIMEZONE, 1);  // see rtc.h: the RTC and system clock are UTC
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
  const timeval tv = {utcSeconds(t), 0};
  settimeofday(&tv, nullptr);
  valid = true;
  tm local;
  localtime_r(&tv.tv_sec, &local);
  char when[32];
  strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S %Z", &local);
  log_i("RTC: %04d-%02d-%02d %02d:%02d:%02d UTC (%s)", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
        t.tm_hour, t.tm_min, t.tm_sec, when);
  return true;
}

bool timeValid() { return valid; }

bool set(time_t utc) {
  tm t;
  gmtime_r(&utc, &t);
  if (t.tm_year + 1900 < kMinValidYear || t.tm_year + 1900 > 2099) {
    log_e("RTC: refusing to store year %d", t.tm_year + 1900);
    return false;
  }
  const uint8_t r[7] = {toBcd(t.tm_sec),  toBcd(t.tm_min), toBcd(t.tm_hour),
                        toBcd(t.tm_mday), static_cast<uint8_t>(t.tm_wday),
                        toBcd(t.tm_mon + 1), toBcd(t.tm_year - 100)};
  if (!writeTime(r)) return false;
  valid = true;
  log_i("RTC: set to %04d-%02d-%02d %02d:%02d:%02d UTC", t.tm_year + 1900, t.tm_mon + 1,
        t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
  return true;
}

bool forget() {
  const uint8_t r[7] = {0x00, 0x00, 0x00, 0x01, 6, 0x01, 0x00};  // Sat 2000-01-01 00:00:00
  if (!writeTime(r)) return false;
  log_w("RTC: reset to 2000-01-01 (unset from the next boot)");
  return true;
}

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
