// Sensors (the "Sensors" artboard, and "Sensors · scan panel on WI-FI / BLE" and "· menu sheet"):
// SHTC3 temperature/humidity with a 24 h sparkline, the IMU as a bubble level with an
// accel/gyro table, PMIC battery readings, and a scan panel at the bottom: the live i2cdetect
// grid of the shared bus, the Wi-Fi networks in range or the Bluetooth LE devices, the strongest
// seven of each (wifi_survey.h, ble_survey.h).
//   ▲▼ panel: ENV, IMU, PMIC, then I2C, WI-FI and BLE, which share the bottom panel (it keeps
//      the last of them). The focused panel (inverted header) updates live, the others once a
//      minute; a radio scans only while its stop has the focus
//   ● the // SENSORS menu sheet: WI-FI NETWORKS and BLUETOOTH LE open their screens
//      (apps/wifi_networks.h, apps/ble_devices.h), HOLD READINGS freezes them all, LOG TO SD
//      starts or stops the CSV log. It opens on the focused radio's row, else on the item last run
//   HOLD ● start / stop the CSV log   BOOT back
// Logging keeps running after leaving the screen.
#pragma once
#include <memory>

#include "ui/nav.h"

namespace apps {

std::unique_ptr<ui::Screen> makeSensors();

}  // namespace apps
