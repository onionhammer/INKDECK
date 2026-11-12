// Wi-Fi station on demand, joining the network compiled in from .env (scripts/gen_secrets.py).
// The radio stays off unless something needs it: connect(), do the work, disconnect(). Named
// wlan because a wifi.h would shadow Arduino's WiFi.h on a case-insensitive file system.
#pragma once
#include <stdint.h>

#include <string>

namespace wlan {

bool configured();      // .env names a network
const char *network();  // its SSID, "" if none

// For the System screen's WIFI row: "off · HOME-NET", "connecting · HOME-NET",
// "on · HOME-NET · -52 dBm". Safe from any task.
std::string describe();

// Blocks until connected (with an IP) or timeoutMs passes; on failure, error says why in a few
// words ("NETWORK NOT FOUND") and the radio is off again. Call from a background task.
bool connect(uint32_t timeoutMs, std::string &error);

void disconnect();  // and radio off

}  // namespace wlan
