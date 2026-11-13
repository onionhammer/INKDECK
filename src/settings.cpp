#include "settings.h"

#include <Arduino.h>
#include <Preferences.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "inkdeck_secrets.h"  // TIMEZONE, LOCATION_CONFIGURED, LATITUDE, LONGITUDE
#include "sdcard.h"

namespace settings {
namespace {

constexpr const char *kNamespace = "settings";

// What the card set; empty / false means the built-in value is used.
struct Values {
  std::string tz;
  bool location = false;
  double lat = 0;
  double lon = 0;

  bool operator==(const Values &o) const {
    return tz == o.tz && location == o.location && lat == o.lat && lon == o.lon;
  }
};

Values card;
bool loaded = false;

constexpr const char *kTemplate =
    "; INKDECK settings. A value here overrides the one built into the firmware from .env.\n"
    "; Remove the ; in front of a line to use it. Read when the board starts.\n"
    "; Wi-Fi isn't read from this file: a card is easy to lift, so it stays in the firmware.\n"
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
    "; longitude = 0.00\n";

void loadNvs() {
  if (loaded) return;
  loaded = true;
  Preferences prefs;
  if (!prefs.begin(kNamespace, false)) return;
  card.tz = prefs.getString("tz", "").c_str();
  card.location = prefs.getBool("loc", false);
  card.lat = prefs.getDouble("lat", 0);
  card.lon = prefs.getDouble("lon", 0);
  prefs.end();
}

void saveNvs() {
  Preferences prefs;
  if (!prefs.begin(kNamespace, false)) return;
  prefs.putString("tz", card.tz.c_str());
  prefs.putBool("loc", card.location);
  prefs.putDouble("lat", card.lat);
  prefs.putDouble("lon", card.lon);
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

bool parseDegrees(const std::string &text, double limit, double &out) {
  char *end = nullptr;
  const double v = strtod(text.c_str(), &end);
  if (text.empty() || *end != '\0' || !isfinite(v) || fabs(v) > limit) return false;
  out = v;
  return true;
}

Values parse(const std::string &text) {
  Values v;
  bool hasLat = false, hasLon = false;
  double lat = 0, lon = 0;
  size_t pos = 0;
  int lineNo = 0;
  while (pos < text.size()) {
    size_t nl = text.find('\n', pos);
    if (nl == std::string::npos) nl = text.size();
    std::string line = text.substr(pos, nl - pos);
    pos = nl + 1;
    ++lineNo;
    if (lineNo == 1 && line.compare(0, 3, "\xEF\xBB\xBF") == 0) line.erase(0, 3);  // Notepad's BOM
    if (!line.empty() && line.back() == '\r') line.pop_back();
    line = trim(line);
    if (line.empty() || line[0] == ';' || line[0] == '#' || line[0] == '[') continue;
    const size_t eq = line.find('=');
    if (eq == std::string::npos) {
      log_w("settings: line %d of %s isn't key = value", lineNo, kFile);
      continue;
    }
    std::string key = trim(line.substr(0, eq));
    for (char &c : key) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    std::string value = trim(line.substr(eq + 1));
    const size_t comment = value.find(" ;");  // a comment after the value
    if (comment != std::string::npos) value = trim(value.substr(0, comment));
    if (value.size() >= 2 && (value[0] == '"' || value[0] == '\'') && value.back() == value[0]) {
      value = value.substr(1, value.size() - 2);
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
    } else if (key.find("wifi") != std::string::npos || key == "ssid" || key == "password") {
      log_w("settings: %s: Wi-Fi isn't read from the card (it stays in .env)", kFile);
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
  const Values next = parse(text);
  if (!(next == card)) {
    card = next;
    saveNvs();
  }
  applyTimezone();
  log_i("settings: time zone %s, location %s", card.tz.empty() ? "built in" : "from the card",
        card.location ? "from the card" : hasLocation() ? "built in" : "not set");
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

std::string describe() {
  const bool tz = timezoneSource() == Source::Card;
  const bool loc = locationSource() == Source::Card;
  if (!tz && !loc) return ".env (built in)";
  return std::string("SD:/inkdeck.ini \xC2\xB7 ") + (tz ? "TZ" : "") + (tz && loc ? ", " : "") +
         (loc ? "LOCATION" : "");
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
