// INKDECK/OS firmware (docs/ui-design.md): a stack of screens driven by the rotary switch and
// BOOT, rendered to the e-paper by the navigator in ui/nav.h.
//
// Keys, the same on every screen (details in ui/nav.h):
//   ▲ / ▼ dial: move, page, adjust      ●  primary action      HOLD ●  secondary action
//   BOOT  back                          HOLD BOOT  home (with a full refresh)
//
// Screens live in apps/: Home lists the modules, Files browses the SD card, Reader shows books
// (● opens its menu), Music plays MP3s. Progress and plans are in PROGRESS.md.
#include <Arduino.h>
#include <Wire.h>

#include "apps/home.h"
#include "audio.h"
#include "board_pins.h"
#include "devtools.h"
#include "input.h"
#include "pmic.h"
#include "rtc.h"
#include "screen.h"
#include "sdcard.h"
#include "trackinfo.h"
#include "ui/nav.h"

SET_LOOP_TASK_STACK_SIZE(16 * 1024);  // XML scanning + layout; the default 8 KB is tight

void setup() {
  Serial.begin(115200);
  delay(500);  // let the USB CDC host attach so the first logs aren't lost
  Serial.println("\n== ESP32-S3-ePaper-3.97 INKDECK/OS ==");
  Serial.printf("PSRAM: %lu KB total, %lu KB free%s\n", ESP.getPsramSize() >> 10,
                ESP.getFreePsram() >> 10,
                ESP.getPsramSize() ? "" : "  <-- PSRAM missing: check memory_type");

  Wire.begin(board::I2C_SDA, board::I2C_SCL, 400000);  // the one shared I2C bus
  pmic::configure();  // charger and PWR-key settings; they don't survive a total power loss
  rtc::begin();
  pmic::status();  // first read logs what the PMIC reports
  if (!screen::begin()) Serial.println("screen init failed");
  input::begin();
  audio::begin();
  trackinfo::begin();
  sdcard::mount();
  ui::nav::begin(apps::makeHome());
  devtools::begin();  // after the first frame, so a screenshot never catches a blank buffer
}

void loop() { ui::nav::loop(); }
