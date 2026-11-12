// Sensors (the "Sensors" artboard): SHTC3 temperature/humidity with a 24 h sparkline, the IMU
// as a bubble level with an accel/gyro table, PMIC battery readings, and a live i2cdetect grid
// of the shared bus.
//   ▲▼ panel: the focused panel (inverted header) updates live, the others once a minute
//   ● hold / resume all readings   HOLD ● start / stop the CSV log on SD   BOOT back
// Logging keeps running after leaving the screen.
#pragma once
#include <memory>

#include "ui/nav.h"

namespace apps {

std::unique_ptr<ui::Screen> makeSensors();

}  // namespace apps
