#include "wlan.h"

#include <Arduino.h>
#include <WiFi.h>

#include <atomic>
#include <mutex>

#include "settings.h"  // the network: SD:/inkdeck.ini's [wifi], else .env's

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

bool configured() { return settings::hasWifi(); }

std::string network() { return settings::wifiSsid(); }

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
    error = "NO NETWORK SET";
    return false;
  }
  std::lock_guard<std::mutex> lock(usersMutex);
  if (users > 0 && state.load() == State::On && WiFi.status() == WL_CONNECTED) {
    ++users;
    return true;
  }
  state = State::Connecting;
  WiFi.persistent(false);  // settings.h keeps the network; don't copy it into the driver's NVS
  WiFi.mode(WIFI_STA);
  const std::string ssid = network();
  const std::string password = settings::wifiPassword();
  WiFi.begin(ssid.c_str(), password.empty() ? nullptr : password.c_str());
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
    log_w("wlan: %s didn't connect (status %d) in %lu ms", ssid.c_str(), static_cast<int>(s),
          static_cast<unsigned long>(timeoutMs));
    // Another user's connection dropped too, if there was one; it finds out on its own.
    radioOff();
    users = 0;
    return false;
  }
  rssi = WiFi.RSSI();
  state = State::On;
  ++users;
  log_i("wlan: connected to %s in %lu ms, ip %s, %d dBm", ssid.c_str(),
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
