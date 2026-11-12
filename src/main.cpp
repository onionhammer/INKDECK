// Prototype e-reader UI: browse the microSD card and read EPUBs.
//
// Controls
//   File browser   wheel up/down: move   wheel press: open folder/book   BOOT: parent folder
//                  BOOT hold: full refresh
//   Reader         wheel up/down: previous/next page   wheel press: next chapter
//                  wheel hold: chapter start / previous chapter
//                  BOOT: back to the browser   BOOT hold: cycle font size
//
// Quick and dirty on purpose: no PMIC/power management, no saved reading position, ASCII-only
// fonts, no images. See the per-module headers for their individual shortcuts.
#include <Arduino.h>
#include <SD_MMC.h>

#include "board_pins.h"
#include "browser.h"
#include "input.h"
#include "reader.h"
#include "screen.h"

SET_LOOP_TASK_STACK_SIZE(16 * 1024);  // XML scanning + layout; the default 8 KB is tight

namespace {

constexpr const char *kMountPoint = "/sdcard";

enum class Mode { Browser, Reader };

Mode mode = Mode::Browser;
Browser browser(kMountPoint);
Reader reader;
bool sdMounted = false;

bool mountSd() {
  SD_MMC.end();
  if (!SD_MMC.setPins(board::SD_CLK, board::SD_CMD, board::SD_D0, board::SD_D1, board::SD_D2,
                      board::SD_D3)) {
    log_e("SD: setPins failed");
    return false;
  }
  // 4-bit at 40 MHz like the vendor IDF app; fall back to 20 MHz if the card is unhappy.
  for (const int freq : {SDMMC_FREQ_HIGHSPEED, SDMMC_FREQ_DEFAULT}) {
    if (SD_MMC.begin(kMountPoint, false, false, freq, 5)) {
      log_i("SD: mounted at %d kHz, %llu MB card", freq, SD_MMC.cardSize() >> 20);
      return true;
    }
  }
  log_w("SD: mount failed (no card?)");
  return false;
}

// Applies one key. Returns true if the screen changed; sets full for a full refresh.
bool handleKey(input::Key key, bool &full) {
  using input::Key;
  if (mode == Mode::Browser) {
    browser.clearMessage();
    switch (key) {
      case Key::Up: browser.moveCursor(-1); return true;
      case Key::Down: browser.moveCursor(+1); return true;
      case Key::Back: browser.goUp(); return true;
      case Key::BackLong: full = true; return true;
      case Key::SelectLong: return false;
      case Key::Select: {
        if (!sdMounted) {
          sdMounted = mountSd();
          browser.open(kMountPoint);
          return true;
        }
        const Browser::Entry *e = browser.selected();
        if (!e) {
          browser.reload();
          return true;
        }
        if (e->isDir) {
          browser.open(browser.selectedPath());
          return true;
        }
        if (!Browser::isEpub(e->name)) {
          browser.setMessage("Not an EPUB");
          return true;
        }
        std::string err;
        if (reader.open(browser.selectedPath(), err)) {
          mode = Mode::Reader;
          full = true;
        } else {
          log_w("open failed: %s", err.c_str());
          browser.setMessage("Can't open: " + err);
        }
        return true;
      }
    }
  } else {
    switch (key) {
      case Key::Up: reader.prevPage(); return true;
      case Key::Down: reader.nextPage(); return true;
      case Key::Select: reader.nextChapter(); return true;
      case Key::SelectLong: reader.prevChapter(); return true;
      case Key::BackLong: reader.cycleFontSize(); return true;
      case Key::Back:
        reader.close();
        mode = Mode::Browser;
        full = true;
        return true;
    }
  }
  return false;
}

void render(bool full) {
  const uint32_t t0 = millis();
  screen::clear();
  if (mode == Mode::Browser) {
    browser.draw(screen::canvas());
  } else {
    reader.draw(screen::canvas());
  }
  const uint32_t t1 = millis();
  screen::present(full);
  log_i("render: draw %lu ms, panel %lu ms", (unsigned long)(t1 - t0),
        (unsigned long)(millis() - t1));
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(500);  // let the USB CDC host attach so the first logs aren't lost
  Serial.println("\n== ESP32-S3-ePaper-3.97 e-reader prototype ==");
  Serial.printf("PSRAM: %lu KB total, %lu KB free%s\n", ESP.getPsramSize() >> 10,
                ESP.getFreePsram() >> 10,
                ESP.getPsramSize() ? "" : "  <-- PSRAM missing: check memory_type");

  if (!screen::begin()) Serial.println("screen init failed");
  input::begin();
  sdMounted = mountSd();
  browser.open(kMountPoint);
  render(true);
}

void loop() {
  input::Key key;
  if (!input::wait(key, 60 * 1000)) return;

  // Apply every queued key before drawing, so fast scrolling doesn't queue up refreshes.
  bool dirty = false;
  bool full = false;
  do {
    log_d("key %s", input::name(key));
    dirty |= handleKey(key, full);
  } while (input::wait(key, 0));

  if (dirty) render(full);
}
