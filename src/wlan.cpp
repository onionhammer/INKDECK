#include "wlan.h"

#include <Arduino.h>
#include <WiFi.h>

#include <atomic>
#include <mutex>

#include "inkdeck_secrets.h"  // generated from .env by scripts/gen_secrets.py

namespace wlan {
namespace {

enum class State : uint8_t { Off, Connecting, On };
std::atomic<State> state{State::Off};
std::atomic<int> rssi{0};

std::mutex usersMutex;  // held through a connect, so a second caller waits for it
int users = 0;          // successful connects not yet matched by a disconnect

void radioOff() {
  WiFi.disconnect(true);  // and Wi-Fi off
  WiFi.mode(WIFI_OFF);
  state = State::Off;
}

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
  std::lock_guard<std::mutex> lock(usersMutex);
  if (users > 0 && state.load() == State::On && WiFi.status() == WL_CONNECTED) {
    ++users;
    return true;
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
    // Another user's connection dropped too, if there was one; it finds out on its own.
    radioOff();
    users = 0;
    return false;
  }
  rssi = WiFi.RSSI();
  state = State::On;
  ++users;
  log_i("wlan: connected to %s in %lu ms, ip %s, %d dBm", WIFI_SSID,
        static_cast<unsigned long>(millis() - start), WiFi.localIP().toString().c_str(),
        rssi.load());
  return true;
}

bool radioOn() { return state.load() != State::Off; }

void disconnect() {
  std::lock_guard<std::mutex> lock(usersMutex);
  if (users > 0) --users;
  if (users == 0) radioOff();
}

void forceOff() {
  if (radioOn()) radioOff();
}

}  // namespace wlan
