// Prototype e-reader UI: browse the microSD card, read EPUBs, play MP3s.
//
// Controls
//   File browser   wheel up/down: move   wheel press: open folder/book, play MP3
//                  wheel hold: now-playing screen (while music is on)
//                  BOOT: parent folder   BOOT hold: full refresh
//   Reader         wheel up/down: previous/next page   wheel press: next chapter
//                  wheel hold: chapter start / previous chapter
//                  BOOT: back to the browser   BOOT hold: cycle font size
//   Now playing    wheel up/down: volume   wheel press: pause/resume   wheel hold: next track
//                  BOOT: back to the browser (keeps playing)   BOOT hold: stop
//
// Playing an MP3 queues the rest of the MP3s in that folder after it. Music keeps playing in
// the browser and the reader.
//
// Quick and dirty on purpose: no PMIC/power management, no saved reading position, ASCII-only
// fonts, no images. See the per-module headers for their individual shortcuts.
#include <Arduino.h>
#include <SD_MMC.h>
#include <Wire.h>

#include "audio.h"
#include "board_pins.h"
#include "browser.h"
#include "input.h"
#include "player_screen.h"
#include "reader.h"
#include "screen.h"

SET_LOOP_TASK_STACK_SIZE(16 * 1024);  // XML scanning + layout; the default 8 KB is tight

namespace {

constexpr const char *kMountPoint = "/sdcard";
constexpr int kVolumeStep = 5;

enum class Mode { Browser, Reader, NowPlaying };

Mode mode = Mode::Browser;
Browser browser(kMountPoint);
Reader reader;
bool sdMounted = false;
uint32_t drawnAudioGeneration = 0;
bool sentAudioCommand = false;  // set by handleKey; render waits briefly for the audio task

bool mountSd() {
  SD_MMC.end();
  if (!SD_MMC.setPins(board::SD_CLK, board::SD_CMD, board::SD_D0, board::SD_D1, board::SD_D2,
                      board::SD_D3)) {
    log_e("SD: setPins failed");
    return false;
  }
  // 4-bit at 20 MHz. 40 MHz (the vendor IDF app's setting) mounted fine but produced
  // intermittent read CRC errors (0x109) on our board/card; 20 MHz is still ~10x what an
  // MP3 or a chapter load needs.
  if (SD_MMC.begin(kMountPoint, false, false, SDMMC_FREQ_DEFAULT, 5)) {
    log_i("SD: mounted at %d kHz, %llu MB card", SDMMC_FREQ_DEFAULT, SD_MMC.cardSize() >> 20);
    return true;
  }
  log_w("SD: mount failed (no card?)");
  return false;
}

// Browser: open whatever is under the cursor.
void openSelected(bool &full) {
  if (!sdMounted) {
    sdMounted = mountSd();
    browser.open(kMountPoint);
    return;
  }
  const Browser::Entry *e = browser.selected();
  if (!e) {
    browser.reload();
    return;
  }
  if (e->isDir) {
    browser.open(browser.selectedPath());
    return;
  }
  if (Browser::isMp3(e->name)) {
    const std::string path = browser.selectedPath();
    if (!(audio::active() && audio::status().path == path)) {
      int index;
      std::vector<std::string> playlist = browser.filePaths(Browser::isMp3, index);
      audio::play(std::move(playlist), index);
      sentAudioCommand = true;
    }
    mode = Mode::NowPlaying;
    return;
  }
  if (!Browser::isEpub(e->name)) {
    browser.setMessage("Not an EPUB or MP3");
    return;
  }
  std::string err;
  if (reader.open(browser.selectedPath(), err)) {
    mode = Mode::Reader;
    full = true;
  } else {
    log_w("open failed: %s", err.c_str());
    browser.setMessage("Can't open: " + err);
  }
}

// Applies one key. Returns true if the screen changed; sets full for a full refresh.
bool handleKey(input::Key key, bool &full) {
  using input::Key;
  switch (mode) {
    case Mode::Browser:
      browser.clearMessage();
      switch (key) {
        case Key::Up: browser.moveCursor(-1); return true;
        case Key::Down: browser.moveCursor(+1); return true;
        case Key::Select: openSelected(full); return true;
        case Key::SelectLong:
          if (!audio::active()) return false;
          mode = Mode::NowPlaying;
          return true;
        case Key::Back: browser.goUp(); return true;
        case Key::BackLong: full = true; return true;
      }
      break;

    case Mode::Reader:
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
      break;

    case Mode::NowPlaying:
      sentAudioCommand = true;
      switch (key) {
        case Key::Up: audio::changeVolume(+kVolumeStep); return true;
        case Key::Down: audio::changeVolume(-kVolumeStep); return true;
        case Key::Select: audio::togglePause(); return true;
        case Key::SelectLong: audio::next(); return true;
        case Key::Back: mode = Mode::Browser; return true;
        case Key::BackLong:
          audio::stop();
          mode = Mode::Browser;
          return true;
      }
      break;
  }
  return false;
}

void render(bool full) {
  const uint32_t t0 = millis();
  screen::clear();
  drawnAudioGeneration = audio::status().generation;
  switch (mode) {
    case Mode::Browser: browser.draw(screen::canvas()); break;
    case Mode::Reader: reader.draw(screen::canvas()); break;
    case Mode::NowPlaying: drawNowPlaying(screen::canvas()); break;
  }
  const uint32_t t1 = millis();
  screen::present(full);
  log_i("render: draw %lu ms, panel %lu ms", (unsigned long)(t1 - t0),
        (unsigned long)(millis() - t1));
}

// Screens that show player state get redrawn when the track or play state changes.
bool showsAudio() { return mode == Mode::Browser || mode == Mode::NowPlaying; }

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(500);  // let the USB CDC host attach so the first logs aren't lost
  Serial.println("\n== ESP32-S3-ePaper-3.97 e-reader prototype ==");
  Serial.printf("PSRAM: %lu KB total, %lu KB free%s\n", ESP.getPsramSize() >> 10,
                ESP.getFreePsram() >> 10,
                ESP.getPsramSize() ? "" : "  <-- PSRAM missing: check memory_type");

  Wire.begin(board::I2C_SDA, board::I2C_SCL, 400000);  // the one shared I2C bus
  if (!screen::begin()) Serial.println("screen init failed");
  input::begin();
  audio::begin();
  sdMounted = mountSd();
  browser.open(kMountPoint);
  render(true);
}

void loop() {
  input::Key key;
  const uint32_t timeout = showsAudio() && audio::active() ? 1000 : 60 * 1000;
  if (!input::wait(key, timeout)) {
    // A track ended or playback stopped on its own: refresh the screens that show it.
    if (showsAudio() && audio::status().generation != drawnAudioGeneration) render(false);
    return;
  }

  // Apply every queued key before drawing, so fast scrolling doesn't queue up refreshes.
  bool dirty = false;
  bool full = false;
  do {
    log_d("key %s", input::name(key));
    dirty |= handleKey(key, full);
  } while (input::wait(key, 0));

  if (dirty) {
    // Player commands are asynchronous; give the audio task a moment so the screen shows the
    // new state (paused, next track) rather than the old one.
    if (sentAudioCommand) delay(50);
    sentAudioCommand = false;
    render(full);
  }
}
