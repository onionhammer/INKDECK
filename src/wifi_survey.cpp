#include "wifi_survey.h"

#include <Arduino.h>
#include <esp_wifi.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <mutex>

#include "settings.h"
#include "wlan.h"

namespace wifisurvey {
namespace {

constexpr uint16_t kMaxRecords = 48;
constexpr uint32_t kStaleMs = 5 * 60 * 1000;  // a list older than this is dropped at the next start

std::mutex mutex;  // guards everything below
int users = 0;
uint32_t lastUserGoneMs = 0;
std::vector<Network> list;
uint32_t scannedAt = 0;
uint32_t gen = 0;
bool holding = false;  // the task holds the radio
std::string lastError;
bool tracking = false;
bool trackChanged = false;
bool fullDue = false;
uint8_t trackBssid[6] = {};
uint8_t trackChannel = 0;
int firstChannel = 1, lastChannel = 13;
TaskHandle_t task = nullptr;

void wake() {
  if (task) xTaskNotifyGive(task);
}

Network fromRecord(const wifi_ap_record_t &r, const std::vector<std::string> &saved) {
  Network n;
  memcpy(n.bssid, r.bssid, 6);
  n.ssid.assign(reinterpret_cast<const char *>(r.ssid),
                strnlen(reinterpret_cast<const char *>(r.ssid), sizeof r.ssid));
  n.channel = r.primary;
  n.second = r.second;
  n.rssi = r.rssi;
  n.auth = r.authmode;
  n.pairwise = r.pairwise_cipher;
  n.group = r.group_cipher;
  n.b = r.phy_11b;
  n.g = r.phy_11g;
  n.n = r.phy_11n;
  n.ax = r.phy_11ax;
  n.lr = r.phy_lr;
  n.wps = r.wps;
  n.ftmResponder = r.ftm_responder;
  n.ftmInitiator = r.ftm_initiator;
  if (r.country.cc[0] >= 'A' && r.country.cc[0] <= 'Z') {
    n.country[0] = r.country.cc[0];
    n.country[1] = r.country.cc[1];
    n.countryFirst = r.country.schan;
    n.countryCount = r.country.nchan;
    n.countryMaxTx = r.country.max_tx_power;
  }
  if (n.ax && !r.he_ap.bss_color_disabled) n.bssColor = r.he_ap.bss_color;
  n.saved = !n.ssid.empty() && std::find(saved.begin(), saved.end(), n.ssid) != saved.end();
  n.heardMs = millis();
  return n;
}

const char *errorText(esp_err_t e) {
  switch (e) {
    case ESP_ERR_WIFI_NOT_INIT:
    case ESP_ERR_WIFI_NOT_STARTED: return "RADIO OFF";
    case ESP_ERR_WIFI_STATE: return "RADIO BUSY";
    case ESP_ERR_WIFI_TIMEOUT: return "SCAN TIMED OUT";
    default: return "SCAN FAILED";
  }
}

// One scan, blocking. Returns false with error set if the driver refused it.
bool scan(const wifi_scan_config_t &cfg, std::vector<wifi_ap_record_t> &out, std::string &error) {
  const esp_err_t e = esp_wifi_scan_start(&cfg, true);
  if (e != ESP_OK) {
    error = errorText(e);
    log_w("wifisurvey: scan refused: %s", esp_err_to_name(e));
    return false;
  }
  uint16_t count = 0;
  esp_wifi_scan_get_ap_num(&count);
  count = std::min(count, kMaxRecords);
  out.resize(count);
  if (count > 0 && esp_wifi_scan_get_ap_records(&count, out.data()) != ESP_OK) count = 0;
  out.resize(count);
  esp_wifi_clear_ap_list();  // whatever didn't fit
  return true;
}

void fullScan() {
  wifi_scan_config_t cfg = {};
  cfg.show_hidden = true;
  cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
  cfg.scan_time.active.min = 50;
  cfg.scan_time.active.max = 120;
  std::vector<wifi_ap_record_t> records;
  std::string error;
  const bool ok = scan(cfg, records, error);
  wifi_country_t country = {};
  const bool haveCountry = esp_wifi_get_country(&country) == ESP_OK && country.nchan > 0;
  const std::vector<std::string> saved = settings::wifiSsids();
  std::vector<Network> found;
  found.reserve(records.size());
  for (const wifi_ap_record_t &r : records) found.push_back(fromRecord(r, saved));
  std::stable_sort(found.begin(), found.end(),
                   [](const Network &a, const Network &b) { return a.rssi > b.rssi; });
  std::lock_guard<std::mutex> lock(mutex);
  lastError = error;
  if (haveCountry) {
    firstChannel = country.schan;
    lastChannel = country.schan + country.nchan - 1;
  }
  if (!ok) return;
  // A scan misses some networks some of the time: keep those heard in the last kForgetMs.
  const uint32_t now = millis();
  for (const Network &old : list) {
    const bool again = std::any_of(found.begin(), found.end(),
                                   [&](const Network &n) { return sameBssid(n.bssid, old.bssid); });
    if (!again && now - old.heardMs <= kForgetMs) found.push_back(old);
  }
  std::stable_sort(found.begin(), found.end(),
                   [](const Network &a, const Network &b) { return a.rssi > b.rssi; });
  list = std::move(found);
  scannedAt = millis();
  if (scannedAt == 0) scannedAt = 1;
  ++gen;
}

void trackScan(const uint8_t bssid[6], uint8_t channel) {
  uint8_t target[6];
  memcpy(target, bssid, 6);
  wifi_scan_config_t cfg = {};
  cfg.bssid = target;
  cfg.channel = channel;
  cfg.show_hidden = true;
  cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
  cfg.scan_time.active.min = 50;
  cfg.scan_time.active.max = 120;
  std::vector<wifi_ap_record_t> records;
  std::string error;
  const bool ok = scan(cfg, records, error);
  std::lock_guard<std::mutex> lock(mutex);
  lastError = error;
  if (!ok || !tracking || !sameBssid(trackBssid, target)) return;
  for (const wifi_ap_record_t &r : records) {
    if (!sameBssid(r.bssid, target)) continue;
    ++gen;
    for (Network &n : list) {
      if (!sameBssid(n.bssid, target)) continue;
      n.rssi = r.rssi;
      n.heardMs = millis();
      n.channel = r.primary;  // an AP can move
    }
    break;
  }
}

void run(void *) {
  uint32_t lastFull = 0, lastTrack = 0;
  for (;;) {
    bool hold, trk, changed, due;
    uint8_t bssid[6];
    uint8_t channel;
    uint32_t gone;
    {
      std::lock_guard<std::mutex> lock(mutex);
      hold = users > 0;
      gone = lastUserGoneMs;
      trk = tracking;
      changed = trackChanged;
      trackChanged = false;
      due = fullDue;
      fullDue = false;
      memcpy(bssid, trackBssid, 6);
      channel = trackChannel;
    }
    if (!hold) {
      if (holding && millis() - gone >= kLingerMs) {
        wlan::releaseRadio();
        std::lock_guard<std::mutex> lock(mutex);
        holding = false;
      }
      if (!holding) {
        // Nobody left: the task ends, so its stack doesn't sit in the internal heap between
        // uses. begin() starts another.
        std::lock_guard<std::mutex> lock(mutex);
        if (users == 0) {
          task = nullptr;
          break;
        }
        continue;
      }
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500));
      continue;
    }
    if (!holding) {
      wlan::holdRadio();
      std::lock_guard<std::mutex> lock(mutex);
      holding = true;
      if (scannedAt != 0 && millis() - scannedAt > kStaleMs) {
        list.clear();
        scannedAt = 0;
        ++gen;
      }
      due = due || scannedAt == 0 || millis() - scannedAt >= kEveryMs;
    }
    const uint32_t now = millis();
    if (trk) {
      if (changed || now - lastTrack >= kTrackMs) {
        lastTrack = now;
        trackScan(bssid, channel);
      }
    } else if (due || lastFull == 0 || now - lastFull >= kEveryMs) {
      fullScan();
      lastFull = millis();
      std::lock_guard<std::mutex> lock(mutex);
      // Refused (a connect under way): try again soon rather than in a full period.
      if (!lastError.empty()) lastFull -= kEveryMs - 2000;
    }
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200));
  }
  vTaskDelete(nullptr);
}

}  // namespace

void begin() {
  std::lock_guard<std::mutex> lock(mutex);
  ++users;
  if (!task) xTaskCreate(run, "wifisurvey", 4096, nullptr, 1, &task);
  wake();
}

void end() {
  std::lock_guard<std::mutex> lock(mutex);
  if (users == 0) return;
  if (--users == 0) lastUserGoneMs = millis();
  wake();
}

std::vector<Network> networks() {
  std::lock_guard<std::mutex> lock(mutex);
  return list;
}

uint32_t lastScanMs() {
  std::lock_guard<std::mutex> lock(mutex);
  return scannedAt;
}

uint32_t generation() {
  std::lock_guard<std::mutex> lock(mutex);
  return gen;
}

std::string error() {
  std::lock_guard<std::mutex> lock(mutex);
  return lastError;
}

void track(const uint8_t bssid[6], uint8_t channel) {
  std::lock_guard<std::mutex> lock(mutex);
  if (tracking && sameBssid(trackBssid, bssid) && trackChannel == channel) return;
  tracking = true;
  trackChanged = true;
  memcpy(trackBssid, bssid, 6);
  trackChannel = channel;
  wake();
}

void untrack() {
  std::lock_guard<std::mutex> lock(mutex);
  if (!tracking) return;
  tracking = false;
  fullDue = true;
  wake();
}

void channelRange(int &first, int &last) {
  std::lock_guard<std::mutex> lock(mutex);
  first = firstChannel;
  last = lastChannel;
}

std::string bssidText(const uint8_t b[6]) {
  char buf[18];
  snprintf(buf, sizeof buf, "%02X:%02X:%02X:%02X:%02X:%02X", b[0], b[1], b[2], b[3], b[4], b[5]);
  return buf;
}

const char *securityShort(wifi_auth_mode_t auth) {
  switch (auth) {
    case WIFI_AUTH_OPEN: return "OPEN";
    case WIFI_AUTH_WEP: return "WEP";
    case WIFI_AUTH_WPA_PSK: return "WPA";
    case WIFI_AUTH_WPA2_PSK: return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA1/2";
    case WIFI_AUTH_ENTERPRISE: return "WPA2-E";
    case WIFI_AUTH_WPA3_PSK:
    case WIFI_AUTH_WPA3_EXT_PSK:
    case WIFI_AUTH_WPA3_EXT_PSK_MIXED_MODE: return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/3";
    case WIFI_AUTH_WAPI_PSK: return "WAPI";
    case WIFI_AUTH_OWE: return "OWE";
    case WIFI_AUTH_WPA3_ENT_192:
    case WIFI_AUTH_WPA3_ENTERPRISE: return "WPA3-E";
    case WIFI_AUTH_WPA2_WPA3_ENTERPRISE: return "W2/3-E";
    case WIFI_AUTH_DPP: return "DPP";
    case WIFI_AUTH_WPA_ENTERPRISE: return "WPA-E";
    default: return "?";
  }
}

const char *securityLong(wifi_auth_mode_t auth) {
  switch (auth) {
    case WIFI_AUTH_OPEN: return "OPEN (NONE)";
    case WIFI_AUTH_WEP: return "WEP";
    case WIFI_AUTH_WPA_PSK: return "WPA PERSONAL";
    case WIFI_AUTH_WPA2_PSK: return "WPA2 PERSONAL";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2 PERSONAL";
    case WIFI_AUTH_ENTERPRISE: return "WPA2 ENTERPRISE";
    case WIFI_AUTH_WPA3_PSK:
    case WIFI_AUTH_WPA3_EXT_PSK:
    case WIFI_AUTH_WPA3_EXT_PSK_MIXED_MODE: return "WPA3 PERSONAL";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3 PERSONAL";
    case WIFI_AUTH_WAPI_PSK: return "WAPI PERSONAL";
    case WIFI_AUTH_OWE: return "ENHANCED OPEN (OWE)";
    case WIFI_AUTH_WPA3_ENT_192: return "WPA3 ENTERPRISE 192-BIT";
    case WIFI_AUTH_WPA3_ENTERPRISE: return "WPA3 ENTERPRISE";
    case WIFI_AUTH_WPA2_WPA3_ENTERPRISE: return "WPA2/WPA3 ENTERPRISE";
    case WIFI_AUTH_DPP: return "EASY CONNECT (DPP)";
    case WIFI_AUTH_WPA_ENTERPRISE: return "WPA ENTERPRISE";
    default: return "UNKNOWN";
  }
}

const char *cipherName(wifi_cipher_type_t cipher) {
  switch (cipher) {
    case WIFI_CIPHER_TYPE_NONE: return "NONE";
    case WIFI_CIPHER_TYPE_WEP40: return "WEP-40";
    case WIFI_CIPHER_TYPE_WEP104: return "WEP-104";
    case WIFI_CIPHER_TYPE_TKIP: return "TKIP";
    case WIFI_CIPHER_TYPE_CCMP: return "CCMP";
    case WIFI_CIPHER_TYPE_TKIP_CCMP: return "TKIP+CCMP";
    case WIFI_CIPHER_TYPE_AES_CMAC128: return "CMAC";
    case WIFI_CIPHER_TYPE_SMS4: return "SMS4";
    case WIFI_CIPHER_TYPE_GCMP: return "GCMP";
    case WIFI_CIPHER_TYPE_GCMP256: return "GCMP-256";
    case WIFI_CIPHER_TYPE_AES_GMAC128: return "GMAC";
    case WIFI_CIPHER_TYPE_AES_GMAC256: return "GMAC-256";
    default: return "?";
  }
}

std::string phyModes(const Network &n) {
  std::string s;
  auto add = [&](bool on, const char *mode) {
    if (!on) return;
    if (!s.empty()) s += '/';
    s += mode;
  };
  add(n.b, "B");
  add(n.g, "G");
  add(n.n, "N");
  add(n.ax, "AX");
  if (s.empty()) s = "?";
  s = "802.11 " + s;
  if (n.lr) s += " + LR";
  return s;
}

const char *generationName(const Network &n) {
  if (n.ax) return "WI-FI 6";
  if (n.n) return "WI-FI 4";
  if (n.g) return "11G";
  if (n.b) return "11B";
  return "";
}

int frequencyMhz(int channel) { return channel == 14 ? 2484 : 2407 + 5 * channel; }

void span(const Network &n, int &lo, int &hi) {
  lo = n.channel - 2;
  hi = n.channel + 2;
  if (n.second == WIFI_SECOND_CHAN_ABOVE) hi = n.channel + 6;
  if (n.second == WIFI_SECOND_CHAN_BELOW) lo = n.channel - 6;
}

bool sameBssid(const uint8_t a[6], const uint8_t b[6]) { return memcmp(a, b, 6) == 0; }

}  // namespace wifisurvey
