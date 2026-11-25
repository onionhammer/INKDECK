// Settings a file on the card can change without a rebuild. SD:/inkdeck.ini overrides the Wi-Fi
// network, time zone and location built in from .env (scripts/gen_secrets.py), and sets the
// lock (apps/lock.h):
//
//   [wifi]
//   ssid = HomeNet
//   password = secret                    ; the board keeps it and replaces the line with a note
//   ssid = Phone                         ; more networks, up to 8, in the order to try them
//   password = another-secret            ; each password goes under its ssid
//   [clock]
//   timezone = CST6CDT,M3.2.0,M11.1.0   ; a POSIX TZ string, daylight saving rule included
//   [location]
//   latitude = 51.48                     ; decimal degrees, north and east positive
//   longitude = 0.00
//   [lock]
//   pin = 2468                           ; 4 to 8 digits; pin = off removes it
//   lock_minutes = 5                     ; also lock after this long without a key (0: only at boot)
//   [agent]
//   agent_host = nas.local               ; the machine running inkharness serve (agent_client.h); only the card sets it
//   agent_port = 7391
//   agent_token = something-long         ; stays on the card: it keys the agent link (agent_noise.h)
//   agent_face_details = on              ; the Agent face shows titles even with a PIN on the Agent
//   [news]
//   news_face_every = 5                  ; minutes per story on News's off face: 1, 5, 10 or 15 (apps/news_edition.h)
//   [tailscale]
//   tailscale_key = tskey-auth-...       ; the board keeps it and replaces the line with a note (tailnet.h)
//   tailscale_name = inkdeck             ; the board's name on the tailnet
//   tailscale_exit_node = on             ; offer the board as an exit node, IPv4 (tailnet.h)
//   [comms]
//   mesh_group = <secret>?name=Family&via=air   ; up to 4; stays on the card: it keys the group (comms.h)
//   mesh_name = KITCHEN                  ; this board's name on the others
//   mesh_channel = 6                     ; the radio's Wi-Fi channel off Wi-Fi
//   mesh_range = long                    ; send at the long-range rate, 500 kbps (comms.h)
//   mesh_reachable = 10                  ; on battery, sleep polling every 10 s instead of powering off (comms.h)
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
// "Built in" means the firmware's .env values, or for an image built without one (a release from
// GitHub, ota.h) the value the last image that had it kept in NVS: so an update from GitHub
// keeps the Wi-Fi network, time zone and location the board was built with.
//
// Each ssid line names a network wlan.h may join, and the password line after it (before the
// next ssid) is that network's. wlan.h tries the network it last joined first, then the others
// in the file's order. The card's networks replace the built-in one; without an ssid line, the
// built-in network.
//
// Passwords don't stay on the card, which is easy to lift. The board keeps each Wi-Fi password
// (by the ssid it belongs to) and each SMB share's (by its user, server and share) in NVS, and
// rewrites their lines without them: a password line becomes a note, and an smb line loses the
// :password part of its URL. To change one, write it in again. An ssid with no password line
// joins its network with the password the board kept for that ssid, else as an open one. A
// password the board kept for an ssid the file no longer lists is dropped. NVS isn't encrypted:
// anyone with the board and a USB cable can read it, as they can the firmware's .env values.
//
// The Tailscale auth key works like the PIN below, without the wait: the board keeps it in NVS
// and replaces its line with a note, a file without the line keeps the board's key, and
// tailscale_key = off removes it.
//
// The PIN is the exception: a card is easy to lift, so the board keeps it in NVS (System's PIN
// LOCK sets it too) and replaces the pin line on the card with a note once it has acted on it.
// A file without a pin line keeps the board's PIN. A new PIN on a board that has none applies
// at once; a different one, or pin = off, waits until the lock is opened with the current PIN
// (unlocked()), and its line stays on the card until then, so a power-off before that doesn't
// lose it.
#pragma once
#include <stdint.h>

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

// The Wi-Fi networks wlan.h may join: the card's ([wifi] ssid lines, in the file's order), else
// .env's one.
constexpr int kMaxWifiNetworks = 8;
struct WifiNetwork {
  std::string ssid;
  std::string password;  // "" for an open network
};
bool hasWifi();  // either names a network
std::vector<WifiNetwork> wifiNetworks();  // reads the passwords from NVS
std::vector<std::string> wifiSsids();     // the names alone, from RAM
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

// The agent daemon (agent_client.h, docs/voice-protocol.md). Only the card names one.
bool hasAgent();
std::string agentHost();
int agentPort();
std::string agentToken();  // "" if the daemon asks for none
// agent_face_details = on: the Agent face (and its copy in NVS) keeps titles and the question
// even when the PIN guards the Agent. Off by default: counts only.
bool agentFaceDetails();

// news_face_every: how many minutes each story stays on News's off face (apps/news_edition.h):
// 1, 5, 10 or 15; 5 without a line.
int newsFaceEvery();

// The tailnet (tailnet.h). hasTailscale(): the board has an auth key.
bool hasTailscale();
std::string tailscaleKey();   // from NVS; "" if none
std::string tailscaleName();  // tailscale_name, else "inkdeck"
bool tailscaleExitNode();     // tailscale_exit_node = on: the board offers itself as an exit node

// [comms] (comms.h, docs/comms-protocol.md). Each mesh_group line is a group: a secret every
// board in it shares (8 to 128 printable bytes, no ?), then options after a ?, joined by &:
// name (what the screens call it, up to 16 bytes), via (air, net or both: how it travels; both
// by default) and hub (where daemons meet; kept, not used yet). Values are %XX-decoded. Like
// agent_token, the secret stays on the card and in NVS: it is the invite.
constexpr int kMaxMeshGroups = 4;
enum class MeshVia : unsigned char { Both, Air, Net };
struct MeshGroup {
  std::string secret;
  std::string name;  // "" if the line names none
  MeshVia via = MeshVia::Both;
  std::string hub;     // "" if none
  uint32_t line = 0;   // FNV-1a of the whole line: another value once the line changes
};
std::vector<MeshGroup> meshGroups();  // in the file's order
std::string meshName();  // mesh_name, "" if the card sets none
int meshChannel();       // mesh_channel: 1 to 13, 6 without a line
// mesh_range: the rate this board's comms packets go out at (comms.h). Normal (the default, and
// any value but long): 1 Mbps. Long: Espressif's long-range rate, 500 kbps, which reaches
// further. Every board hears both. The engine asks comms::range(), which a dev override can
// change.
enum class MeshRange : unsigned char { Normal, Long };
MeshRange meshRange();
// mesh_reachable: on battery, the seconds between a sleeping board's polls instead of a power-off
// (comms.h, power.h): 2 to 30, on meaning 5; 0 for off (the default).
int meshReachable();

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

// Wrong PINs in a row on any PIN screen (apps/lock.h), kept in NVS so a restart doesn't clear
// them.
int pinMisses();
void setPinMisses(int n);

// System's PIN LOCK, once it has the current PIN: a new PIN (4 to 8 digits), or none.
void setPin(const std::string &digits);
void clearPin();

// For the System screen: "SD:/inkdeck.ini · WI-FI, TZ, LOCATION", ".env (built in)". The lock
// has a row of its own there (PIN LOCK).
std::string describe();

// A POSIX TZ string newlib's tzset() understands: std offset [dst [offset] [,start,end]].
bool validTimezone(const std::string &tz);

}  // namespace settings
