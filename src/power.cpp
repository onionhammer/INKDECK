#include "power.h"

#include <Arduino.h>
#include <Preferences.h>
#include <driver/gpio.h>
#include <esp_sleep.h>
#include <esp_system.h>

#include "audio.h"
#include "board_pins.h"
#include "input.h"
#include "ota.h"
#include "pmic.h"
#include "podcast_library.h"
#include "rtc.h"
#include "sensor_hub.h"
#include "timesync.h"
#include "forecast.h"
#include "wlan.h"

namespace power {
namespace {

constexpr const char *kNamespace = "power";
constexpr uint32_t kIdleMs = 10 * 60 * 1000;
constexpr int kOnBitPwron = 1 << 0;  // PMIC 0x20: PWRON pulled low (the key or the RTC's INT)
constexpr int kOnBitVbus = 1 << 2;   // USB plugged in

Wake wakeReason = Wake::Reset;
Mode mode = Mode::On;
time_t armed = 0;
int cyclesLeft = 0;

}  // namespace

void begin() {
  const bool alarmFired = rtc::alarm().fired;
  rtc::disarm();  // release INT (and PWRON) before anything slow

  const esp_reset_reason_t reset = esp_reset_reason();
  const int bits = pmic::powerOnBits();
  if (reset != ESP_RST_POWERON) {
    wakeReason = Wake::Reset;
  } else if (alarmFired) {
    wakeReason = Wake::Alarm;
  } else if (bits >= 0 && (bits & kOnBitPwron)) {
    wakeReason = Wake::PowerKey;
  } else if (bits >= 0 && (bits & kOnBitVbus)) {
    wakeReason = Wake::Usb;
  } else {
    wakeReason = Wake::Other;
  }

  Preferences prefs;
  if (prefs.begin(kNamespace, false)) {
    mode = static_cast<Mode>(prefs.getUChar("mode", 0));
    armed = static_cast<time_t>(prefs.getULong64("armed", 0));
    cyclesLeft = prefs.getInt("cycles", 0);
    prefs.end();
  }
  if (mode > Mode::Standby) mode = Mode::On;
  log_i("power: woke by %s (reset %d, PMIC 0x20 = %02X), last off: %s, armed %ld", wakeName(wakeReason),
        static_cast<int>(reset), bits, mode == Mode::Sleep ? "sleep" : mode == Mode::Standby ? "standby" : "-",
        static_cast<long>(armed));
}

Wake wake() { return wakeReason; }

const char *wakeName(Wake w) {
  switch (w) {
    case Wake::Reset: return "RESET";
    case Wake::PowerKey: return "PWR KEY";
    case Wake::Alarm: return "RTC ALARM";
    case Wake::Usb: return "USB";
    case Wake::Other: break;
  }
  return "OTHER";
}

Mode lastMode() { return mode; }
time_t armedFor() { return armed; }
int standbyLeft() { return cyclesLeft; }

void markOn() {
  if (mode == Mode::On) return;
  Preferences prefs;
  if (prefs.begin(kNamespace, false)) {
    prefs.putUChar("mode", static_cast<uint8_t>(Mode::On));
    prefs.end();
  }
  mode = Mode::On;
}

void off(Mode m, time_t wakeAt, int standbyCycles) {
  if (audio::active() || audio::ringing() || audio::recording()) {
    const bool recording = audio::recording();
    audio::stop();  // also finishes a recording's file
    // The audio task mutes and switches the amp off first, so the speaker doesn't pop; a
    // recording needs its last write and header too.
    delay(recording ? 400 : 150);
  }
  if (sensors::logging()) sensors::setLogging(false);  // close the CSV before the power goes
  wlan::forceOff();

  if (wakeAt) {
    const time_t now = time(nullptr);
    if (wakeAt < now + 2) wakeAt += 60 * ((now + 2 - wakeAt) / 60 + 1);
    if (!rtc::arm(wakeAt)) wakeAt = 0;
  } else {
    rtc::disarm();
  }
  Preferences prefs;
  if (prefs.begin(kNamespace, false)) {
    prefs.putUChar("mode", static_cast<uint8_t>(m));
    prefs.putULong64("armed", static_cast<uint64_t>(wakeAt));
    prefs.putInt("cycles", standbyCycles);
    prefs.end();
  }
  mode = m;  // so markOn() below rewrites it if the power-off fails
  log_i("power: off (%s), wake at %ld", m == Mode::Standby ? "standby" : "sleep",
        static_cast<long>(wakeAt));
  if (pmic::powerOff()) delay(2000);  // power goes within milliseconds
  // Still here: the PMIC didn't take it. Undo, so the alarm can't hold PWRON low.
  log_e("power: the PMIC didn't power off");
  rtc::disarm();
  markOn();
}

bool idle(bool reading) {
  if (input::idleMs() < kIdleMs) return false;
  const pmic::Status p = pmic::status();
  if (!p.ok || !p.battery || p.vbus) return false;  // on USB (or unknown): stay on
  if (audio::ringing()) return false;
  const bool playing =
      audio::status().state == audio::State::Playing || audio::recording();  // a memo too
  const bool low = p.percent >= 0 && p.percent < kLowBatteryPercent;
  if ((reading || playing) && !low) return false;
  if (weather::syncing() || podcasts::busy() || ota::active()) return false;
  const timesync::Phase t = timesync::status().phase;
  return t != timesync::Phase::Connecting && t != timesync::Phase::Asking;
}

bool nap(uint32_t maxMs) {
  // The buttons are active low, PWR_KEY_SENSE active high; all have pull-ups on the board.
  // Keep their normal (awake) configuration through the sleep, rather than a sleep
  // configuration that may switch the input off.
  const int activeLow[] = {board::BTN_UP, board::BTN_DOWN, board::BTN_PRESS, board::BTN_BOOT};
  for (const int pin : activeLow) {
    gpio_sleep_sel_dis(static_cast<gpio_num_t>(pin));
    gpio_wakeup_enable(static_cast<gpio_num_t>(pin), GPIO_INTR_LOW_LEVEL);
  }
  gpio_sleep_sel_dis(static_cast<gpio_num_t>(board::PWR_KEY_SENSE));
  gpio_wakeup_enable(static_cast<gpio_num_t>(board::PWR_KEY_SENSE), GPIO_INTR_HIGH_LEVEL);
  esp_sleep_enable_gpio_wakeup();
  esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(maxMs) * 1000);
  esp_light_sleep_start();  // no Serial.flush(): it can block on a half-connected USB host
  const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  for (const int pin : activeLow) gpio_wakeup_disable(static_cast<gpio_num_t>(pin));
  gpio_wakeup_disable(static_cast<gpio_num_t>(board::PWR_KEY_SENSE));
  return cause == ESP_SLEEP_WAKEUP_GPIO;
}

}  // namespace power
