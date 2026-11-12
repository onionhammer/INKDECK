// Board smoke test: reports chip/flash/PSRAM and scans the shared I2C bus for the
// five onboard devices. Replace with real firmware once the toolchain is proven.
#include <Arduino.h>
#include <Wire.h>

#include "board_pins.h"

namespace {

struct I2cDevice {
  uint8_t addr;
  const char *name;
};

constexpr I2cDevice kExpected[] = {
    {board::I2C_ADDR_ES8311, "ES8311 codec"},
    {board::I2C_ADDR_PMIC, "TG28 PMIC"},
    {board::I2C_ADDR_PCF85063, "PCF85063 RTC"},
    {board::I2C_ADDR_QMI8658, "QMI8658 IMU"},
    {board::I2C_ADDR_SHTC3, "SHTC3 temp/humidity"},
};

void reportSystem() {
  Serial.printf("Chip:  %s rev %d, %d cores @ %lu MHz\n", ESP.getChipModel(),
                ESP.getChipRevision(), ESP.getChipCores(), ESP.getCpuFreqMHz());
  Serial.printf("Flash: %lu MB\n", ESP.getFlashChipSize() >> 20);
  Serial.printf("PSRAM: %lu KB total, %lu KB free%s\n", ESP.getPsramSize() >> 10,
                ESP.getFreePsram() >> 10,
                ESP.getPsramSize() ? "" : "  <-- PSRAM missing: check memory_type");
}

void scanI2c() {
  Wire.begin(board::I2C_SDA, board::I2C_SCL, 400000);
  for (const auto &dev : kExpected) {
    Wire.beginTransmission(dev.addr);
    const bool found = Wire.endTransmission() == 0;
    Serial.printf("I2C 0x%02X %-20s %s\n", dev.addr, dev.name, found ? "OK" : "MISSING");
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(2000);  // give the host time to open the USB CDC port
  Serial.println("\n== ESP32-S3-ePaper-3.97 smoke test ==");
  reportSystem();
  scanI2c();
}

void loop() { delay(1000); }
