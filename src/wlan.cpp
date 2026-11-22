#include "wlan.h"

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <vector>

#include "settings.h"  // the networks: SD:/inkdeck.ini's [wifi], else .env's

namespace wlan {
namespace {

enum class State : uint8_t { Off, Connecting, On };
std::atomic<State> state{State::Off};
std::atomic<int> rssi{0};

std::mutex usersMutex;  // held through a connect, so a second caller waits for it
int users = 0;          // successful connects not yet matched by a disconnect
int holders = 0;        // holdRadio() calls not yet matched by a releaseRadio()
std::atomic<bool> held{false};  // holders > 0, for radioOn() without the mutex

// With several networks, how long one gets before the next one's turn, unless the driver says
// sooner that it isn't in range or it fails twice. Joining a network in range takes 1 to 5 s.
constexpr uint32_t kAttemptMs = 10000;

constexpr const char *kNamespace = "wlan";  // NVS: "last", the ssid last joined

std::mutex infoMutex;  // guards the rest, which the UI task and the Wi-Fi event task read
std::string current;   // the network being joined, or joined; "" while the radio is off
std::string last;      // the network last joined, "" if none yet
bool lastLoaded = false;
// What the driver said about current since its try began (onDisconnected()).
int notFound = 0;      // its scans didn't find it
int failures = 0;      // other failed tries: refused, timed out
bool refused = false;  // one of them looks like a wrong password

// Under infoMutex.
const std::string &lastJoined() {
  if (!lastLoaded) {
    lastLoaded = true;
    Preferences prefs;
    if (prefs.begin(kNamespace, true)) {  // read-only: fails until the first join creates it
      if (prefs.isKey("last")) last = prefs.getString("last", "").c_str();
      prefs.end();
    }
  }
  return last;
}

void rememberJoined(const std::string &ssid) {
  {
    std::lock_guard<std::mutex> lock(infoMutex);
    if (lastJoined() == ssid) return;  // NVS is written only when it changes
    last = ssid;
  }
  Preferences prefs;
  if (prefs.begin(kNamespace, false)) {
    prefs.putString("last", ssid.c_str());
    prefs.end();
  }
}

// The driver gave up on a try (the Arduino core then tries again by itself).
void onDisconnected(arduino_event_id_t, arduino_event_info_t info) {
  const wifi_event_sta_disconnected_t &d = info.wifi_sta_disconnected;
  std::lock_guard<std::mutex> lock(infoMutex);
  // Another network's, from a try before this one. Counted if the driver leaves the ssid out.
  if (d.ssid_len > 0 &&
      (d.ssid_len != current.size() || memcmp(d.ssid, current.data(), d.ssid_len) != 0)) {
    return;
  }
  switch (d.reason) {
    case WIFI_REASON_ASSOC_LEAVE:  // we stopped it
    case WIFI_REASON_STA_LEAVING:  // so did the core, to try again after a miss
      return;
    case WIFI_REASON_NO_AP_FOUND:
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD: ++notFound; return;
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT: refused = true; break;
    default: break;
  }
  ++failures;
}

enum class Outcome : uint8_t { Joined, NotFound, Refused, Failed };

const char *outcomeName(Outcome o) {
  switch (o) {
    case Outcome::Joined: return "joined";
    case Outcome::NotFound: return "not found";
    case Outcome::Refused: return "refused";
    case Outcome::Failed: return "failed";
  }
  return "";
}

// Tries one network until it has an IP or the deadline passes. With giveUp, also stops once
// the driver says it isn't in range or it has failed twice, so the next network gets its turn.
Outcome attempt(const settings::WifiNetwork &net, uint32_t deadline, bool giveUp) {
  {
    std::lock_guard<std::mutex> lock(infoMutex);
    current = net.ssid;
    notFound = failures = 0;
    refused = false;
  }
  WiFi.begin(net.ssid.c_str(), net.password.empty() ? nullptr : net.password.c_str());
  for (;;) {
    if (WiFi.status() == WL_CONNECTED) return Outcome::Joined;
    const bool late = static_cast<int32_t>(millis() - deadline) >= 0;
    {
      std::lock_guard<std::mutex> lock(infoMutex);
      if (late || (giveUp && (notFound > 0 || failures >= 2))) {
        if (refused) return Outcome::Refused;
        return notFound > 0 && failures == 0 ? Outcome::NotFound : Outcome::Failed;
      }
    }
    delay(100);
  }
}

void radioOff() {
  WiFi.disconnect(true);  // and Wi-Fi off
  WiFi.mode(WIFI_OFF);
  state = State::Off;
  std::lock_guard<std::mutex> lock(infoMutex);
  current.clear();
}

// Under usersMutex: the last connected user is done. The radio goes off, unless a scanner holds
// it, which keeps station mode without the connection.
void dropConnection() {
  if (holders == 0) {
    radioOff();
    return;
  }
  WiFi.disconnect(false);
  state = State::Off;
  std::lock_guard<std::mutex> lock(infoMutex);
  current.clear();
}

}  // namespace

bool configured() { return settings::hasWifi(); }

std::string network() {
  {
    std::lock_guard<std::mutex> lock(infoMutex);
    if (state.load() != State::Off && !current.empty()) return current;
  }
  // The one connect() tries first.
  const std::vector<std::string> ssids = settings::wifiSsids();
  if (ssids.empty()) return {};
  std::lock_guard<std::mutex> lock(infoMutex);
  const std::string &prev = lastJoined();
  return std::find(ssids.begin(), ssids.end(), prev) != ssids.end() ? prev : ssids.front();
}

std::string describe() {
  const std::string name = network();
  switch (state.load()) {
    case State::Connecting: return "connecting \xC2\xB7 " + name;
    case State::On:
      return "on \xC2\xB7 " + name + " \xC2\xB7 " + std::to_string(rssi.load()) + " dBm";
    case State::Off: break;
  }
  const size_t count = settings::wifiSsids().size();
  return "off \xC2\xB7 " + name + (count > 1 ? " +" + std::to_string(count - 1) : "");
}

bool connect(uint32_t timeoutMs, std::string &error) {
  std::vector<settings::WifiNetwork> networks = settings::wifiNetworks();
  if (networks.empty()) {
    error = "NO NETWORK SET";
    return false;
  }
  std::lock_guard<std::mutex> lock(usersMutex);
  if (users > 0 && state.load() == State::On && WiFi.status() == WL_CONNECTED) {
    ++users;
    return true;
  }
  static bool listening = false;
  if (!listening) {
    WiFi.onEvent(onDisconnected, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
    listening = true;
  }
  {
    // The network last joined first, then the others in the card's order.
    std::lock_guard<std::mutex> info(infoMutex);
    const std::string &prev = lastJoined();
    const auto it = std::find_if(networks.begin(), networks.end(),
                                 [&](const settings::WifiNetwork &n) { return n.ssid == prev; });
    if (it != networks.end()) std::rotate(networks.begin(), it, it + 1);
  }
  state = State::Connecting;
  WiFi.persistent(false);  // settings.h keeps the networks; don't copy one into the driver's NVS
  WiFi.mode(WIFI_STA);
  const uint32_t start = millis();
  const uint32_t deadline = start + timeoutMs;
  const bool several = networks.size() > 1;
  const settings::WifiNetwork *joined = nullptr;
  bool anyRefused = false, allNotFound = true;
  // Round the list until one joins or the time is up: a network the driver missed once may be
  // found the next time round.
  for (size_t i = 0, tries = 0; static_cast<int32_t>(millis() - deadline) < 0;
       i = (i + 1) % networks.size(), ++tries) {
    if (tries > 0) WiFi.disconnect();  // stop the last try
    const uint32_t now = millis();
    const uint32_t left = deadline - now;
    const Outcome o =
        attempt(networks[i], now + (several ? std::min(left, kAttemptMs) : left), several);
    if (o == Outcome::Joined) {
      joined = &networks[i];
      break;
    }
    log_i("wlan: %s %s after %lu ms", networks[i].ssid.c_str(), outcomeName(o),
          static_cast<unsigned long>(millis() - now));
    anyRefused |= o == Outcome::Refused;
    allNotFound &= o == Outcome::NotFound;
  }
  if (!joined) {
    error = anyRefused    ? "REFUSED (PASSWORD?)"
            : allNotFound ? "NETWORK NOT FOUND"
                          : "NO CONNECTION";
    log_w("wlan: no network joined in %lu ms (%s)", static_cast<unsigned long>(timeoutMs),
          error.c_str());
    // Another user's connection dropped too, if there was one; it finds out on its own.
    dropConnection();
    users = 0;
    return false;
  }
  rssi = WiFi.RSSI();
  state = State::On;
  ++users;
  rememberJoined(joined->ssid);
  log_i("wlan: connected to %s in %lu ms, ip %s, %d dBm", joined->ssid.c_str(),
        static_cast<unsigned long>(millis() - start), WiFi.localIP().toString().c_str(),
        rssi.load());
  return true;
}

bool radioOn() { return state.load() != State::Off || held.load(); }

bool joining() { return state.load() == State::Connecting; }

void disconnect() {
  std::lock_guard<std::mutex> lock(usersMutex);
  if (users > 0) --users;
  if (users == 0) dropConnection();
}

void forceOff() {
  if (radioOn()) radioOff();
}

void holdRadio() {
  std::lock_guard<std::mutex> lock(usersMutex);
  if (holders++ == 0) held = true;
  if (WiFi.getMode() == WIFI_OFF) {
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
  }
}

void releaseRadio() {
  std::lock_guard<std::mutex> lock(usersMutex);
  if (holders == 0) return;
  if (--holders > 0) return;
  held = false;
  if (users == 0 && state.load() == State::Off) radioOff();
}

}  // namespace wlan
