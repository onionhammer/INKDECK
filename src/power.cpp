#include "power.h"

#include <Arduino.h>
#include <Preferences.h>
#include <driver/gpio.h>
#include <esp_attr.h>
#include <algorithm>
#include <esp_sleep.h>
#include <esp_system.h>
#include <soc/system_struct.h>

#include "audio.h"
#include "board_pins.h"
#include "comms.h"
#include "file_copy.h"
#include "input.h"
#include "news_library.h"
#include "page_fetch.h"
#include "ota.h"
#include "pmic.h"
#include "podcast_library.h"
#include "qmi8658.h"
#include "rtc.h"
#include "sensor_hub.h"
#include "timesync.h"
#include "forecast.h"
#include "features.h"
#if INKDECK_AGENT
#include "agent_client.h"
#endif
#include "webdav.h"
#include "wlan.h"

namespace power {
namespace {

constexpr const char *kNamespace = "power";
constexpr uint32_t kIdleMs = 10 * 60 * 1000;
constexpr uint32_t kBookIdleMs = 15 * 60 * 1000;
constexpr int kOnBitPwron = 1 << 0;  // PMIC 0x20: PWRON pulled low (the key or the RTC's INT)
constexpr int kOnBitVbus = 1 << 2;   // USB plugged in

Wake wakeReason = Wake::Reset;
Mode mode = Mode::On;
time_t armed = 0;
int cyclesLeft = 0;

// Reachable on battery: why off() (or the sleep) restarted the board, kept across the restart.
RTC_NOINIT_ATTR uint32_t restartMark;
constexpr uint32_t kMarkMagic = 0x5AC30000;
enum : uint8_t { kMarkNone, kMarkReach, kMarkPwr, kMarkAlarm, kMarkUsb };
bool reachPending = false;  // the restart was to sleep reachable
bool booted = false;        // setup() is over
bool noReach = false;       // off() takes the PMIC's way (the sleep can't, or is over)
Mode wokenMode = Mode::On;  // the mode the board slept in before a Comms wake
Held held = Held::None;
uint32_t heldSinceMs = 0;
ReachTest test = ReachTest::Off;
time_t testEnd = 0;

const char *modeName(Mode m) {
  switch (m) {
    case Mode::Sleep: return "sleep";
    case Mode::Standby: return "standby";
    case Mode::Book: return "book";
    case Mode::Agent: return "agent";
    case Mode::Edition: return "edition";
    case Mode::On: break;
  }
  return "-";
}

// A sync, a download, a copy from a share, a Wi-Fi drive session or a agent exchange that a
// power-off would cut short; or comms with a mic on (this board's, or another's in a conversation
// this board is in) or LOST's alarm, which runs until it's switched off (on battery until the
// battery gives out: the boards that hear it then see LOST).
bool working() {
  if (weather::syncing() || podcasts::busy() || ota::active() || webdav::active()) return true;
  if (news::busy() || pagefetch::busy()) return true;
  if (filecopy::busy()) return true;
#if INKDECK_AGENT
  if (agent::busy()) return true;
#endif
#if INKDECK_COMMS
  if (comms::keepAwake()) return true;
#endif
  const timesync::Phase t = timesync::status().phase;
  return t == timesync::Phase::Connecting || t == timesync::Phase::Asking;
}

bool reachWanted() {
#if INKDECK_COMMS
  if (comms::reachInterval() <= 0) return false;
  const pmic::Status p = pmic::status();
  if (!p.ok || p.vbus || !p.battery || p.percent < 0) return false;
  return p.percent >= kLowBatteryPercent;
#else
  return false;
#endif
}

const char *markName(uint8_t m) {
  switch (m) {
    case kMarkReach: return "to sleep reachable";
    case kMarkPwr: return "as PWR would boot it";
    case kMarkAlarm: return "as the RTC alarm would boot it";
    case kMarkUsb: return "as USB power would boot it";
    default: return "";
  }
}

// The USB-Serial-JTAG after light sleep: each light sleep switches its pad off and on again, and
// after the bench's 20 of them on 2026-11-23 both boards were off the bus for good, through a
// replug and through esp_restart() (which resets the CPUs, not the controller). Its clock on and its
// registers at their reset values, so a restart finds it as a power-on would.
void usbControllerReset() {
  SYSTEM.perip_clk_en1.usb_device_clk_en = 1;
  SYSTEM.perip_rst_en1.usb_device_rst = 1;
  SYSTEM.perip_rst_en1.usb_device_rst = 0;
}

[[noreturn]] void restartFor(uint8_t m) {
#if INKDECK_COMMS
  comms::reachLogFlush();
#endif
  // The console after light sleep, for USB plugged into a sleeping board (nobody is on the bus
  // otherwise). Not while pretending: there was no light sleep, and it would drop the console.
  if (test != ReachTest::Pretend) usbControllerReset();
  restartMark = kMarkMagic | m;
  log_i("power: restarting %s", markName(m));
  esp_restart();
  for (;;) {
  }
}

// In setup(), with nothing else started: the reachable sleep, in place of the PMIC's power-off.
// Returns on a Comms wake (wakeReason is then Comms), or with noReach set for the PMIC's way;
// restarts the board for the rest.
void sleepReachable() {
#if INKDECK_COMMS
  // A restart leaves the chips as they were (only the ESP32 resets): what the board's normal
  // start would quiet, quieted for the night. The amp's enable floats after a reset; the IMU keeps
  // sampling if the sensor log had it on; the codec and panel rails are off already (audio and
  // every panel update switch them off).
  pinMode(board::AMP_EN, OUTPUT);
  digitalWrite(board::AMP_EN, LOW);
  if (qmi8658::begin()) qmi8658::setEnabled(false);
  comms::ReachArgs a;
  a.wakeAt = armed;
  a.intervalS = comms::reachInterval();
  a.pretend = test == ReachTest::Pretend;
  a.ignoreUsb = test != ReachTest::Off;
  a.testEnd = test == ReachTest::Sleep ? testEnd : 0;
  const comms::ReachEnd e = comms::reachSleep(a);
  switch (e) {
    case comms::ReachEnd::TestOver:
      setReachTest(ReachTest::Off);
      consoleRecover();
    case comms::ReachEnd::PowerKey:
      if (test == ReachTest::Sleep) consoleRecover();
      restartFor(kMarkPwr);
    case comms::ReachEnd::Due:
      if (test == ReachTest::Sleep) consoleRecover();  // no minute wakes in a real-sleep test
      restartFor(kMarkAlarm);
    case comms::ReachEnd::Usb: restartFor(kMarkUsb);
    case comms::ReachEnd::Woken:
      // The console after light sleep: the boot goes on without a restart, so the controller is
      // reset here, or USB plugged in later finds it off the bus for good (found 2026-11-23: a
      // board woken on battery, answered and plugged in had no port). Not while pretending: there
      // was no light sleep, and it would drop the console.
      if (test != ReachTest::Pretend) {
        usbControllerReset();
        Serial.end();
        Serial.begin(115200);
      }
      wokenMode = mode;
      wakeReason = Wake::Comms;
      log_i("power: woken for comms from %s", modeName(mode));
      return;
    case comms::ReachEnd::LowBattery:
    case comms::ReachEnd::Failed: break;
  }
  log_w("power: %s: powering off as without mesh_reachable",
        e == comms::ReachEnd::LowBattery ? "the battery is low" : "can't sleep reachable");
#endif
  noReach = true;
}

}  // namespace

void begin() {
  const bool alarmFired = rtc::alarm().fired;
  rtc::disarm();  // release INT (and PWRON) before anything slow

  const esp_reset_reason_t reset = esp_reset_reason();
  const int bits = pmic::powerOnBits();
  const uint32_t mark = restartMark;
  restartMark = 0;
  const uint8_t marked =
      reset == ESP_RST_SW && (mark & 0xFFFF0000u) == kMarkMagic ? static_cast<uint8_t>(mark) : 0;
  if (marked == kMarkReach) {
    wakeReason = Wake::Other;  // reachResume() sleeps; it returns only for a Comms wake
    reachPending = true;
  } else if (marked == kMarkPwr) {
    wakeReason = Wake::PowerKey;
  } else if (marked == kMarkAlarm) {
    wakeReason = Wake::Alarm;
  } else if (marked == kMarkUsb) {
    wakeReason = Wake::Usb;
  } else if (reset != ESP_RST_POWERON) {
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
    test = static_cast<ReachTest>(prefs.getUChar("rtest", 0));
    testEnd = static_cast<time_t>(prefs.getULong64("rtestend", 0));
    prefs.end();
  }
  if (test > ReachTest::Sleep) test = ReachTest::Off;
  if (test != ReachTest::Off) pmic::pretendBattery(true);
  if (mode > Mode::Edition) mode = Mode::On;
  log_i("power: woke by %s (reset %d, PMIC 0x20 = %02X), last off: %s, armed %ld", wakeName(wakeReason),
        static_cast<int>(reset), bits, modeName(mode), static_cast<long>(armed));
}

Wake wake() { return wakeReason; }

const char *wakeName(Wake w) {
  switch (w) {
    case Wake::Reset: return "RESET";
    case Wake::PowerKey: return "PWR KEY";
    case Wake::Alarm: return "RTC ALARM";
    case Wake::Usb: return "USB";
    case Wake::Comms: return "COMMS";
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
  held = Held::None;
  if (audio::active() || audio::ringing() || audio::recording()) {
    const bool recording = audio::recording();
    audio::stop();  // also finishes a recording's file
    // The audio task mutes and switches the amp off first, so the speaker doesn't pop; a
    // recording needs its last write and header too.
    delay(recording ? 400 : 150);
  }
  if (sensors::logging()) sensors::setLogging(false);  // close the CSV before the power goes
  const bool reach = !noReach && reachWanted();
#if INKDECK_COMMS
  // ESP-NOW down before Wi-Fi goes. Not when the reachable sleep follows in setup(): comms hasn't
  // started, and this would keep it off for good.
  if (!reach || booted) comms::forceOff();
#endif
  wlan::forceOff();

  if (wakeAt) {
    const time_t now = time(nullptr);
    if (wakeAt < now + 2) wakeAt += 60 * ((now + 2 - wakeAt) / 60 + 1);
  }
  if (reach) {
    // Reachable on battery: the same record, no RTC alarm (it would hold PWRON low once it fired,
    // and 4 s of that forces a running board off); the sleep keeps the time instead.
    rtc::disarm();
    Preferences prefs;
    if (prefs.begin(kNamespace, false)) {
      prefs.putUChar("mode", static_cast<uint8_t>(m));
      prefs.putULong64("armed", static_cast<uint64_t>(wakeAt));
      prefs.putInt("cycles", standbyCycles);
      prefs.end();
    }
    mode = m;
    armed = wakeAt;
    cyclesLeft = standbyCycles;
    log_i("power: off (%s), reachable, wake at %ld", modeName(m), static_cast<long>(wakeAt));
    if (booted) restartFor(kMarkReach);  // the sleep runs first thing at boot, nothing else up
    sleepReachable();
    if (wakeReason == Wake::Comms) return;  // woken: the boot goes on
  }
  if (wakeAt) {
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
  log_i("power: off (%s), wake at %ld", modeName(m), static_cast<long>(wakeAt));
  if (pmic::powerOff()) delay(2000);  // power goes within milliseconds
  // Still here: the PMIC didn't take it. Undo, so the alarm can't hold PWRON low.
  log_e("power: the PMIC didn't power off");
  rtc::disarm();
  markOn();
}

void reachResume() {
  if (!reachPending) return;
  reachPending = false;
  if (!noReach && reachWanted()) {
    sleepReachable();
    if (wakeReason == Wake::Comms) return;
  }
  // The battery fell below kLowBatteryPercent, the setting went, or the sleep couldn't start: off
  // as without it, with the RTC alarm.
  noReach = true;
  off(mode, armed, cyclesLeft);
  noReach = false;  // still here: the PMIC didn't power off; boot normally
}

Mode reachMode() { return wokenMode; }

void consoleRecover() {
#if INKDECK_COMMS
  comms::reachLogFlush();
#endif
  usbControllerReset();
  // A whole power cycle resets everything the restart doesn't: off with the PMIC, the RTC alarm
  // boots the board 20 s later (as a standby wake). On USB the PMIC may not take it: then the
  // restart.
  markOn();
  if (rtc::timeValid() && rtc::arm(time(nullptr) + 20)) {
    log_i("power: a power cycle for the USB console: off, back in 20 s");
    delay(50);
    if (pmic::powerOff()) delay(3000);
    rtc::disarm();
    log_w("power: the PMIC didn't power off; restarting instead");
  }
  restartMark = kMarkMagic | kMarkPwr;
  esp_restart();
  for (;;) {
  }
}

void reachWokenBegin() {
#if INKDECK_COMMS
  if (wakeReason != Wake::Comms) return;
  held = Held::Stay;
  heldSinceMs = millis();
  comms::hold();
#endif
}

void bootDone() { booted = true; }

Held reachHold() { return held; }

void reachStep() {
#if INKDECK_COMMS
  if (held != Held::Stay) return;
  // A key since the wake, or an alarm ringing (it takes the board as an alarm boot would).
  const bool keyed = input::idleMs() < millis() - heldSinceMs || audio::ringing();
  switch (comms::reachWokenStep(keyed)) {
    case comms::Woken::Sleep:
      held = Held::Sleep;
      comms::release();
      log_i("power: woken for comms, and it's over: the face powers off again");
      break;
    case comms::Woken::On:
      held = Held::None;
      comms::release();
      log_i("power: woken for comms, and a key: on");
      break;
    case comms::Woken::None:
      // comms didn't take the wake (no group after all): back to sleep.
      if (millis() - heldSinceMs > 20000) {
        held = Held::Sleep;
        comms::release();
      }
      break;
    case comms::Woken::Stay: break;
  }
#endif
}

ReachTest reachTest() { return test; }

void setReachTest(ReachTest t, int minutes) {
  test = t;
  testEnd = t == ReachTest::Sleep && rtc::timeValid() ? time(nullptr) + 60 * std::max(1, minutes) : 0;
  Preferences prefs;
  if (prefs.begin(kNamespace, false)) {
    prefs.putUChar("rtest", static_cast<uint8_t>(t));
    prefs.putULong64("rtestend", static_cast<uint64_t>(testEnd));
    prefs.end();
  }
  pmic::pretendBattery(t != ReachTest::Off);
#if INKDECK_COMMS
  if (t != ReachTest::Off) comms::reachLogReset();
#endif
}

bool lowBattery(const pmic::Status &p) {
  return p.ok && !p.vbus && p.percent >= 0 && p.percent < kLowBatteryPercent;
}

bool lowBattery() { return lowBattery(pmic::status()); }

bool pwrHeld() { return digitalRead(board::PWR_KEY_SENSE) == HIGH; }

bool idle(bool reading) {
  if (input::idleMs() < kIdleMs) return false;
  const pmic::Status p = pmic::status();
  if (!p.ok || !p.battery || p.vbus) return false;  // on USB (or unknown): stay on
  if (audio::ringing()) return false;
  const bool playing =
      audio::status().state == audio::State::Playing || audio::recording();  // a memo too
  if ((reading || playing) && !lowBattery(p)) return false;
  return !working();
}

bool idleBook() {
  if (input::idleMs() < kBookIdleMs) return false;
  const pmic::Status p = pmic::status();
  if (!p.ok || !p.battery || p.vbus) return false;  // on USB (or unknown): the page stays
  return !audio::busy() && !working();
}

NapEnd nap(uint32_t maxMs, bool motion) {
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
  // The IMU toggles its pin at each wake-on-motion event, so wake on the level it isn't at.
  const gpio_num_t imu = static_cast<gpio_num_t>(board::IMU_INT2);
  const int imuBefore = gpio_get_level(imu);
  if (motion) {
    gpio_sleep_sel_dis(imu);
    gpio_wakeup_enable(imu, imuBefore ? GPIO_INTR_LOW_LEVEL : GPIO_INTR_HIGH_LEVEL);
  }
  esp_sleep_enable_gpio_wakeup();
  esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(maxMs) * 1000);
  esp_light_sleep_start();  // no Serial.flush(): it can block on a half-connected USB host
  const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  for (const int pin : activeLow) gpio_wakeup_disable(static_cast<gpio_num_t>(pin));
  gpio_wakeup_disable(static_cast<gpio_num_t>(board::PWR_KEY_SENSE));
  if (motion) gpio_wakeup_disable(imu);
  if (cause != ESP_SLEEP_WAKEUP_GPIO) return NapEnd::Timer;
  // A button that woke the board is still down (a press lasts far longer than the wake).
  bool button = gpio_get_level(static_cast<gpio_num_t>(board::PWR_KEY_SENSE));
  for (const int pin : activeLow) button = button || !gpio_get_level(static_cast<gpio_num_t>(pin));
  if (motion && !button && gpio_get_level(imu) != imuBefore) return NapEnd::Motion;
  return NapEnd::Button;
}

}  // namespace power
