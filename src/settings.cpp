#include "settings.h"

#include <Arduino.h>
#include <Preferences.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include <algorithm>

#include "inkdeck_secrets.h"  // WIFI_*, TIMEZONE, LOCATION_CONFIGURED, LATITUDE, LONGITUDE
#include "shares.h"
#include "sdcard.h"
#include "text.h"

namespace settings {
namespace {

constexpr const char *kNamespace = "settings";
constexpr int kMaxLockMinutes = 24 * 60;

// What the card set; empty / false means the built-in value is used.
struct Values {
  std::string tz;
  bool location = false;
  double lat = 0;
  double lon = 0;
  std::vector<std::string> wifiSsids;  // in the file's order
  std::vector<ShareLine> shares;       // passwords taken out
  int lockMinutes = 0;
  std::string agentHost;
  int agentPort = 0;  // 0: the default
  std::string agentToken;
  std::string tailscaleName;  // "": the default
  bool tailscaleExitNode = false;
  bool agentFaceDetails = false;
  int newsFaceEvery = 5;  // minutes per story on News's off face
  std::vector<std::string> meshGroups;  // the mesh_group lines' values, checked, in order
  std::string meshName;
  int meshChannel = 6;
  bool meshMicPush = false;  // mesh_mic = push
  bool meshRangeLong = false;  // mesh_range = long
  int meshReachable = 0;     // mesh_reachable: seconds between polls, 0 off

  bool operator==(const Values &o) const {
    return tz == o.tz && location == o.location && lat == o.lat && lon == o.lon &&
           wifiSsids == o.wifiSsids && shares == o.shares && lockMinutes == o.lockMinutes &&
           agentHost == o.agentHost && agentPort == o.agentPort && agentToken == o.agentToken &&
           tailscaleName == o.tailscaleName && tailscaleExitNode == o.tailscaleExitNode &&
           agentFaceDetails == o.agentFaceDetails && newsFaceEvery == o.newsFaceEvery &&
           meshGroups == o.meshGroups && meshName == o.meshName && meshChannel == o.meshChannel &&
           meshMicPush == o.meshMicPush && meshRangeLong == o.meshRangeLong &&
           meshReachable == o.meshReachable;
  }
};

// Passwords by what they open: an ssid, or an SMB share's credentialId.
using Secrets = std::vector<std::pair<std::string, std::string>>;

// Passwords the card gave at this boot, which the board keeps and takes off the card.
struct CardSecrets {
  Secrets wifi;  // one per ssid, for the networks with a password line
  Secrets smb;
};

// The card's pin line. Not in Values: a file without one keeps the board's PIN (settings.h).
struct CardPin {
  enum Kind : unsigned char { Absent, Digits, Off } kind = Absent;
  std::string digits;
};

// The card's tailscale_key line, kept the same way: a file without one keeps the board's key.
struct CardTailscaleKey {
  enum Kind : unsigned char { Absent, Key, Off } kind = Absent;
  std::string key;
};

Values card;
bool loaded = false;
std::string pin;  // the board's PIN (NVS); empty: no lock
PinChange pending = PinChange::None;
std::string pendingDigits;  // the PIN a pending PinChange::New brings
bool tailscaleKeySet = false;  // NVS has "tskey"

// What a Wi-Fi password line becomes once the board has it, and what an smb line whose
// password the board took gets after it.
constexpr const char *kWifiPasswordNote =
    "; password: the board keeps it. Write password = ... here to change it.";
constexpr const char *kSmbPasswordNote = " ; the board keeps the password";

// What a pin line becomes once the board has acted on it. A comment, so it never reads as a
// value, and true whether the board has a PIN now or not.
constexpr const char *kPinNote =
    "; The board keeps the PIN (System > PIN LOCK). Write pin = <digits> to change it, pin = off "
    "to remove it.";

// The same for a tailscale_key line, true whether the board has a key now or not.
constexpr const char *kTailscaleKeyNote =
    "; The board keeps its Tailscale auth key (System > TAILNET). Write tailscale_key = ... to "
    "change it, tailscale_key = off to remove it.";

constexpr const char *kTemplate =
    "; INKDECK settings. A value here overrides the one built into the firmware from .env.\n"
    "; Remove the ; in front of a line to use it. Read when the board starts.\n"
    "; Passwords don't stay here: the board keeps them and rewrites their lines.\n"
    "\n"
    "[wifi]\n"
    "; The Wi-Fi networks to join, instead of the one built into the firmware: up to 8 ssid\n"
    "; lines, each with its password on the line under it (none: an open network). The board\n"
    "; tries the network it last joined first, then the others in this order. It keeps the\n"
    "; passwords and replaces their lines with a note.\n"
    "; ssid = HomeNet\n"
    "; password = correct horse battery staple\n"
    "; ssid = Phone\n"
    "; password = another passphrase\n"
    "\n"
    "[clock]\n"
    "; The time zone as a POSIX TZ string, which includes the daylight saving rule:\n"
    ";   EST5EDT,M3.2.0,M11.1.0      US Eastern\n"
    ";   CST6CDT,M3.2.0,M11.1.0      US Central\n"
    ";   MST7MDT,M3.2.0,M11.1.0      US Mountain\n"
    ";   PST8PDT,M3.2.0,M11.1.0      US Pacific\n"
    ";   GMT0BST,M3.5.0/1,M10.5.0    UK\n"
    ";   CET-1CEST,M3.5.0,M10.5.0/3  Central Europe\n"
    "; timezone = GMT0BST,M3.5.0/1,M10.5.0\n"
    "\n"
    "[location]\n"
    "; Where sunrise, sunset and the weather forecast are for, in decimal degrees (north and\n"
    "; east positive; two decimals is about a kilometre). Set both or neither.\n"
    "; latitude = 51.48\n"
    "; longitude = 0.00\n"
    "\n"
    "[network]\n"
    "; Shares to browse in Files, up to 4, read-only. SMB (Windows, Samba, most NAS boxes):\n"
    "; smb://user:password@server/share/folder. The board keeps the password and takes it out\n"
    "; of the line. In it, write @ : / ? ; % as %40 %3A %2F %3F %3B %25. NFS version 3:\n"
    "; nfs://server/export or server:/export, no password (options uid and gid, default 0).\n"
    "; Options go after a ?, joined by &: name=... is what Files calls the share.\n"
    "; smb = smb://erik:secret@192.168.1.20/share/media?name=Media\n"
    "; nfs = nfs://nas.local/volume1/books\n"
    "\n"
    "[lock]\n"
    "; A PIN of 4 to 8 digits locks the board when it powers on, and when it wakes from the desk\n"
    "; face. The firmware can be built to ask for it on opening System, Files or the Agent instead, or as well\n"
    "; (INKDECK_PIN_ON). Turn the wheel to pick each digit, then press for the next, like a padlock.\n"
    "; System > PIN LOCK sets, changes and clears it on the board as well. The board keeps the\n"
    "; PIN and replaces this line with a note, so the PIN doesn't stay on the card. To change it\n"
    "; here, write a new pin = line. To remove it, write pin = off. Either change takes effect\n"
    "; once you unlock with the current PIN.\n"
    "; pin = 2468\n"
    "; Also lock after this many minutes without a key (0: only at power-on). Power-on builds only.\n"
    "; lock_minutes = 5\n"
    "\n"
    "[agent]\n"
    "; The machine on your network that runs inkharness, the agent daemon (a name or an\n"
    "; address), its port (7391 unless you changed it) and the token in its inkharness.toml. The\n"
    "; token never leaves this file: it keys the connection's encryption, so make it long and random\n"
    "; (openssl rand -hex 24). With [tailscale] set, agent_host can be the machine's name on your\n"
    "; tailnet (macbook, macbook.tail1234.ts.net) or its 100.x address, and the board reaches it\n"
    "; from any Wi-Fi. A name is tried on your LAN first, so at home the board doesn't join.\n"
    "; agent_host = nas.local\n"
    "; agent_port = 7391\n"
    "; agent_token = something-long\n"
    "; With a PIN on the Agent, the Agent face shows counts only, and its titles stay off the\n"
    "; board's flash. On, it shows everything, for a board that stays on a desk at home.\n"
    "; agent_face_details = off\n"
    "\n"
    "[news]\n"
    "; PWR on a News screen leaves the board off behind the stories of the last 12 hours from the\n"
    "; feeds in SD:/News/feeds.txt, one at a time, with the time and the weather. Minutes each\n"
    "; story stays: 1, 5, 10 or 15.\n"
    "; news_face_every = 5\n"
    "\n"
    "[tailscale]\n"
    "; Puts the board on your tailnet when something needs a machine there: an agent_host, a share's\n"
    "; server or a feed's host given as a tailnet name or 100.x address. Wi-Fi Drive and Update\n"
    "; over Wi-Fi answer on the tailnet too while they're open. Make an auth key in Tailscale's\n"
    "; admin console (Settings > Keys), reusable and not ephemeral: the board is offline most of\n"
    "; the time, and Tailscale removes an ephemeral machine soon after it goes. Once the board shows\n"
    "; up in Machines, turn off its key expiry there. The board keeps the key and replaces this\n"
    "; line with a note; a different key joins it as a new machine. tailscale_key = off removes it.\n"
    "; tailscale_key = tskey-auth-k1234567CNTRL-abcdefghijklmnopqrstuvwxyz\n"
    "; Its name on the tailnet: letters, digits and hyphens (inkdeck unless you change it).\n"
    "; tailscale_name = inkdeck\n"
    "; Offer the board as an exit node, IPv4 only: the tailnet's machines can send their internet\n"
    "; traffic out through its Wi-Fi. Approve it in the admin console (Machines, Edit route\n"
    "; settings); access rules other than the default must allow autogroup:internet. It's there\n"
    "; while the board is on the tailnet, all the time on USB power. Software encryption over\n"
    "; 2.4 GHz Wi-Fi: much slower than a computer would be.\n"
    "; tailscale_exit_node = on\n"
    "\n"
    "[comms]\n"
    "; Talk to other boards over the radio, board to board with no router (COMMS on Home). A group\n"
    "; is a secret its boards share: boards with the same mesh_group line hear each other. Up to 4\n"
    "; lines, one a group. The whole line is the invite: copy it to every board of the group. Make\n"
    "; the secret long and random (openssl rand -hex 16), without a ?. It keys the group's\n"
    "; encryption and stays on the card. Options go after a ?, joined by &:\n"
    ";   name  what the screens call the group (GROUP 1, GROUP 2... without one)\n"
    ";   via   how it travels: air (the radio only), net (through inkharness only; not there\n"
    ";         yet, so nothing is sent) or both (the default). COMMS > GROUPS changes it too.\n"
    ";   hub   where inkharness daemons of different households meet (kept for later)\n"
    "; Without a mesh_group line, a board with an agent_token is in one group, HOME, with the\n"
    "; other boards of that token.\n"
    "; mesh_group = 3f9c0e1b7a2d4c65e8f1a0b9c7d2e4f6?name=Family\n"
    "; mesh_group = another-long-secret?name=Workshop&via=air\n"
    "; This board's name on the other boards, up to 16 letters (its tailscale_name without one,\n"
    "; else INKDECK and 4 digits of its id).\n"
    "; mesh_name = KITCHEN\n"
    "; The Wi-Fi channel the radio uses while the board isn't on a Wi-Fi network: 1 to 13, the\n"
    "; same on every board. On a network it uses the network's channel, so set this to your\n"
    "; Wi-Fi's channel and boards on and off the Wi-Fi still hear each other.\n"
    "; mesh_channel = 6\n"
    "; How this board's mic is keyed on the COMMS screens: sticky (the default: a press of the\n"
    "; wheel switches it on, and it stays on until the next) or push (on only while BOOT is held:\n"
    "; push to talk; the menu's KEEP MIC ON leaves it on, to hear a room).\n"
    "; mesh_mic = push\n"
    "; The rate this board sends at: normal (the default: 1 Mbps) or long (Espressif's long-range\n"
    "; rate, 500 kbps: further, through more walls, but each packet takes twice the air). Every\n"
    "; board hears both, so boards on either setting talk to each other.\n"
    "; mesh_range = long\n"
    "; On battery, instead of powering off, stay reachable: light sleep with the radio off,\n"
    "; waking every few seconds to ask the boards in range whether a ring, voice or a message\n"
    "; waits. off (the default), on (every 5 s) or the seconds between those checks, 2 to 30:\n"
    "; sooner answers, more battery. A sleeping board doesn't sound LOST's alarm. SYSTEM >\n"
    "; ACTIONS > REACHABLE (OFF, 10 S, 30 S) overrides this line until !commsreach - on the console.\n"
    "; mesh_reachable = 10\n";

// The share lines and SMB passwords go in NVS as one string each, a line apiece.
std::vector<std::string> splitLines(const std::string &text) {
  std::vector<std::string> out;
  size_t pos = 0;
  while (pos < text.size()) {
    size_t nl = text.find('\n', pos);
    if (nl == std::string::npos) nl = text.size();
    if (nl > pos) out.push_back(text.substr(pos, nl - pos));
    pos = nl + 1;
  }
  return out;
}

std::string joinLines(const std::vector<std::string> &lines) {
  std::string out;
  for (const std::string &l : lines) out += l + "\n";
  return out;
}

// The same with no '\n' after the last line, for the Wi-Fi ssids and passwords: one network is
// then stored exactly as it was before there could be several, so a firmware from before that
// (after an OTA rollback) still reads it.
std::string joinRecords(const std::vector<std::string> &lines) {
  std::string out;
  for (const std::string &l : lines) out += (out.empty() ? "" : "\n") + l;
  return out;
}

// "smb <url>" / "nfs <url>", for NVS.
std::vector<std::string> shareRecords(const std::vector<ShareLine> &shares) {
  std::vector<std::string> out;
  for (const ShareLine &s : shares) {
    out.push_back((s.kind == ::shares::Kind::Smb ? "smb " : "nfs ") + s.url);
  }
  return out;
}

std::vector<ShareLine> sharesFromRecords(const std::vector<std::string> &records) {
  std::vector<ShareLine> out;
  for (const std::string &r : records) {
    if (r.size() < 5 || r[3] != ' ') continue;
    out.push_back({r.compare(0, 3, "smb") == 0 ? ::shares::Kind::Smb : ::shares::Kind::Nfs,
                   r.substr(4)});
  }
  return out;
}

// One NVS string, or "" without a log line when it was never written (getString() logs an
// error for a missing key).
std::string nvsString(Preferences &prefs, const char *key) {
  return prefs.isKey(key) ? std::string(prefs.getString(key, "").c_str()) : std::string();
}

void loadNvs() {
  if (loaded) return;
  loaded = true;
  Preferences prefs;
  if (!prefs.begin(kNamespace, false)) return;
  card.tz = prefs.getString("tz", "").c_str();
  card.location = prefs.getBool("loc", false);
  card.lat = prefs.getDouble("lat", 0);
  card.lon = prefs.getDouble("lon", 0);
  card.wifiSsids = splitLines(nvsString(prefs, "wssid"));
  card.shares = sharesFromRecords(splitLines(nvsString(prefs, "shares")));
  card.lockMinutes = prefs.getInt("lockmin", 0);
  card.agentHost = nvsString(prefs, "vhost");
  card.agentPort = prefs.getInt("vport", 0);
  card.agentToken = nvsString(prefs, "vtoken");
  card.tailscaleName = nvsString(prefs, "tsname");
  card.tailscaleExitNode = prefs.getBool("tsexit", false);
  card.agentFaceDetails = prefs.getBool("afacedet", false);
  card.newsFaceEvery = prefs.getInt("nfevery", 5);
  card.meshGroups = splitLines(nvsString(prefs, "mgroups"));
  card.meshName = nvsString(prefs, "mname");
  card.meshChannel = prefs.getInt("mchan", 6);
  card.meshMicPush = prefs.getBool("mmic", false);
  card.meshRangeLong = prefs.getBool("mrange", false);
  card.meshReachable = prefs.getInt("mreach", 0);
  tailscaleKeySet = prefs.isKey("tskey");
  // Asked first: getString() logs an error for a missing key, and most boards have no PIN.
  if (prefs.isKey("pin")) pin = prefs.getString("pin", "").c_str();
  prefs.end();
}

void saveNvs() {
  Preferences prefs;
  if (!prefs.begin(kNamespace, false)) return;
  prefs.putString("tz", card.tz.c_str());
  prefs.putBool("loc", card.location);
  prefs.putDouble("lat", card.lat);
  prefs.putDouble("lon", card.lon);
  prefs.putString("wssid", joinRecords(card.wifiSsids).c_str());
  prefs.putString("shares", joinLines(shareRecords(card.shares)).c_str());
  prefs.putInt("lockmin", card.lockMinutes);
  prefs.putString("vhost", card.agentHost.c_str());
  prefs.putInt("vport", card.agentPort);
  prefs.putString("vtoken", card.agentToken.c_str());
  prefs.putString("tsname", card.tailscaleName.c_str());
  prefs.putBool("tsexit", card.tailscaleExitNode);
  prefs.putBool("afacedet", card.agentFaceDetails);
  prefs.putInt("nfevery", card.newsFaceEvery);
  prefs.putString("mgroups", joinLines(card.meshGroups).c_str());
  prefs.putString("mname", card.meshName.c_str());
  prefs.putInt("mchan", card.meshChannel);
  prefs.putBool("mmic", card.meshMicPush);
  prefs.putBool("mrange", card.meshRangeLong);
  prefs.putInt("mreach", card.meshReachable);
  prefs.end();
}

void savePin() {
  Preferences prefs;
  if (!prefs.begin(kNamespace, false)) return;
  prefs.putString("pin", pin.c_str());
  prefs.end();
}

void applyTimezone() {
  setenv("TZ", timezone(), 1);  // see rtc.h: the RTC and the system clock hold UTC
  tzset();
}

// --- POSIX TZ syntax ------------------------------------------------------------------------

bool tzName(const char *&p) {
  if (*p == '<') {
    const char *q = p + 1;
    while (*q && *q != '>') {
      if (!isalnum(static_cast<unsigned char>(*q)) && *q != '+' && *q != '-') return false;
      ++q;
    }
    if (*q != '>' || q - p - 1 < 3) return false;
    p = q + 1;
    return true;
  }
  int n = 0;
  while (isalpha(static_cast<unsigned char>(*p))) {
    ++p;
    ++n;
  }
  return n >= 3;
}

bool digits(const char *&p, int min, int max) {
  int n = 0;
  while (n < max && isdigit(static_cast<unsigned char>(*p))) {
    ++p;
    ++n;
  }
  return n >= min;
}

bool tzOffset(const char *&p) {  // [+-]h[h][:mm[:ss]]
  if (*p == '+' || *p == '-') ++p;
  if (!digits(p, 1, 2)) return false;
  for (int i = 0; i < 2 && *p == ':'; ++i) {
    ++p;
    if (!digits(p, 2, 2)) return false;
  }
  return true;
}

bool tzRule(const char *&p) {  // Mm.w.d, Jn or n, then an optional /time
  if (*p == 'M') {
    ++p;
    if (!digits(p, 1, 2) || *p++ != '.' || !digits(p, 1, 1) || *p++ != '.' || !digits(p, 1, 1)) {
      return false;
    }
  } else {
    if (*p == 'J') ++p;
    if (!digits(p, 1, 3)) return false;
  }
  if (*p == '/') {
    ++p;
    if (!tzOffset(p)) return false;
  }
  return true;
}

// --- The file ------------------------------------------------------------------------------

using text::trim;

bool readFile(const char *path, std::string &out) {
  FILE *f = fopen(path, "rb");
  if (!f) return false;
  char buf[512];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0 && out.size() < 16 * 1024) out.append(buf, n);
  fclose(f);
  return true;
}

bool writeFile(const char *path, const std::string &text) {
  FILE *f = fopen(path, "wb");
  if (!f) return false;
  // From RAM: a write of a sector or more from flash reaches the card as zeros (AGENTS.md,
  // Gotchas).
  const bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
  return fclose(f) == 0 && ok;
}

// One line of the file as key (lower case) and value: false for blank lines, comments and
// [sections]. A line without '=' gives an empty key.
bool splitLine(std::string line, std::string &key, std::string &value) {
  if (line.compare(0, 3, "\xEF\xBB\xBF") == 0) line.erase(0, 3);  // Notepad's BOM
  if (!line.empty() && line.back() == '\r') line.pop_back();
  line = trim(line);
  key.clear();
  value.clear();
  if (line.empty() || line[0] == ';' || line[0] == '#' || line[0] == '[') return false;
  const size_t eq = line.find('=');
  if (eq == std::string::npos) return true;
  key = trim(line.substr(0, eq));
  key = text::lower(std::move(key));
  value = trim(line.substr(eq + 1));
  if (!value.empty() && (value[0] == '"' || value[0] == '\'')) {
    const size_t close = value.find(value[0], 1);  // quoted: anything after it is a comment
    if (close != std::string::npos) {
      value = value.substr(1, close - 1);
      return true;
    }
  }
  const size_t comment = value.find(" ;");  // a comment after the value
  if (comment != std::string::npos) value = trim(value.substr(0, comment));
  return true;
}

bool validPin(const std::string &s) {
  if (s.size() < 4 || s.size() > 8) return false;
  for (const char c : s) {
    if (!isdigit(static_cast<unsigned char>(c))) return false;
  }
  return true;
}

// 1 to 32 bytes (UTF-8 is fine). No control characters: NVS keeps an ssid on a line of its own,
// before a tab.
bool validSsid(const std::string &s) {
  if (s.empty() || s.size() > 32) return false;
  return std::none_of(s.begin(), s.end(), [](char c) {
    return static_cast<unsigned char>(c) < 0x20 || c == 0x7F;
  });
}

// Empty (an open network), a WPA passphrase of 8 to 63 bytes or a 64-hex-digit key, as
// scripts/gen_secrets.py takes for .env. No control characters, as for an ssid.
bool validWifiPassword(const std::string &s) {
  if (s.empty()) return true;
  if (std::any_of(s.begin(), s.end(), [](char c) {
        return static_cast<unsigned char>(c) < 0x20 || c == 0x7F;
      })) {
    return false;
  }
  if (s.size() >= 8 && s.size() <= 63) return true;
  return s.size() == 64 && std::all_of(s.begin(), s.end(), [](char c) {
           return isxdigit(static_cast<unsigned char>(c));
         });
}

bool isSsidKey(const std::string &key) { return key == "ssid" || key == "wifi_ssid"; }
bool isWifiPasswordKey(const std::string &key) {
  return key == "password" || key == "wifi_password";
}

// Sets id's password in a list of them, replacing one it had.
void setSecret(Secrets &secrets, const std::string &id, const std::string &password) {
  for (auto &[known, value] : secrets) {
    if (known == id) {
      value = password;
      return;
    }
  }
  secrets.push_back({id, password});
}

const std::string *findSecret(const Secrets &secrets, const std::string &id) {
  for (const auto &[known, value] : secrets) {
    if (known == id) return &value;
  }
  return nullptr;
}

// A machine name on a tailnet: letters, digits and hyphens, not at either end, up to 63.
bool validTailscaleName(const std::string &s) {
  if (s.empty() || s.size() > 63 || s.front() == '-' || s.back() == '-') return false;
  return std::all_of(s.begin(), s.end(), [](char c) {
    return isalnum(static_cast<unsigned char>(c)) || c == '-';
  });
}

// An auth key as the admin console gives it (tskey-auth-...), or a Headscale one: printable,
// no spaces.
bool validTailscaleKey(const std::string &s) {
  if (s.size() < 8 || s.size() > 200) return false;
  return std::all_of(s.begin(), s.end(), [](char c) { return c > 0x20 && c < 0x7F; });
}

bool isOff(std::string s) {
  s = text::lower(std::move(s));
  return s == "off";
}

// on / off, or yes / no, true / false, 1 / 0. False if it's none of them.
bool parseOnOff(std::string s, bool &out) {
  s = text::lower(std::move(s));
  if (s == "on" || s == "yes" || s == "true" || s == "1") {
    out = true;
  } else if (s == "off" || s == "no" || s == "false" || s == "0") {
    out = false;
  } else {
    return false;
  }
  return true;
}

bool parseDegrees(const std::string &text, double limit, double &out) {
  char *end = nullptr;
  const double v = strtod(text.c_str(), &end);
  if (text.empty() || *end != '\0' || !isfinite(v) || fabs(v) > limit) return false;
  out = v;
  return true;
}

bool printable(const std::string &s) {
  return std::none_of(s.begin(), s.end(), [](char c) {
    return static_cast<unsigned char>(c) < 0x20 || c == 0x7F;
  });
}

// A mesh_group line's value: <secret>?name=...&via=...&hub=... (settings.h). False, with the
// reason in error, if it isn't one.
bool parseMeshGroup(const std::string &value, MeshGroup &out, std::string &error) {
  out = MeshGroup();
  const size_t q = value.find('?');
  out.secret = trim(value.substr(0, q));
  if (out.secret.size() < 8 || out.secret.size() > 128 || !printable(out.secret)) {
    error = "the secret must be 8 to 128 characters (make it long and random)";
    return false;
  }
  const std::string query = q == std::string::npos ? "" : value.substr(q + 1);
  for (size_t pos = 0; pos < query.size();) {
    size_t amp = query.find('&', pos);
    if (amp == std::string::npos) amp = query.size();
    const std::string item = query.substr(pos, amp - pos);
    pos = amp + 1;
    if (item.empty()) continue;
    const size_t eq = item.find('=');
    const std::string key = text::lower(item.substr(0, eq));
    const std::string v =
        eq == std::string::npos ? "" : trim(text::percentDecode(item.substr(eq + 1)));
    if (key == "name") {
      if (v.empty() || v.size() > 16 || !printable(v)) {
        error = "name must be 1 to 16 characters";
        return false;
      }
      out.name = v;
    } else if (key == "via") {
      const std::string via = text::lower(v);
      if (via == "both" || via == "air+net") {
        out.via = MeshVia::Both;
      } else if (via == "air") {
        out.via = MeshVia::Air;
      } else if (via == "net") {
        out.via = MeshVia::Net;
      } else {
        error = "via must be air, net or both";
        return false;
      }
    } else if (key == "hub") {
      out.hub = v;
    } else {
      error = "unknown option \"" + key + "\" (name, via, hub)";
      return false;
    }
  }
  out.line = text::fnv1a(value);
  return true;
}

Values parse(const std::string &text, CardPin &cardPin, CardTailscaleKey &cardKey,
             CardSecrets &secrets) {
  Values v;
  cardPin = {};
  cardKey = {};
  secrets = {};
  bool hasLat = false, hasLon = false;
  double lat = 0, lon = 0;
  int wifiNet = -1;  // the network in v.wifiSsids a password line belongs to; -1: none
  size_t pos = 0;
  int lineNo = 0;
  while (pos < text.size()) {
    size_t nl = text.find('\n', pos);
    if (nl == std::string::npos) nl = text.size();
    const std::string line = text.substr(pos, nl - pos);
    pos = nl + 1;
    ++lineNo;
    std::string key, value;
    if (!splitLine(line, key, value)) continue;
    if (key.empty()) {
      log_w("settings: line %d of %s isn't key = value", lineNo, kFile);
      continue;
    }
    if (key == "voice_host" || key == "voice_port" || key == "voice_token") {
      // Before the Agent, these were the voice assistant's: a card from then still works.
      log_w("settings: %s: %s is now agent_%s", kFile, key.c_str(), key.c_str() + 6);
      key = "agent_" + key.substr(6);
    }
    if (key == "timezone") {
      if (validTimezone(value)) {
        v.tz = value;
      } else {
        log_w("settings: %s: \"%s\" isn't a POSIX TZ string; using the built-in time zone",
              kFile, value.c_str());
      }
    } else if (key == "latitude") {
      hasLat = parseDegrees(value, 90, lat);
      if (!hasLat) log_w("settings: %s: latitude must be -90 to 90", kFile);
    } else if (key == "longitude") {
      hasLon = parseDegrees(value, 180, lon);
      if (!hasLon) log_w("settings: %s: longitude must be -180 to 180", kFile);
    } else if (key == "smb" || key == "nfs") {
      const ::shares::Kind kind = key == "smb" ? ::shares::Kind::Smb : ::shares::Kind::Nfs;
      ::shares::Share share;
      std::string password, error;
      const std::string shown = ::shares::withoutPassword(value);  // for the log
      if (!::shares::parse(kind, value, share, password, error)) {
        log_w("settings: %s: %s \"%s\": %s", kFile, key.c_str(), shown.c_str(), error.c_str());
      } else if (static_cast<int>(v.shares.size()) >= ::shares::kMaxShares) {
        log_w("settings: %s: only %d shares; \"%s\" left out", kFile, ::shares::kMaxShares,
              shown.c_str());
      } else {
        v.shares.push_back({kind, kind == ::shares::Kind::Smb ? shown : value});
        if (!password.empty()) secrets.smb.push_back({share.credentialId(), password});
      }
    } else if (isSsidKey(key)) {
      wifiNet = -1;
      const auto known = std::find(v.wifiSsids.begin(), v.wifiSsids.end(), value);
      if (!validSsid(value)) {
        log_w("settings: %s: line %d: an ssid must be 1 to 32 bytes", kFile, lineNo);
      } else if (known != v.wifiSsids.end()) {
        log_w("settings: %s: ssid %s is listed twice; it keeps its first place", kFile,
              value.c_str());
        wifiNet = static_cast<int>(known - v.wifiSsids.begin());
      } else if (static_cast<int>(v.wifiSsids.size()) >= kMaxWifiNetworks) {
        log_w("settings: %s: only %d Wi-Fi networks; %s left out", kFile, kMaxWifiNetworks,
              value.c_str());
      } else {
        v.wifiSsids.push_back(value);
        wifiNet = static_cast<int>(v.wifiSsids.size()) - 1;
      }
    } else if (isWifiPasswordKey(key)) {
      if (wifiNet < 0) {
        log_w("settings: %s: line %d: a Wi-Fi password with no ssid line above it", kFile, lineNo);
      } else if (!validWifiPassword(value)) {
        log_w("settings: %s: the password for %s must be empty, 8 to 63 characters or 64 hex "
              "digits; left on the card",
              kFile, v.wifiSsids[wifiNet].c_str());
      } else {
        setSecret(secrets.wifi, v.wifiSsids[wifiNet], value);
      }
    } else if (key == "pin") {
      if (isOff(value)) {
        cardPin = {CardPin::Off, {}};
      } else if (validPin(value)) {
        cardPin = {CardPin::Digits, value};
      } else {
        log_w("settings: %s: pin must be 4 to 8 digits, or off", kFile);
      }
    } else if (key == "lock_minutes") {
      char *end = nullptr;
      const long m = strtol(value.c_str(), &end, 10);
      if (value.empty() || *end != '\0' || m < 0 || m > kMaxLockMinutes) {
        log_w("settings: %s: lock_minutes must be 0 to %d", kFile, kMaxLockMinutes);
      } else {
        v.lockMinutes = static_cast<int>(m);
      }
    } else if (key == "agent_host") {
      v.agentHost = value;
    } else if (key == "agent_port") {
      char *end = nullptr;
      const long port = strtol(value.c_str(), &end, 10);
      if (value.empty() || *end != '\0' || port < 1 || port > 65535) {
        log_w("settings: %s: agent_port must be 1 to 65535", kFile);
      } else {
        v.agentPort = static_cast<int>(port);
      }
    } else if (key == "agent_token") {
      v.agentToken = value;
    } else if (key == "tailscale_key") {
      if (isOff(value)) {
        cardKey = {CardTailscaleKey::Off, {}};
      } else if (validTailscaleKey(value)) {
        cardKey = {CardTailscaleKey::Key, value};
      } else {
        log_w("settings: %s: tailscale_key isn't an auth key (tskey-auth-...), or off", kFile);
      }
    } else if (key == "tailscale_name") {
      std::string name = value;
      name = text::lower(std::move(name));
      if (validTailscaleName(name)) {
        v.tailscaleName = name;
      } else {
        log_w("settings: %s: tailscale_name must be letters, digits and hyphens, up to 63", kFile);
      }
    } else if (key == "agent_face_details") {
      if (!parseOnOff(value, v.agentFaceDetails)) {
        log_w("settings: %s: agent_face_details must be on or off", kFile);
      }
    } else if (key == "news_face_every") {
      int n = 0, used = 0;
      if (siscanf(value.c_str(), "%d%n", &n, &used) == 1 &&
          used == static_cast<int>(value.size()) && (n == 1 || n == 5 || n == 10 || n == 15)) {
        v.newsFaceEvery = n;
      } else {
        log_w("settings: %s: news_face_every must be 1, 5, 10 or 15", kFile);
      }
    } else if (key == "tailscale_exit_node") {
      if (!parseOnOff(value, v.tailscaleExitNode)) {
        log_w("settings: %s: tailscale_exit_node must be on or off", kFile);
      }
    } else if (key == "mesh_group") {
      MeshGroup group;
      std::string error;
      if (!parseMeshGroup(value, group, error)) {
        log_w("settings: %s: line %d: mesh_group: %s", kFile, lineNo, error.c_str());
      } else if (static_cast<int>(v.meshGroups.size()) >= kMaxMeshGroups) {
        log_w("settings: %s: only %d mesh_group lines; line %d left out", kFile, kMaxMeshGroups,
              lineNo);
      } else {
        v.meshGroups.push_back(value);
      }
    } else if (key == "mesh_name") {
      if (value.empty() || value.size() > 16 || !printable(value)) {
        log_w("settings: %s: mesh_name must be 1 to 16 characters", kFile);
      } else {
        v.meshName = value;
      }
    } else if (key == "mesh_channel") {
      int n = 0, used = 0;
      if (siscanf(value.c_str(), "%d%n", &n, &used) == 1 &&
          used == static_cast<int>(value.size()) && n >= 1 && n <= 13) {
        v.meshChannel = n;
      } else {
        log_w("settings: %s: mesh_channel must be 1 to 13", kFile);
      }
    } else if (key == "mesh_mic") {
      // sticky or push; anything else is sticky.
      const std::string mode = text::lower(value);
      v.meshMicPush = mode == "push";
      if (!v.meshMicPush && mode != "sticky") {
        log_w("settings: %s: mesh_mic must be sticky or push; sticky it is", kFile);
      }
    } else if (key == "mesh_range") {
      // normal or long; anything else is normal.
      const std::string range = text::lower(value);
      v.meshRangeLong = range == "long";
      if (!v.meshRangeLong && range != "normal") {
        log_w("settings: %s: mesh_range must be normal or long; normal it is", kFile);
      }
    } else if (key == "mesh_reachable") {
      // off, on (5 s) or the seconds between polls, 2 to 30.
      const std::string v2 = text::lower(value);
      int n = 0, used = 0;
      if (v2 == "off") {
        v.meshReachable = 0;
      } else if (v2 == "on") {
        v.meshReachable = 5;
      } else if (siscanf(v2.c_str(), "%d%n", &n, &used) == 1 &&
                 used == static_cast<int>(v2.size()) && n >= 2 && n <= 30) {
        v.meshReachable = n;
      } else {
        log_w("settings: %s: mesh_reachable must be off, on or 2 to 30 (seconds); off it is",
              kFile);
      }
    } else {
      log_w("settings: %s: unknown setting \"%s\"", kFile, key.c_str());
    }
  }
  if (hasLat && hasLon) {
    v.location = true;
    v.lat = lat;
    v.lon = lon;
  } else if (hasLat || hasLon) {
    log_w("settings: %s: set both latitude and longitude; using the built-in location", kFile);
  }
  return v;
}

// Rewrites lines of the card's file: replace(key, value, out) puts what a line becomes in out
// and returns true, or false to keep it. what names the lines, for the log. Without the card
// the next boot does it.
template <typename Replace>
void rewriteCardLines(const char *what, Replace replace) {
  if (!sdcard::mounted()) return;
  std::string text;
  if (!readFile(kFile, text)) return;
  std::string out;
  out.reserve(text.size() + 64);
  bool changed = false;
  size_t pos = 0;
  while (pos < text.size()) {
    const size_t nl = text.find('\n', pos);
    const size_t end = nl == std::string::npos ? text.size() : nl;
    const std::string line = text.substr(pos, end - pos);
    std::string key, value, replacement;
    if (splitLine(line, key, value) && replace(key, value, replacement)) {
      if (line.compare(0, 3, "\xEF\xBB\xBF") == 0) out += "\xEF\xBB\xBF";
      out += replacement;
      if (!line.empty() && line.back() == '\r') out += '\r';
      changed = true;
    } else {
      out += line;
    }
    if (nl != std::string::npos) out += '\n';
    pos = end + 1;
  }
  if (!changed) return;
  if (writeFile(kFile, out)) {
    log_i("settings: rewrote the %s in %s", what, kFile);
  } else {
    log_w("settings: couldn't rewrite %s; its %s stay", kFile, what);
  }
}

// Replaces each pin = <digits> or pin = off line in the card's file with kPinNote, once the
// board has acted on it: so the PIN doesn't stay on the card, and an old pin = off can't undo a
// PIN set later in System. Without the card (it can come out between boot and unlock) the next
// boot does it.
void hidePinLines() {
  rewriteCardLines("pin line", [](const std::string &key, const std::string &value,
                                  std::string &out) {
    if (key != "pin" || !(validPin(value) || isOff(value))) return false;
    out = kPinNote;
    return true;
  });
}

// What the card's tailscale_key line asks for: the board keeps the key (or forgets it), and the
// line becomes kTailscaleKeyNote either way, so the key doesn't stay on the card.
void applyCardTailscaleKey(const CardTailscaleKey &c) {
  if (c.kind == CardTailscaleKey::Absent) return;
  Preferences prefs;
  if (!prefs.begin(kNamespace, false)) return;
  if (c.kind == CardTailscaleKey::Key) {
    if (nvsString(prefs, "tskey") != c.key) {
      prefs.putString("tskey", c.key.c_str());
      log_i("settings: Tailscale auth key kept on the board");
    }
    tailscaleKeySet = true;
  } else {
    if (prefs.isKey("tskey")) {
      prefs.remove("tskey");
      log_i("settings: Tailscale auth key removed, as the card asked");
    }
    tailscaleKeySet = false;
  }
  prefs.end();
  rewriteCardLines("tailscale_key line", [](const std::string &key, const std::string &value,
                                            std::string &out) {
    if (key != "tailscale_key" || !(validTailscaleKey(value) || isOff(value))) return false;
    out = kTailscaleKeyNote;
    return true;
  });
}

// --- Passwords kept on the board -------------------------------------------------------------

// NVS: "wpass" is a "<ssid>\t<password>" line per Wi-Fi network (one, with no '\n', before there
// could be several), "smbpw" a "<credentialId>\t<password>" line per share.
Secrets storedSecrets(const char *key) {
  Preferences prefs;
  std::string v;
  if (prefs.begin(kNamespace, true)) {
    v = nvsString(prefs, key);
    prefs.end();
  }
  Secrets out;
  for (const std::string &line : splitLines(v)) {
    const size_t tab = line.find('\t');
    if (tab != std::string::npos) out.push_back({line.substr(0, tab), line.substr(tab + 1)});
  }
  return out;
}

// The "<id>\t<password>" records to keep: the card's, and the stored ones the card didn't
// replace whose id it still names.
std::vector<std::string> keptSecrets(const Secrets &stored, const Secrets &fromCard,
                                     const std::vector<std::string> &inUse) {
  std::vector<std::string> keep;
  for (const auto &[id, password] : stored) {
    if (!findSecret(fromCard, id) && std::find(inUse.begin(), inUse.end(), id) != inUse.end()) {
      keep.push_back(id + "\t" + password);
    }
  }
  for (const auto &[id, password] : fromCard) keep.push_back(id + "\t" + password);
  return keep;
}

void putOrRemove(Preferences &prefs, const char *key, const std::string &value) {
  if (value == nvsString(prefs, key)) return;
  if (value.empty()) {
    prefs.remove(key);
  } else {
    prefs.putString(key, value.c_str());
  }
}

// Keeps the passwords the card gave (and drops ones nothing names any more), then takes them
// off the card: a Wi-Fi password line becomes a note, an smb URL loses its :password.
void applyCardSecrets(const CardSecrets &s) {
  // Wi-Fi: each password belongs to the ssid line above it.
  const Secrets storedWifi = storedSecrets("wpass");
  for (const auto &[ssid, password] : s.wifi) {
    const std::string *had = findSecret(storedWifi, ssid);
    if (!had || *had != password) {
      log_i("settings: Wi-Fi password for %s kept on the board", ssid.c_str());
    }
  }
  for (const std::string &ssid : card.wifiSsids) {
    if (!findSecret(s.wifi, ssid) && !findSecret(storedWifi, ssid)) {
      log_i("settings: no Wi-Fi password for %s; joining it as an open network", ssid.c_str());
    }
  }
  const std::string wifi = joinRecords(keptSecrets(storedWifi, s.wifi, card.wifiSsids));

  // SMB: the card's passwords, and the stored ones the card's shares still name.
  std::vector<std::string> inUse;
  for (const ShareLine &line : card.shares) {
    ::shares::Share share;
    std::string password, error;
    if (line.kind == ::shares::Kind::Smb &&
        ::shares::parse(line.kind, line.url, share, password, error)) {
      inUse.push_back(share.credentialId());
    }
  }
  const std::string smb = joinLines(keptSecrets(storedSecrets("smbpw"), s.smb, inUse));

  Preferences prefs;
  if (!prefs.begin(kNamespace, false)) return;
  putOrRemove(prefs, "wpass", wifi);
  putOrRemove(prefs, "smbpw", smb);
  prefs.end();

  if (s.wifi.empty() && s.smb.empty()) return;
  bool wifiListed = false;  // the password lines here belong to a network the card lists
  rewriteCardLines("passwords", [&](const std::string &key, const std::string &value,
                                    std::string &out) {
    // The same pairing as parse(): only a password line it took is hidden.
    if (isSsidKey(key)) {
      wifiListed = std::find(card.wifiSsids.begin(), card.wifiSsids.end(), value) !=
                   card.wifiSsids.end();
      return false;
    }
    if (isWifiPasswordKey(key) && wifiListed && validWifiPassword(value)) {
      out = kWifiPasswordNote;
      return true;
    }
    if (key == "smb") {
      const std::string bare = ::shares::withoutPassword(value);
      if (bare == value) return false;
      out = "smb = " + bare + kSmbPasswordNote;
      return true;
    }
    return false;
  });
}

// What the card's pin line asks for (settings.h has the rules).
void applyCardPin(const CardPin &c) {
  pending = PinChange::None;
  pendingDigits.clear();
  switch (c.kind) {
    case CardPin::Absent: return;
    case CardPin::Off:
      if (pin.empty()) {
        hidePinLines();  // nothing to remove
      } else {
        pending = PinChange::Off;
        log_i("settings: the card removes the PIN once the lock is opened");
      }
      return;
    case CardPin::Digits:
      if (pin.empty()) {
        pin = c.digits;
        savePin();
        log_i("settings: PIN set from the card");
        hidePinLines();
      } else if (c.digits == pin) {
        hidePinLines();
      } else {
        pending = PinChange::New;
        pendingDigits = c.digits;
        log_i("settings: the card sets a new PIN once the lock is opened");
      }
      return;
  }
}

}  // namespace

void begin() {
  loadNvs();
  applyTimezone();
}

void loadCard() {
  if (!sdcard::mounted()) return;  // keep what the last card set
  loadNvs();
  std::string text;
  if (!readFile(kFile, text)) {
    if (writeFile(kFile, kTemplate)) {
      log_i("settings: wrote a commented %s", kFile);
    } else {
      log_w("settings: couldn't write %s", kFile);
    }
  }
  CardPin cardPin;
  CardTailscaleKey cardKey;
  CardSecrets secrets;
  const Values next = parse(text, cardPin, cardKey, secrets);
  if (!(next == card)) {
    card = next;
    saveNvs();
  }
  applyCardSecrets(secrets);
  applyCardPin(cardPin);
  applyCardTailscaleKey(cardKey);
  if (card.lockMinutes > 0 && pin.empty()) {
    log_w("settings: %s: lock_minutes does nothing without a pin", kFile);
  }
  applyTimezone();
  const std::string wifi = card.wifiSsids.empty()
                               ? std::string("built in")
                               : std::to_string(card.wifiSsids.size()) + " from the card";
  log_i("settings: Wi-Fi %s, time zone %s, location %s, %u shares", wifi.c_str(),
        card.tz.empty() ? "built in" : "from the card",
        card.location ? "from the card" : hasLocation() ? "built in" : "not set",
        static_cast<unsigned>(card.shares.size()));
}

const char *timezone() {
  loadNvs();
  return card.tz.empty() ? TIMEZONE : card.tz.c_str();
}

Source timezoneSource() {
  loadNvs();
  return card.tz.empty() ? Source::Built : Source::Card;
}

bool hasLocation() {
  loadNvs();
  return card.location || LOCATION_CONFIGURED;
}

double latitude() {
  loadNvs();
  return card.location ? card.lat : LATITUDE;
}

double longitude() {
  loadNvs();
  return card.location ? card.lon : LONGITUDE;
}

Source locationSource() {
  loadNvs();
  return card.location ? Source::Card : Source::Built;
}

bool hasWifi() {
  loadNvs();
  return !card.wifiSsids.empty() || WIFI_CONFIGURED;
}

std::vector<WifiNetwork> wifiNetworks() {
  loadNvs();
  if (card.wifiSsids.empty()) {
    if (!WIFI_CONFIGURED) return {};
    return {{WIFI_SSID, WIFI_PASSWORD}};
  }
  const Secrets stored = storedSecrets("wpass");
  std::vector<WifiNetwork> out;
  for (const std::string &ssid : card.wifiSsids) {
    const std::string *password = findSecret(stored, ssid);
    out.push_back({ssid, password ? *password : std::string()});
  }
  return out;
}

std::vector<std::string> wifiSsids() {
  loadNvs();
  if (!card.wifiSsids.empty()) return card.wifiSsids;
  if (!WIFI_CONFIGURED) return {};
  return {WIFI_SSID};
}

Source wifiSource() {
  loadNvs();
  return card.wifiSsids.empty() ? Source::Built : Source::Card;
}

std::vector<ShareLine> shareLines() {
  loadNvs();
  return card.shares;
}

std::string sharePassword(const std::string &credentialId) {
  const Secrets stored = storedSecrets("smbpw");
  const std::string *password = findSecret(stored, credentialId);
  return password ? *password : std::string();
}

bool hasPin() {
  loadNvs();
  return !pin.empty();
}

int pinLength() {
  loadNvs();
  return static_cast<int>(pin.size());
}

bool pinMatches(const std::string &digits) {
  loadNvs();
  return !pin.empty() && digits == pin;
}

int lockMinutes() {
  loadNvs();
  return card.lockMinutes;
}

bool hasAgent() {
  loadNvs();
  return !card.agentHost.empty();
}

std::string agentHost() {
  loadNvs();
  return card.agentHost;
}

int agentPort() {
  loadNvs();
  return card.agentPort > 0 ? card.agentPort : 7391;
}

std::string agentToken() {
  loadNvs();
  return card.agentToken;
}

bool hasTailscale() {
  loadNvs();
  return tailscaleKeySet;
}

std::string tailscaleKey() {
  Preferences prefs;
  std::string key;
  if (prefs.begin(kNamespace, true)) {
    key = nvsString(prefs, "tskey");
    prefs.end();
  }
  return key;
}

std::string tailscaleName() {
  loadNvs();
  return card.tailscaleName.empty() ? "inkdeck" : card.tailscaleName;
}

bool tailscaleExitNode() {
  loadNvs();
  return card.tailscaleExitNode;
}

bool agentFaceDetails() {
  loadNvs();
  return card.agentFaceDetails;
}

int newsFaceEvery() {
  loadNvs();
  return card.newsFaceEvery;
}

std::vector<MeshGroup> meshGroups() {
  loadNvs();
  std::vector<MeshGroup> out;
  for (const std::string &line : card.meshGroups) {
    MeshGroup g;
    std::string error;
    if (parseMeshGroup(line, g, error)) out.push_back(std::move(g));
  }
  return out;
}

std::string meshName() {
  loadNvs();
  return card.meshName;
}

int meshChannel() {
  loadNvs();
  return card.meshChannel >= 1 && card.meshChannel <= 13 ? card.meshChannel : 6;
}

MeshMic meshMic() {
  loadNvs();
  return card.meshMicPush ? MeshMic::Push : MeshMic::Sticky;
}

MeshRange meshRange() {
  loadNvs();
  return card.meshRangeLong ? MeshRange::Long : MeshRange::Normal;
}

int meshReachable() {
  loadNvs();
  return card.meshReachable >= 2 && card.meshReachable <= 30 ? card.meshReachable : 0;
}

PinChange pendingPin() { return pending; }

void unlocked() {
  switch (pending) {
    case PinChange::None: return;
    case PinChange::Off:
      pin.clear();
      savePin();
      log_i("settings: PIN removed, as the card asked");
      break;
    case PinChange::New:
      pin = pendingDigits;
      savePin();
      log_i("settings: new PIN from the card");
      break;
  }
  hidePinLines();
  pending = PinChange::None;
  pendingDigits.clear();
}

void setPin(const std::string &digits) {
  if (!validPin(digits)) return;
  loadNvs();
  pin = digits;
  savePin();
  pending = PinChange::None;
  pendingDigits.clear();
  log_i("settings: PIN set in System");
}

int pinMisses() {
  Preferences prefs;
  if (!prefs.begin(kNamespace, true)) return 0;  // read-only; fails until the namespace exists
  const int n = prefs.getInt("pinmiss", 0);
  prefs.end();
  return n;
}

void setPinMisses(int n) {
  Preferences prefs;
  if (!prefs.begin(kNamespace, false)) return;
  prefs.putInt("pinmiss", n);
  prefs.end();
}

void clearPin() {
  loadNvs();
  pin.clear();
  savePin();
  pending = PinChange::None;
  pendingDigits.clear();
  log_i("settings: PIN cleared in System");
}

std::string describe() {
  std::string what;
  auto add = [&](bool fromCard, const char *name) {
    if (!fromCard) return;
    if (!what.empty()) what += ", ";
    what += name;
  };
  add(wifiSource() == Source::Card, "WI-FI");
  add(timezoneSource() == Source::Card, "TZ");
  add(locationSource() == Source::Card, "LOCATION");
  return what.empty() ? ".env (built in)" : "SD:/inkdeck.ini \xC2\xB7 " + what;
}

bool validTimezone(const std::string &tz) {
  const char *p = tz.c_str();
  if (!tzName(p) || !tzOffset(p)) return false;
  if (!*p) return true;
  if (!tzName(p)) return false;
  if (*p && *p != ',' && !tzOffset(p)) return false;
  if (!*p) return true;
  if (*p++ != ',' || !tzRule(p) || *p++ != ',' || !tzRule(p)) return false;
  return *p == '\0';
}

}  // namespace settings
