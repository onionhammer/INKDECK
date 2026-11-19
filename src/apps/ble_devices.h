// Bluetooth LE devices (the canvas's "Sensors · BLE devices" and "· BLE device detail"), opened
// from the Sensors screen's menu. The scan runs while either screen is open (ble_survey.h).
//
// The list: a proximity strip at the top (every device a square at its signal, far on the left,
// near on the right; the selected one ringed and named); below, one two-line row per device: its
// name, or its address when it has none, then the address type, CONN when it takes connections,
// and what its advert says it is ("NEARBY INFO", "HEART RATE", "iBEACON 1/4"), with a tag for
// the kind (APPLE, HID, BEACON...), bars and dBm. Signals update every 5 s; the order is redone
// every 15 s, and the cursor stays on its device.
//   ▲▼ move   ● detail   HOLD ● sort (signal, kind, name)   BOOT back
//
// The detail: the name, address and address type; the signal live (every advert) with two
// minutes of history, and the advertised transmit power and path loss when it sends one; the
// advert decoded (kind, appearance, services, maker, flags, name, how often it advertises) and
// its raw bytes, one AD structure a line.
//   ▲▼ the previous / next device in the list's order   ● tone on/off   BOOT back to the list
#pragma once
#include <memory>

#include "ui/nav.h"

namespace apps {

std::unique_ptr<ui::Screen> makeBleDevices();

}  // namespace apps
