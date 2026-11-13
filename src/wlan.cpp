#include "wlan.h"

#include <Arduino.h>
#include <WiFi.h>

#include <atomic>

#include "inkdeck_secrets.h"  // generated from .env by scripts/gen_secrets.py

namespace wlan {
namespace {

enum class State : uint8_t { Off, Connecting, On };
std::atomic<State> state{State::Off};
std::atomic<int> rssi{0};

}  // namespace

bool configured() { return WIFI_CONFIGURED; }

const char *network() { return WIFI_SSID; }

std::string describe() {
  const std::string name = network();
  switch (state.load()) {
    case State::Connecting: return "connecting \xC2\xB7 " + name;
    case State::On:
      return "on \xC2\xB7 " + name + " \xC2\xB7 " + std::to_string(rssi.load()) + " dBm";
    case State::Off: break;
  }
  return "off \xC2\xB7 " + name;
}

bool connect(uint32_t timeoutMs, std::string &error) {
  if (!configured()) {
    error = "NO NETWORK IN .ENV";
    return false;
  }
  state = State::Connecting;
  WiFi.persistent(false);  // the network comes from .env; don't copy it into NVS
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  const uint32_t start = millis();
  wl_status_t s = WiFi.status();
  while (s != WL_CONNECTED && millis() - start < timeoutMs) {
    delay(100);
    s = WiFi.status();
  }
  if (s != WL_CONNECTED) {
    error = s == WL_NO_SSID_AVAIL    ? "NETWORK NOT FOUND"
            : s == WL_CONNECT_FAILED ? "REFUSED (PASSWORD?)"
                                     : "NO CONNECTION";
    log_w("wlan: %s didn't connect (status %d) in %lu ms", WIFI_SSID, static_cast<int>(s),
          static_cast<unsigned long>(timeoutMs));
    disconnect();
    return false;
  }
  rssi = WiFi.RSSI();
  state = State::On;
  log_i("wlan: connected to %s in %lu ms, ip %s, %d dBm", WIFI_SSID,
        static_cast<unsigned long>(millis() - start), WiFi.localIP().toString().c_str(),
        rssi.load());
  return true;
}

bool radioOn() { return state.load() != State::Off; }

void disconnect() {
  WiFi.disconnect(true);  // and Wi-Fi off
  WiFi.mode(WIFI_OFF);
  state = State::Off;
}

}  // namespace wlan
