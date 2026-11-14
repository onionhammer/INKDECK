// QMI8658A 6-axis IMU (I2C 0x6B on this board; see board_pins.h), polled. Registers from the
// datasheet; references/sensors.md §5 lists the vendor driver's bugs this avoids (gyro scale
// 2x off, gyro X/Y swapped, low-pass filters never enabled).
//
// Configuration: accelerometer ±4 g, gyroscope ±256 °/s, both at 56 Hz (6-axis mode) with the
// low-pass filters on. The only interrupt is wake on motion, on INT2 (GPIO40; INT1 isn't wired).
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

// Both sensors off (standby, a few µA) or back on with the configuration above, leaving wake
// on motion if it was armed. The Reader's light sleep turns them off; the first readings after
// turning them on may still be settling (or still hold the last ones from before).
bool setEnabled(bool on);

// Wake on motion (datasheet §12): the gyroscope off, the accelerometer in its 11 Hz low-power
// mode, and INT2 (board::IMU_INT2) toggling each time the acceleration on an axis moves by more
// than thresholdMg from one sample to the next: by default a turn of the board, not a hand
// holding it still. read() gets no new data in this mode. The picture viewer's light sleep
// arms it, so turning the board wakes it (power::nap()); setEnabled() ends it.
constexpr int kWakeThresholdMg = 100;
bool wakeOnMotion(int thresholdMg = kWakeThresholdMg);  // 1-255

}  // namespace qmi8658
