// QMI8658A 6-axis IMU (I2C 0x6B on this board; see board_pins.h), polled. Registers from the
// datasheet; references/sensors.md §5 lists the vendor driver's bugs this avoids (gyro scale
// 2x off, gyro X/Y swapped, low-pass filters never enabled).
//
// Configuration: accelerometer ±4 g, gyroscope ±256 °/s, both at 56 Hz (6-axis mode) with the
// low-pass filters on. Interrupts are unused; INT2 (GPIO40) is the usable pin if they're needed.
#pragma once

namespace qmi8658 {

// Soft-resets and configures the IMU. Begin Wire first.
bool begin();
bool present();

struct Reading {
  bool ok = false;
  float acc[3] = {};   // g, chip axes X, Y, Z
  float gyro[3] = {};  // °/s, chip axes X, Y, Z
  float tempC = 0;     // die temperature
};

Reading read();

}  // namespace qmi8658
