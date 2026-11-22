// INKDECK/OS firmware (docs/ui-design.md): a stack of screens driven by the rotary switch and
// BOOT, rendered to the e-paper by the navigator in ui/nav.h.
//
// Keys, the same on every screen (details in ui/nav.h):
//   ▲ / ▼ dial: move, page, adjust      ●  primary action      HOLD ●  secondary action
//   BOOT  back (on Home: full refresh)  HOLD BOOT  the player sheet while music plays, else
//                                       talk to the assistant (apps/hold_boot.h)
//   PWR   the Sleep screen, then power-off (power.h)
//
// Screens live in apps/, one per module on Home (README.md lists them): Reader, Podcasts,
// Agent, Weather, Clock, Recorder, Music, Sensors (its menu opens the Level), Files, System and
// News. With a PIN set in SD:/inkdeck.ini, the lock (apps/lock.h) comes up over them at every
// start.
#include <Arduino.h>
#include <Wire.h>

#include "alarms.h"
#include "apps/clock.h"
#include "apps/hold_boot.h"
#include "apps/home.h"
#include "apps/idle.h"
#include "apps/lock.h"
#include "apps/news.h"
#include "apps/news_edition.h"
#include "apps/reader.h"
#include "apps/sleep.h"
#include "apps/agent.h"
#include "apps/agent_face.h"
#include "apps/board_tools.h"
#include "audio.h"
#include "board_pins.h"
#include "comms.h"
#include "devtools.h"
#include "features.h"
#include "input.h"
#include "netget.h"
#include "ota.h"
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
#include "tailnet.h"
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
  // So does an agent check-in from the Agent face, Sleep or a closed book (apps/agent_face.h).
  apps::agentWake();
  // And News's off face, each minute (apps/news_edition.h); true if PWR was pressed.
  const bool editionPwr = apps::editionWake();
  // Powered off behind a book's cover (apps/closed_book.h): this start opens the book again.
  const bool reopenBook = power::lastMode() == power::Mode::Book;
  // Behind the Agent face, HOLD PWR (not an alarm) opens the Agent.
  const bool reopenAgent =
      power::lastMode() == power::Mode::Agent && power::wake() != power::Wake::Alarm;
  // Behind News's face, HOLD PWR opens News on the story it showed (so does PWR during a wake).
  const bool reopenNews = power::lastMode() == power::Mode::Edition &&
                          (power::wake() != power::Wake::Alarm || editionPwr);
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
  apps::beginHoldBoot();
  if (reopenBook) {
    if (auto book = apps::resumeLastBook()) ui::nav::push(std::move(book));
  }
#if INKDECK_AGENT
  if (reopenAgent) apps::openGuarded(apps::Guarded::Agent, apps::makeAgent);  // the wheels first
#else
  (void)reopenAgent;
#endif
  if (reopenNews) apps::openNewsStory(apps::faceStory());
  // Woken for an alarm: show the Clock, and pollAlarms() rings over it.
  if (power::wake() == power::Wake::Alarm && !editionPwr) ui::nav::push(apps::makeClock());
  apps::lockNow();  // with a PIN and power_on (pin_places.h), every start asks (alarms ring over it)
  ui::nav::flush();  // the first frame: Home, the Clock or the lock, whichever is on top
  devtools::begin();  // after the first frame, so a screenshot never catches a blank buffer
  // The clock came up unset (first boot, or the battery was out): fetch it from NTP once, in
  // the background, after the first frame. If that fails, System → SYNC TIME tries by hand.
  if (!rtc::timeValid() && wlan::configured()) {
    log_i("RTC unset: syncing the time from NTP");
    timesync::start();
  }
  tailnet::begin();  // on USB, the board stays on the tailnet (tailnet.h)
#if INKDECK_COMMS
  comms::begin();  // with a group, listens for the other boards on USB (comms.h)
#endif
  // Made it through setup() with a frame on the panel: if this is the first boot after an
  // update over Wi-Fi, keep the new image (until now a reset would have rolled it back).
  ota::confirmBoot();
}

void loop() {
  apps::pollAlarms();
  apps::pollPower();
  apps::pollLock();  // before the nap: a lock over the Reader ends the naps and gets drawn
  apps::pollBoardTools();  // the assistant's calls to the board, and the screens they open
#if INKDECK_AGENT
  apps::pollAgent();  // chirps, the ask sheet; on a nap's minute wake too, before it naps again
#endif
  // The hourly forecast sync, in the background. Not while the Reader naps on battery: that
  // would wake the radio every hour (every 15 minutes out of range) with nobody reading. It
  // runs after the next key instead. Nor under the last frame before a power-off, which would
  // cut it short.
  const ui::Screen *top = ui::nav::top();
  if (!apps::isNapping() && !(top && top->offFace())) weather::poll();
  podcasts::poll();  // saves the playing episode's position
  apps::pollNews();  // hourly syncs while News is open or on USB
  if (apps::napIfIdle()) return;  // reading on battery: light sleep until a key or the minute
  ui::nav::loop();
}
