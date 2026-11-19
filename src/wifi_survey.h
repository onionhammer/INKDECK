// Wi-Fi survey for the Sensors screen (apps/sensors.h, apps/wifi_networks.h): the 2.4 GHz
// networks in range, from active scans of every channel, and one of them followed live.
//
// A background task scans while someone uses it (begin()/end() pairs): a full scan at once, then
// every kEveryMs. With track() set it scans only that network's channel, every kTrackMs, and its
// entry follows. The task holds the radio on through wlan::holdRadio() (the status bar shows
// WIFI) and lets it go kLingerMs after the last end(), so moving between screens doesn't cycle
// it; then the task ends, so it holds no memory between uses. Scanning works alongside a
// connection another task has open; the scan then leaves its channel for a moment.
#pragma once
#include <esp_wifi_types.h>
#include <stdint.h>

#include <string>
#include <vector>

namespace wifisurvey {

struct Network {
  uint8_t bssid[6] = {};
  std::string ssid;  // "" for a hidden network
  uint8_t channel = 0;  // primary
  wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;  // 40 MHz: the other 20 MHz above or below
  int8_t rssi = -127;
  wifi_auth_mode_t auth = WIFI_AUTH_OPEN;
  wifi_cipher_type_t pairwise = WIFI_CIPHER_TYPE_NONE;
  wifi_cipher_type_t group = WIFI_CIPHER_TYPE_NONE;
  bool b = false, g = false, n = false, ax = false, lr = false;  // PHY modes it advertises
  bool wps = false, ftmResponder = false, ftmInitiator = false;
  char country[3] = {};  // "US"; "" if the AP sends none
  uint8_t countryFirst = 0, countryCount = 0;  // its channels
  int8_t countryMaxTx = 0;  // dBm
  int8_t bssColor = -1;      // Wi-Fi 6 only; -1 without (or with BSS colouring off)
  bool saved = false;        // its name is one of the networks settings.h lists
  uint32_t heardMs = 0;      // millis() when last heard, by a scan or the live track
};

constexpr uint32_t kEveryMs = 15000;
constexpr uint32_t kTrackMs = 2000;
constexpr uint32_t kLingerMs = 4000;
constexpr uint32_t kForgetMs = 45000;  // a network the scans miss this long leaves the list

void begin();  // a user; the first starts the scanning
void end();

// The networks the full scans have heard (missed for kForgetMs: dropped), strongest first.
// Empty before the first scan; kept after the scanning stops, so a screen can show what was seen
// a moment ago.
std::vector<Network> networks();
uint32_t lastScanMs();  // millis() of the last full scan, 0 if none yet
uint32_t generation();  // changes with each full scan and each tracked sample
bool scanning();        // a user is in and the radio is held
std::string error();    // why the last scan failed ("RADIO BUSY"); "" after one that worked

// Follow one network: its channel only, every kTrackMs, in place of the full scans.
// untrack() goes back to them (the next comes at once).
void track(const uint8_t bssid[6], uint8_t channel);
void untrack();
// How many times the tracked network has been heard since track() (a scan that misses it adds
// nothing); its signal and time are in its networks() entry.
uint32_t trackSamples();

// The channels this board may scan (the driver's country setting), as seen by the last scan.
void channelRange(int &first, int &last);

// Text for the screens.
std::string bssidText(const uint8_t bssid[6]);           // "A4:2B:B0:1C:9E:42"
const char *securityShort(wifi_auth_mode_t auth);        // "WPA2/3", "OPEN": 6 characters at most
const char *securityLong(wifi_auth_mode_t auth);         // "WPA2/WPA3 PERSONAL"
const char *cipherName(wifi_cipher_type_t cipher);       // "CCMP"
std::string phyModes(const Network &n);                  // "802.11 B/G/N/AX"
const char *generationName(const Network &n);            // "WI-FI 6", "WI-FI 4", "11G", "11B"
int frequencyMhz(int channel);                           // 2437 for channel 6
// The 5 MHz channel steps a network covers: 20 MHz spans its channel ±2, 40 MHz the pair.
void span(const Network &n, int &lo, int &hi);
bool sameBssid(const uint8_t a[6], const uint8_t b[6]);

}  // namespace wifisurvey
