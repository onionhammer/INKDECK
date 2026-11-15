// Wi-Fi station on demand, joining the network SD:/inkdeck.ini names, else the one compiled in
// from .env (settings.h).
// The radio stays off unless something needs it: connect(), do the work, disconnect(). Named
// wlan because a wifi.h would shadow Arduino's WiFi.h on a case-insensitive file system.
//
// Several tasks can use it at once (a time sync and a weather sync): each successful connect()
// is matched by one disconnect(), and the radio goes off after the last.
#pragma once
#include <stdint.h>

#include <string>

namespace wlan {

bool configured();      // the card or .env names a network
std::string network();  // its SSID, "" if none

// For the System screen's WIFI row: "off · HOME-NET", "connecting · HOME-NET",
// "on · HOME-NET · -52 dBm". Safe from any task.
std::string describe();

// Blocks until connected (with an IP) or timeoutMs passes; on failure, error says why in a few
// words ("NETWORK NOT FOUND") and the radio is off again (unless another user still has it).
// A second caller waits for a connect already in progress. Call from a background task.
bool connect(uint32_t timeoutMs, std::string &error);

void disconnect();  // this user is done; the radio goes off after the last one

// Radio off now, whoever is using it, without waiting for them (the board is powering off).
void forceOff();

bool radioOn();  // connecting or connected (the status bar shows WIFI)

}  // namespace wlan
