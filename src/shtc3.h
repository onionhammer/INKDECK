// SHTC3 temperature/humidity sensor (I2C 0x70 on the shared Wire bus). Commands, timings, CRC
// and conversion: references/sensors.md §2, checked against the datasheet. The sensor sleeps
// between readings (about 0.3 µA).
//
// No offset is applied here; sensor_hub corrects for the board's self-heating (calibrated at
// 3.2 °C; Waveshare subtracts 1.5 °C in IDF or 4 °C in Arduino).
#pragma once

namespace shtc3 {

// Wakes the sensor, soft-resets it and checks its ID. Begin Wire first.
bool begin();
bool present();

struct Reading {
  bool ok = false;
  float tempC = 0;
  float rh = 0;  // % relative humidity
};

// One measurement (normal mode, about 13 ms, blocking). Wakes the sensor and puts it back to
// sleep.
Reading read();

}  // namespace shtc3
