// Wi-Fi station on demand, joining one of the networks SD:/inkdeck.ini lists, else the one
// compiled in from .env (settings.h).
// The radio stays off unless something needs it: connect(), do the work, disconnect(). Named
// wlan because a wifi.h would shadow Arduino's WiFi.h on a case-insensitive file system.
//
// With several networks, connect() tries the one it last joined first (kept in NVS, so it
// holds across power-offs), then the others in the card's order. Each gets up to 10 s, less
// when the driver reports it out of range (one scan, 2 to 3 s) or it fails twice, and the list
// goes round again while time is left.
//
// Several tasks can use it at once (a time sync and a weather sync): each successful connect()
// is matched by one disconnect(), and the radio goes off after the last.
#pragma once
#include <stdint.h>

#include <string>

namespace wlan {

bool configured();  // the card or .env names a network
// The network being joined or joined; with the radio off, the one connect() tries first. ""
// if none. Safe from any task.
std::string network();

// For the System screen's WIFI row: "off · HOME-NET", "off · HOME-NET +2" (two more networks to
// try), "connecting · HOME-NET", "on · HOME-NET · -52 dBm". Safe from any task.
std::string describe();

// Blocks until connected (with an IP) or timeoutMs passes, whichever network it takes; on
// failure, error says why in a few words ("NETWORK NOT FOUND": none was in range) and the
// radio is off again (unless another user still has it). A second caller waits for a connect
// already in progress. Call from a background task.
bool connect(uint32_t timeoutMs, std::string &error);

void disconnect();  // this user is done; the radio goes off after the last one

// Radio off now, whoever is using it, without waiting for them (the board is powering off).
void forceOff();

bool radioOn();  // connecting, connected or held (the status bar shows WIFI)
// connect() is trying networks: the radio moves between channels, so a holder that sets the
// channel itself (comms.h) waits until it's done. Safe from any task.
bool joining();

// The radio on in station mode without joining anything, for scanning (wifi_survey.h) and the
// comms radio (comms.h, ESP-NOW, which needs Wi-Fi started but no network): it
// stays on until the matching releaseRadio(), whatever connect() and disconnect() users do
// meanwhile, and goes off at the release if nobody is connected. May wait for a connect in
// progress, so call from a background task.
void holdRadio();
void releaseRadio();

}  // namespace wlan
