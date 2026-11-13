// Settings a file on the card can change without a rebuild. SD:/inkdeck.ini overrides the time
// zone and the location built in from .env (scripts/gen_secrets.py):
//
//   [clock]
//   timezone = CST6CDT,M3.2.0,M11.1.0   ; a POSIX TZ string, daylight saving rule included
//   [location]
//   latitude = 51.48                     ; decimal degrees, north and east positive
//   longitude = 0.00
//
// inkdeck.ini.example in the repo root is a filled-in copy; keep it in step with kTemplate in
// settings.cpp.
//
// Wi-Fi is never read from the card: a card is easy to lift. The file is read at boot (a missing
// one is created, every line commented out). The values last read from it are kept in NVS, so a
// standby wake (which doesn't mount the card) and a boot without the card keep using them; a
// card whose file doesn't set a value puts the built-in one back.
#pragma once
#include <string>

namespace settings {

constexpr const char *kFile = "/sdcard/inkdeck.ini";

enum class Source : unsigned char { Built, Card };

// Early in setup(), before anything uses local time: applies the time zone (from NVS if a card
// set one, else from .env).
void begin();
// After sdcard::mount(): reads SD:/inkdeck.ini, keeps what it sets in NVS and applies it.
void loadCard();

const char *timezone();  // the POSIX TZ string in use
Source timezoneSource();

bool hasLocation();  // a location is set (either way)
double latitude();
double longitude();
Source locationSource();

// For the System screen: "SD:/inkdeck.ini · TZ, LOCATION", ".env (built in)".
std::string describe();

// A POSIX TZ string newlib's tzset() understands: std offset [dst [offset] [,start,end]].
bool validTimezone(const std::string &tz);

}  // namespace settings
