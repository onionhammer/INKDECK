#include "qmi8658.h"

#include <Arduino.h>
#include <Wire.h>

#include "board_pins.h"

namespace qmi8658 {
namespace {

// Datasheet Table 19 / 22 / 27.
constexpr uint8_t kRegWhoAmI = 0x00;  // reads 0x05
constexpr uint8_t kRegCtrl1 = 0x02;
constexpr uint8_t kRegCtrl2 = 0x03;   // accelerometer: 6:4 full scale, 3:0 ODR
constexpr uint8_t kRegCtrl3 = 0x04;   // gyroscope: 6:4 full scale, 3:0 ODR
constexpr uint8_t kRegCtrl5 = 0x06;   // low-pass filters
constexpr uint8_t kRegCtrl7 = 0x08;   // sensor enables
constexpr uint8_t kRegTempL = 0x33;   // TEMP_L, TEMP_H, then AX_L ... GZ_H (little-endian pairs)
constexpr uint8_t kRegResetResult = 0x4D;  // 0x80 after a successful reset
constexpr uint8_t kRegReset = 0x60;        // write 0xB0 (§5.9; §7.4's 0x0B is a typo)

constexpr uint8_t kCtrl1 = 0x60;  // address auto-increment; BE left at its default (see §5 note 8)
constexpr uint8_t kCtrl2 = 0x17;  // ±4 g (001), 62.5 Hz accel-only / 56.05 Hz 6-axis (0111)
constexpr uint8_t kCtrl3 = 0x47;  // ±256 °/s (100), 56.05 Hz (0111)
constexpr uint8_t kCtrl5 = 0x77;  // both LPFs on, mode 11 (13.37 % of ODR, ~7.5 Hz)
constexpr uint8_t kCtrl7 = 0x03;  // accelerometer + gyroscope on
constexpr float kAccLsbPerG = 8192.0f;    // ±4 g (Table 7)
constexpr float kGyroLsbPerDps = 128.0f;  // ±256 °/s (Table 8)

bool found = false;

bool write(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(board::I2C_ADDR_QMI8658);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

bool read(uint8_t reg, uint8_t *out, uint8_t n) {
  Wire.beginTransmission(board::I2C_ADDR_QMI8658);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(board::I2C_ADDR_QMI8658, n) != n) return false;
  for (uint8_t i = 0; i < n; ++i) out[i] = Wire.read();
  return true;
}

int16_t le16(const uint8_t *p) { return static_cast<int16_t>(p[0] | (p[1] << 8)); }

}  // namespace

bool begin() {
  found = false;
  uint8_t id = 0;
  for (int attempt = 0; attempt < 5 && id != 0x05; ++attempt) {  // the vendor probes 5 times too
    if (attempt) delay(5);
    if (!read(kRegWhoAmI, &id, 1)) id = 0;
  }
  if (id != 0x05) {
    log_e("QMI8658: no IMU at 0x%02X (WHO_AM_I 0x%02X)", board::I2C_ADDR_QMI8658, id);
    return false;
  }
  write(kRegReset, 0xB0);
  uint8_t done = 0;
  for (int i = 0; i < 50 && done != 0x80; ++i) {  // datasheet: reset takes ~15 ms
    delay(2);
    read(kRegResetResult, &done, 1);
  }
  if (done != 0x80) log_w("QMI8658: reset not confirmed (0x4D = 0x%02X)", done);
  const bool ok = write(kRegCtrl1, kCtrl1) && write(kRegCtrl2, kCtrl2) &&
                  write(kRegCtrl3, kCtrl3) && write(kRegCtrl5, kCtrl5) && write(kRegCtrl7, kCtrl7);
  if (!ok) {
    log_e("QMI8658: configuration failed");
    return false;
  }
  found = true;
  log_i("QMI8658: found, ±4 g / ±256 dps at 56 Hz");
  return true;
}

bool present() { return found; }

Reading read() {
  Reading r;
  uint8_t b[14];
  if (!found || !read(kRegTempL, b, sizeof b)) return r;
  r.ok = true;
  r.tempC = le16(b) / 256.0f;  // §3.9: TEMP_H + TEMP_L / 256
  for (int i = 0; i < 3; ++i) {
    r.acc[i] = le16(b + 2 + 2 * i) / kAccLsbPerG;
    r.gyro[i] = le16(b + 8 + 2 * i) / kGyroLsbPerDps;
  }
  return r;
}

}  // namespace qmi8658
