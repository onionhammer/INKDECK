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
  std::string voiceHost;
  int voicePort = 0;  // 0: the default
  std::string voiceToken;

  bool operator==(const Values &o) const {
    return tz == o.tz && location == o.location && lat == o.lat && lon == o.lon &&
           wifiSsids == o.wifiSsids && shares == o.shares && lockMinutes == o.lockMinutes &&
           voiceHost == o.voiceHost && voicePort == o.voicePort && voiceToken == o.voiceToken;
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

Values card;
bool loaded = false;
std::string pin;  // the board's PIN (NVS); empty: no lock
PinChange pending = PinChange::None;
std::string pendingDigits;  // the PIN a pending PinChange::New brings

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
    "; face. The firmware can be built to ask for it on opening System or Files instead, or as well\n"
    "; (INKDECK_PIN_ON). Turn the wheel to pick each digit, then press for the next, like a padlock.\n"
    "; System > PIN LOCK sets, changes and clears it on the board as well. The board keeps the\n"
    "; PIN and replaces this line with a note, so the PIN doesn't stay on the card. To change it\n"
    "; here, write a new pin = line. To remove it, write pin = off. Either change takes effect\n"
    "; once you unlock with the current PIN.\n"
    "; pin = 2468\n"
    "; Also lock after this many minutes without a key (0: only at power-on). Power-on builds only.\n"
    "; lock_minutes = 5\n"
    "\n"
    "[voice]\n"
    "; The machine on your network that runs inkharness, the assistant daemon (a name or an\n"
    "; address), its port (7391 unless you changed it) and the token its inkharness.toml asks for.\n"
    "; The token stays in this file.\n"
    "; voice_host = nas.local\n"
    "; voice_port = 7391\n"
    "; voice_token = something-long\n";

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
  card.voiceHost = nvsString(prefs, "vhost");
  card.voicePort = prefs.getInt("vport", 0);
  card.voiceToken = nvsString(prefs, "vtoken");
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
  prefs.putString("vhost", card.voiceHost.c_str());
  prefs.putInt("vport", card.voicePort);
  prefs.putString("vtoken", card.voiceToken.c_str());
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

std::string trim(const std::string &s) {
  const size_t b = s.find_first_not_of(" \t");
  if (b == std::string::npos) return {};
  return s.substr(b, s.find_last_not_of(" \t") - b + 1);
}

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
  for (char &c : key) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
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

bool isOff(std::string s) {
  for (char &c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
  return s == "off";
}

bool parseDegrees(const std::string &text, double limit, double &out) {
  char *end = nullptr;
  const double v = strtod(text.c_str(), &end);
  if (text.empty() || *end != '\0' || !isfinite(v) || fabs(v) > limit) return false;
  out = v;
  return true;
}

Values parse(const std::string &text, CardPin &cardPin, CardSecrets &secrets) {
  Values v;
  cardPin = {};
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
    } else if (key == "voice_host") {
      v.voiceHost = value;
    } else if (key == "voice_port") {
      char *end = nullptr;
      const long port = strtol(value.c_str(), &end, 10);
      if (value.empty() || *end != '\0' || port < 1 || port > 65535) {
        log_w("settings: %s: voice_port must be 1 to 65535", kFile);
      } else {
        v.voicePort = static_cast<int>(port);
      }
    } else if (key == "voice_token") {
      v.voiceToken = value;
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
    if (FILE *f = fopen(kFile, "wb")) {
      // From RAM: a write of a sector or more from flash reaches the card as zeros (AGENTS.md,
      // Gotchas).
      const std::string text(kTemplate);
      fwrite(text.data(), 1, text.size(), f);
      fclose(f);
      log_i("settings: wrote a commented %s", kFile);
    }
  }
  CardPin cardPin;
  CardSecrets secrets;
  const Values next = parse(text, cardPin, secrets);
  if (!(next == card)) {
    card = next;
    saveNvs();
  }
  applyCardSecrets(secrets);
  applyCardPin(cardPin);
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

bool hasVoice() {
  loadNvs();
  return !card.voiceHost.empty();
}

std::string voiceHost() {
  loadNvs();
  return card.voiceHost;
}

int voicePort() {
  loadNvs();
  return card.voicePort > 0 ? card.voicePort : 7391;
}

std::string voiceToken() {
  loadNvs();
  return card.voiceToken;
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
