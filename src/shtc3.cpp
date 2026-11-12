#include "shtc3.h"

#include <Arduino.h>
#include <Wire.h>

#include "board_pins.h"

namespace shtc3 {
namespace {

constexpr uint16_t kWakeup = 0x3517;
constexpr uint16_t kSleep = 0xB098;
constexpr uint16_t kSoftReset = 0x805D;
constexpr uint16_t kReadId = 0xEFC8;
constexpr uint16_t kMeasure = 0x7866;  // normal mode, temperature first, no clock stretching
constexpr uint32_t kWakeMs = 1;        // datasheet: 240 µs max after wakeup or soft reset
constexpr uint32_t kMeasureMs = 13;    // datasheet: 12.1 ms max

bool found = false;

bool command(uint16_t cmd) {
  Wire.beginTransmission(board::I2C_ADDR_SHTC3);
  Wire.write(cmd >> 8);
  Wire.write(cmd & 0xFF);
  return Wire.endTransmission() == 0;
}

// CRC-8, polynomial 0x31, init 0xFF (datasheet Table 16: CRC(0xBEEF) = 0x92).
uint8_t crc8(const uint8_t *data, int n) {
  uint8_t crc = 0xFF;
  for (int i = 0; i < n; ++i) {
    crc ^= data[i];
    for (int b = 0; b < 8; ++b) crc = (crc & 0x80) ? (crc << 1) ^ 0x31 : crc << 1;
  }
  return crc;
}

// Reads n bytes; while a measurement is still running the sensor NACKs, so retry briefly.
bool readBytes(uint8_t *out, uint8_t n) {
  for (int attempt = 0; attempt < 4; ++attempt) {
    if (Wire.requestFrom(board::I2C_ADDR_SHTC3, n) == n) {
      for (uint8_t i = 0; i < n; ++i) out[i] = Wire.read();
      return true;
    }
    delay(2);
  }
  return false;
}

}  // namespace

bool begin() {
  found = false;
  if (!command(kWakeup)) {
    log_e("SHTC3: no response at 0x%02X", board::I2C_ADDR_SHTC3);
    return false;
  }
  delay(kWakeMs);
  command(kSoftReset);
  delay(kWakeMs);
  uint8_t id[3];
  if (!command(kReadId) || !readBytes(id, 3) || crc8(id, 2) != id[2]) {
    log_e("SHTC3: ID read failed");
    return false;
  }
  const uint16_t value = (id[0] << 8) | id[1];
  if ((value & 0x083F) != 0x0807) {  // datasheet Table 15
    log_e("SHTC3: unexpected ID 0x%04X", value);
    return false;
  }
  command(kSleep);
  found = true;
  log_i("SHTC3: found, ID 0x%04X", value);
  return true;
}

bool present() { return found; }

Reading read() {
  Reading r;
  if (!found || !command(kWakeup)) return r;
  delay(kWakeMs);
  uint8_t b[6];
  if (command(kMeasure)) {
    delay(kMeasureMs);
    if (readBytes(b, 6) && crc8(b, 2) == b[2] && crc8(b + 3, 2) == b[5]) {
      r.ok = true;
      r.tempC = -45.0f + 175.0f * ((b[0] << 8) | b[1]) / 65536.0f;  // datasheet §5.11
      r.rh = 100.0f * ((b[3] << 8) | b[4]) / 65536.0f;
    }
  }
  command(kSleep);
  return r;
}

}  // namespace shtc3
