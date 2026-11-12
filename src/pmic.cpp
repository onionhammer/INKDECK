// Register meanings: references/power.md §2 ("Status, battery and USB"), from XPowersLib's
// AXP2101 driver checked against docs/datasheets/TG28-PMIC.pdf.
#include "pmic.h"

#include <Arduino.h>
#include <Wire.h>

#include "board_pins.h"

namespace pmic {
namespace {

constexpr uint8_t kRegStatus1 = 0x00;  // bit 5 VBUS good, bit 3 battery present
constexpr uint8_t kRegStatus2 = 0x01;  // bits 7:5 = 001 charging, 010 discharging, 000 standby
constexpr uint8_t kRegPercent = 0xA4;  // fuel gauge, 0-100
constexpr uint8_t kRegLdoOn = 0x90;    // bit 0 ALDO1 ... bit 3 ALDO4 (TG28 datasheet 6.13.2.75)
constexpr uint32_t kCacheMs = 5000;

Status cached;
uint32_t readAt = 0;
bool everRead = false;

bool read(uint8_t reg, uint8_t *out, uint8_t n) {
  Wire.beginTransmission(board::I2C_ADDR_PMIC);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(board::I2C_ADDR_PMIC, n) != n) return false;
  for (uint8_t i = 0; i < n; ++i) out[i] = Wire.read();
  return true;
}

}  // namespace

Status status() {
  if (everRead && millis() - readAt < kCacheMs) return cached;
  readAt = millis();
  everRead = true;

  Status s;
  uint8_t st[2];
  uint8_t pct;
  if (read(kRegStatus1, st, 2)) {
    s.ok = true;
    s.vbus = st[0] & (1 << 5);
    s.battery = st[0] & (1 << 3);
    s.charging = (st[1] >> 5) == 0x01;
    if (s.battery && read(kRegPercent, &pct, 1) && pct <= 100) s.percent = pct;
    uint8_t ldo;
    if (read(kRegLdoOn, &ldo, 1)) s.aldo1 = ldo & 1;
  }
  if (s.ok != cached.ok || s.battery != cached.battery || s.vbus != cached.vbus) {
    log_i("PMIC: %s, battery %s (%d%%), USB %s, %s", s.ok ? "ok" : "no answer",
          s.battery ? "yes" : "no", s.percent, s.vbus ? "yes" : "no",
          s.charging ? "charging" : "not charging");
  }
  cached = s;
  return s;
}

}  // namespace pmic
