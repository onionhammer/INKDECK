#include "apps/idle.h"

#include <Arduino.h>
#include <driver/usb_serial_jtag.h>
#include <esp_system.h>
#include <sys/time.h>

#include <atomic>

#include "apps/clock.h"
#include "apps/sleep.h"
#include "audio.h"
#include "input.h"
#include "pmic.h"
#include "podcast_library.h"
#include "power.h"
#include "qmi8658.h"
#include "timesync.h"
#include "forecast.h"
#include "ui/nav.h"
#include "wlan.h"

namespace apps {
namespace {

// No waiting after a page turn: the redraw finishes inside loop() before the nap check runs,
// a button that's down keeps the board awake, and a released press waits in the key queue.
// The guard only covers the first debounce samples after a button wakes the board.
constexpr uint32_t kKeyWakeGuardMs = 30;
constexpr uint32_t kUsbGraceMs = 3000;  // USB power without a host talking, after naps

std::atomic<int> standbyRequest{-1};

bool napping = false;
bool nappedSinceBoot = false;
uint32_t usbSinceMs = 0;  // when USB power appeared after naps; 0 = not present
uint32_t keyWakeMs = 0;
std::atomic<uint32_t> naps{0};
std::atomic<uint32_t> buttonWakes{0};
std::atomic<uint32_t> asleepMs{0};

bool readingNow() {
  const ui::Screen *b = ui::nav::base();
  return b && b->reading();
}

bool mayNap() {
  if (!readingNow() || audio::active() || audio::ringing() || wlan::radioOn()) return false;
  if (weather::syncing() || podcasts::busy()) return false;
  const timesync::Phase t = timesync::status().phase;
  if (t == timesync::Phase::Connecting || t == timesync::Phase::Asking) return false;
  if (millis() - keyWakeMs < kKeyWakeGuardMs) return false;
  if (input::anyDown() || input::pending()) return false;
  // On USB the power doesn't matter, and light sleep would drop the console. Plugging USB into
  // a napping board ends the naps at the next wake (a key, or the minute).
  const pmic::Status p = pmic::status();
  return p.ok && p.battery && !p.vbus;
}

void stopNapping() {
  if (!napping) return;
  napping = false;
  qmi8658::setEnabled(true);
  log_i("nap: awake (%lu naps, %lu woken by a button, %lu s asleep since boot)",
        static_cast<unsigned long>(naps.load()), static_cast<unsigned long>(buttonWakes.load()),
        static_cast<unsigned long>(asleepMs.load() / 1000));
}

// Light sleep leaves the USB Serial/JTAG controller unable to talk to a host until a reset
// (seen on hardware). So once the board has napped, USB power with no host traffic for a few
// seconds means a clean restart: the Reader saves its place first. Plugged into a charger
// rather than a computer, that's one harmless restart.
void restartIfUsbStuck() {
  if (!nappedSinceBoot) return;
  if (!pmic::status().vbus) {
    usbSinceMs = 0;
    return;
  }
  if (!usbSinceMs) usbSinceMs = millis() | 1;
  if (millis() - usbSinceMs < kUsbGraceMs || usb_serial_jtag_is_connected()) return;
  log_w("nap: USB power but no host after light sleep; restarting");
  if (ui::Screen *b = ui::nav::base()) b->onNap();  // save the place
  delay(50);
  esp_restart();
}

}  // namespace

void pollPower() {
  const int request = standbyRequest.exchange(-1);
  if (request >= 0) {
    stopNapping();
    enterStandby(request);
    return;
  }
  if (!power::idle(readingNow())) return;
  stopNapping();
  const pmic::Status p = pmic::status();
  const bool low = p.percent >= 0 && p.percent < power::kLowBatteryPercent;
  log_i("idle on battery (%d %%): %s", p.percent, low ? "sleep" : "standby");
  if (low) {
    sleepNow();
  } else {
    enterStandby(0);
  }
}

bool napIfIdle() {
  restartIfUsbStuck();
  if (!mayNap()) {
    stopNapping();
    return false;
  }
  if (!napping) {
    napping = true;
    nappedSinceBoot = true;
    qmi8658::setEnabled(false);
    if (ui::Screen *b = ui::nav::base()) b->onNap();
  }
  // Until the next minute boundary (an alarm rings on the minute), a little after it.
  timeval tv;
  gettimeofday(&tv, nullptr);
  const uint32_t ms = 60000 - static_cast<uint32_t>(tv.tv_sec % 60) * 1000 -
                      static_cast<uint32_t>(tv.tv_usec / 1000) + 20;
  const uint32_t before = millis();
  const bool byKey = power::nap(ms);
  asleepMs += millis() - before;
  ++naps;
  if (byKey) {
    keyWakeMs = millis();
    ++buttonWakes;
  }
  return true;
}

NapStats napStats() {
  NapStats s;
  s.naps = naps.load();
  s.buttonWakes = buttonWakes.load();
  s.asleepSeconds = asleepMs.load() / 1000;
  return s;
}

void requestStandby(int cycles) { standbyRequest = cycles; }

}  // namespace apps
