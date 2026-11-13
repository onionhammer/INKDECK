// INKDECK/OS firmware (docs/ui-design.md): a stack of screens driven by the rotary switch and
// BOOT, rendered to the e-paper by the navigator in ui/nav.h.
//
// Keys, the same on every screen (details in ui/nav.h):
//   ▲ / ▼ dial: move, page, adjust      ●  primary action      HOLD ●  secondary action
//   BOOT  back                          HOLD BOOT  home (with a full refresh)
//   PWR   the Sleep screen, then power-off (power.h)
//
// Screens live in apps/: Home lists the modules, Files browses the SD card, Reader shows books
// (● opens its menu), Music plays MP3s, Clock (landscape) keeps the alarms. Progress and plans
// are in PROGRESS.md.
#include <Arduino.h>
#include <Wire.h>

#include "alarms.h"
#include "apps/clock.h"
#include "apps/home.h"
#include "apps/idle.h"
#include "apps/music.h"
#include "apps/sleep.h"
#include "audio.h"
#include "board_pins.h"
#include "devtools.h"
#include "input.h"
#include "netget.h"
#include "pmic.h"
#include "podcast_library.h"
#include "power.h"
#include "rtc.h"
#include "screen.h"
#include "sdcard.h"
#include "sensor_hub.h"
#include "settings.h"
#include "timesync.h"
#include "trackinfo.h"
#include "ui/nav.h"
#include "forecast.h"
#include "wlan.h"

SET_LOOP_TASK_STACK_SIZE(16 * 1024);  // XML scanning + layout; the default 8 KB is tight

void setup() {
  const uint32_t bootMs = millis();
  Serial.begin(115200);
  Wire.begin(board::I2C_SDA, board::I2C_SCL, 400000);  // the one shared I2C bus
  pmic::configure();  // charger and PWR-key settings; they don't survive a total power loss
  rtc::begin();
  power::begin();  // why the board started; releases the RTC alarm's hold on PWRON
  // Let the USB CDC host attach so the first logs aren't lost; not on RTC wakes, which are
  // usually on battery and should be short.
  if (power::wake() != power::Wake::Alarm) delay(500);
  Serial.println("\n== ESP32-S3-ePaper-3.97 INKDECK/OS ==");
  Serial.printf("PSRAM: %lu KB total, %lu KB free%s; setup() at %lu ms\n",
                ESP.getPsramSize() >> 10, ESP.getFreePsram() >> 10,
                ESP.getPsramSize() ? "" : "  <-- PSRAM missing: check memory_type",
                static_cast<unsigned long>(bootMs));
  alarms::begin();
  if (!screen::begin()) Serial.println("screen init failed");
  // A standby wake with no alarm due redraws the clock face and powers straight off again.
  apps::standbyWake();
  power::markOn();
  pmic::status();  // first read logs what the PMIC reports
  net::begin();  // TLS buffers in PSRAM
  sensors::begin();
  input::begin();
  audio::begin();
  trackinfo::begin();
  sdcard::mount();
  settings::loadCard();  // SD:/inkdeck.ini: time zone and location
  ui::nav::begin(apps::makeHome());
  ui::nav::onPowerKey(apps::powerKey);
  ui::nav::onHoldBack(apps::openPlayer);
  // Woken for an alarm: show the Clock, and pollAlarms() rings over it.
  if (power::wake() == power::Wake::Alarm) ui::nav::push(apps::makeClock());
  devtools::begin();  // after the first frame, so a screenshot never catches a blank buffer
  // The clock came up unset (first boot, or the battery was out): fetch it from NTP once, in
  // the background, after the first frame. If that fails, System → SYNC TIME tries by hand.
  if (!rtc::timeValid() && wlan::configured()) {
    log_i("RTC unset: syncing the time from NTP");
    timesync::start();
  }
}

void loop() {
  apps::pollAlarms();
  apps::pollPower();
  weather::poll();  // the hourly forecast sync, in the background
  podcasts::poll();  // saves the playing episode's position
  if (apps::napIfIdle()) return;  // reading on battery: light sleep until a key or the minute
  ui::nav::loop();
}
