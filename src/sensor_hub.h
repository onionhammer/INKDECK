// (File named sensor_hub, not sensors, so it isn't shadowed by apps/sensors.h.)
// Sensor service on top of the SHTC3 and QMI8658A drivers.
//
// A background task samples temperature/humidity every few seconds and keeps a day of
// half-hour averages (in RAM, so the history starts again at each boot). With logging on it
// also appends a CSV row of every reading to the SD card. The IMU is read on demand by the
// caller.
#pragma once
#include <stdint.h>

#include <string>
#include <vector>

#include "qmi8658.h"
#include "shtc3.h"

namespace sensors {

// Starts the drivers, scans the bus once and starts the sampler. Begin Wire first.
void begin();

// Latest temperature/humidity sample, corrected for the board's self-heating (a calibrated
// offset, with humidity recomputed for the cooler room); ok = false until the first one (or
// with no SHTC3). The CSV log records these corrected values too.
shtc3::Reading env();

// Half-hour temperature averages, oldest first, at most kHistorySlots. The last entry is the
// half hour in progress.
constexpr int kHistorySlots = 48;
std::vector<float> history();

// The IMU, read now, in screen axes (portrait: X right, Y up, Z out of the screen), with the
// gyro's zero-rate offset (learnt while the board sits still) taken off. Call from the UI task
// only. The CSV log records raw chip axes.
qmi8658::Reading motion();

// Pitch and roll in degrees from a motion() reading: 0, 0 lying flat, screen up; pitch > 0
// with the top edge raised, roll > 0 with the right edge raised.
void tilt(const qmi8658::Reading &m, float &pitch, float &roll);

// i2cdetect: the addresses (0x03-0x77) that ACK. Probes the bus, a few ms.
std::vector<uint8_t> scanBus();
// The last scan's result (from begin() or the latest scanBus()).
std::vector<uint8_t> lastScan();
// The board's five I2C devices: how many answered in the last scan.
int knownFound();
constexpr int kKnownCount = 5;
const char *deviceName(uint8_t address);  // "SHTC3", or nullptr if not one of ours

// CSV log on the SD card: a row every kLogEverySeconds while on.
constexpr int kLogEverySeconds = 10;
bool logging();
bool setLogging(bool on);  // false if the file can't be opened
const char *logPath();

}  // namespace sensors
