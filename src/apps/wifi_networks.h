// Wi-Fi networks (the canvas's "Sensors · Wi-Fi networks" and "· Wi-Fi network detail"),
// opened from the Sensors screen's menu. Scans run while either screen is open (wifi_survey.h).
//
// The list: a chart of the 2.4 GHz channels at the top, each network a trapezoid over the
// channels it covers (20 or 40 MHz) at the height of its signal, the selected one dithered and
// labelled; below, one two-line row per network (name, then channel, width, security and Wi-Fi
// generation; SAVED for one inkdeck.ini lists; bars and dBm). It rescans every 15 s and the
// cursor stays on its network.
//   ▲▼ move   ● detail   HOLD ● sort (signal, channel, name)   BOOT back
//
// The detail: the name, BSSID and generation; the signal live (only its channel scanned, every
// 2 s) with two minutes of history; everything the driver reports (channel and width, security
// and ciphers, WPS, country, BSS colour, FTM ranging); and the networks on the same or
// overlapping channels.
//   ▲▼ the previous / next network in the list's order   ● tone on/off (a beep a second, higher
//   as the signal gets stronger)   BOOT back to the list
#pragma once
#include <memory>

#include "ui/nav.h"

namespace apps {

std::unique_ptr<ui::Screen> makeWifiNetworks();

}  // namespace apps
