// Settings a file on the card can change without a rebuild. SD:/inkdeck.ini overrides the Wi-Fi
// network, time zone and location built in from .env (scripts/gen_secrets.py), and sets the
// lock (apps/lock.h):
//
//   [wifi]
//   ssid = HomeNet
//   password = secret                    ; the board keeps it and replaces the line with a note
//   [clock]
//   timezone = CST6CDT,M3.2.0,M11.1.0   ; a POSIX TZ string, daylight saving rule included
//   [location]
//   latitude = 51.48                     ; decimal degrees, north and east positive
//   longitude = 0.00
//   [lock]
//   pin = 2468                           ; 4 to 8 digits; pin = off removes it
//   lock_minutes = 5                     ; also lock after this long without a key (0: only at boot)
//
// and lists the network shares Files browses (shares.h has the syntax), one line each:
//
//   [network]
//   smb = smb://erik:secret@nas.local/share/media   ; the board keeps the password, as for Wi-Fi
//   nfs = nfs://nas.local/volume1/books
//
// inkdeck.ini.example in the repo root is a filled-in copy; keep it in step with kTemplate in
// settings.cpp.
//
// The file is read at boot (a missing one is created, every line commented out). The values
// last read from it are kept in NVS, so a standby wake (which doesn't mount the card) and a boot
// without the card keep using them; a card whose file doesn't set a value puts the built-in one
// back.
//
// Passwords don't stay on the card, which is easy to lift. The board keeps a Wi-Fi password
// (with the ssid it belongs to) and each SMB share's (by its user, server and share) in NVS, and
// rewrites their lines without them: the password line becomes a note, and an smb line loses
// the :password part of its URL. To change one, write it in again. An ssid with no password
// line joins its network with the stored password if it's the same network, else as an open
// one. Without an ssid line, the built-in network and no stored password. NVS isn't encrypted:
// anyone with the board and a USB cable can read it, as they can the firmware's .env values.
//
// The PIN is the exception: a card is easy to lift, so the board keeps it in NVS (System's PIN
// LOCK sets it too) and replaces the pin line on the card with a note once it has acted on it.
// A file without a pin line keeps the board's PIN. A new PIN on a board that has none applies
// at once; a different one, or pin = off, waits until the lock is opened with the current PIN
// (unlocked()), and its line stays on the card until then, so a power-off before that doesn't
// lose it.
#pragma once
#include <string>
#include <vector>

#include "shares.h"

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

// The Wi-Fi network wlan.h joins: the card's ([wifi] ssid), else .env's.
bool hasWifi();  // either names a network
std::string wifiSsid();
std::string wifiPassword();  // "" for an open network
Source wifiSource();

// The file's `smb =` and `nfs =` lines, in order, SMB passwords taken out (shares.h reads them).
struct ShareLine {
  ::shares::Kind kind;
  std::string url;
  bool operator==(const ShareLine &o) const { return kind == o.kind && url == o.url; }
};
std::vector<ShareLine> shareLines();
// The password the card gave for an SMB share (by shares::Share::credentialId()); "" if none.
std::string sharePassword(const std::string &credentialId);

// The lock. hasPin(): the board locks at boot, on waking the desk face and after lockMinutes()
// without a key (0: never for that).
bool hasPin();
int pinLength();
bool pinMatches(const std::string &digits);
int lockMinutes();

// A PIN change the card asked for at this boot, waiting for the current PIN.
enum class PinChange : unsigned char { None, New, Off };
PinChange pendingPin();
// The lock was opened with the right PIN: applies the pending change, if any.
void unlocked();

// System's PIN LOCK, once it has the current PIN: a new PIN (4 to 8 digits), or none.
void setPin(const std::string &digits);
void clearPin();

// For the System screen: "SD:/inkdeck.ini · WI-FI, TZ, LOCATION", ".env (built in)". The lock
// has a row of its own there (PIN LOCK).
std::string describe();

// A POSIX TZ string newlib's tzset() understands: std offset [dst [offset] [,start,end]].
bool validTimezone(const std::string &tz);

}  // namespace settings
