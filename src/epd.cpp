#include "epd.h"

#include <Arduino.h>
#include <SPI.h>
#include <driver/gpio.h>
#include <esp_heap_caps.h>
#include <esp_sleep.h>
#include <string.h>

#include "board_pins.h"
#include "pmic.h"

// Command bytes and init values follow Waveshare's EPD_Init / EPD_Display_Base /
// EPD_Display_Partial (see .claude/skills/waveshare-epaper-reference/references/epaper.md §3).
// Deviations, all deliberate:
//  - CS framed per transfer (vendor leaves it asserted).
//  - 0x4F Y counter starts at 479 (datasheet reference program) instead of 0, which should
//    avoid the one-row roll noted in the reference's pitfall #3. Unverified on hardware.
//  - Partial refresh rewrites 0x26 with the previous frame instead of relying on the
//    controller having kept it.
//  - BUSY waits poll with a timeout instead of looping forever.

namespace {

SPIClass epdSpi(HSPI);  // SPI3, as in the vendor driver
const SPISettings kSpiSettings(20000000, MSBFIRST, SPI_MODE0);  // 20 MHz = datasheet max

constexpr uint8_t kBoosterSoftStart[] = {0xAE, 0xC7, 0xC3, 0xC0, 0x80};
constexpr uint8_t kDriverOutput[] = {(Epd::kHeight - 1) & 0xFF, (Epd::kHeight - 1) >> 8, 0x02};
constexpr uint8_t kRamXWindow[] = {0x00, 0x00, (Epd::kWidth - 1) & 0xFF, (Epd::kWidth - 1) >> 8};
constexpr uint8_t kRamYWindow[] = {(Epd::kHeight - 1) & 0xFF, (Epd::kHeight - 1) >> 8, 0x00, 0x00};
constexpr uint8_t kRamXStart[] = {0x00, 0x00};
constexpr uint8_t kRamYStart[] = {(Epd::kHeight - 1) & 0xFF, (Epd::kHeight - 1) >> 8};

}  // namespace

bool Epd::begin() {
  // A software reset (USB plugged into a napping board, an update, a crash) leaves the PMIC as
  // it was, so ALDO3 may still be off from a nap. Waits 10 ms only if it was.
  pmic::setPanelRail(true);
  pinMode(board::EPD_CS, OUTPUT);
  digitalWrite(board::EPD_CS, HIGH);
  pinMode(board::EPD_DC, OUTPUT);
  pinMode(board::EPD_RST, OUTPUT);
  digitalWrite(board::EPD_RST, HIGH);
  pinMode(board::EPD_BUSY, INPUT_PULLUP);  // HIGH = busy; pull-up makes a dead panel time out
  epdSpi.begin(board::EPD_SCK, -1, board::EPD_MOSI, -1);

  prev_ = static_cast<uint8_t *>(heap_caps_malloc(kFrameBytes, MALLOC_CAP_SPIRAM));
  if (!prev_) {
    log_e("EPD: no PSRAM for previous-frame buffer");
    return false;
  }
  return true;
}

void Epd::powerOff() {
  if (!powered_) return;
  // Every update ends in deep sleep, so the controller is idle. The SPI pins are detached and
  // driven low with the rest (the last bit sent may have left MOSI high), BUSY pulled down
  // rather than up into the unpowered pin; Waveshare's EPD_Sleep() also drops RST/CS/DC first.
  epdSpi.end();
  const int low[] = {board::EPD_CS, board::EPD_DC, board::EPD_RST, board::EPD_SCK, board::EPD_MOSI};
  for (const int pin : low) {
    pinMode(pin, OUTPUT);
    digitalWrite(pin, LOW);
  }
  pinMode(board::EPD_BUSY, INPUT_PULLDOWN);
  pmic::setPanelRail(false);
  powered_ = false;
}

void Epd::powerOn() {
  if (powered_) return;
  pmic::setPanelRail(true);  // waits 10 ms for the rail, as Waveshare's EPD_Init() does
  pinMode(board::EPD_BUSY, INPUT_PULLUP);
  digitalWrite(board::EPD_CS, HIGH);
  digitalWrite(board::EPD_RST, HIGH);
  epdSpi.begin(board::EPD_SCK, -1, board::EPD_MOSI, -1);
  powered_ = true;
}

void Epd::reset() {
  digitalWrite(board::EPD_RST, HIGH);
  delay(20);
  digitalWrite(board::EPD_RST, LOW);
  delay(2);
  digitalWrite(board::EPD_RST, HIGH);
  delay(20);
}

bool Epd::waitIdle(uint32_t timeoutMs) {
  const uint32_t start = millis();
  delay(1);
  while (digitalRead(board::EPD_BUSY) == HIGH) {
    const uint32_t waited = millis() - start;
    if (waited > timeoutMs) {
      log_e("EPD: BUSY timeout after %lu ms", (unsigned long)timeoutMs);
      return false;
    }
    if (sleepWhileBusy_) {
      sleepUntilIdle(timeoutMs - waited + 1);
    } else {
      delay(2);
    }
  }
  return true;
}

void Epd::sleepUntilIdle(uint32_t maxMs) {
  // The panel's pins keep their awake configuration through the sleep, as power::nap() does
  // for the buttons: RST let go low mid-refresh would reset the controller.
  const int pins[] = {board::EPD_CS, board::EPD_DC, board::EPD_RST, board::EPD_BUSY};
  for (const int pin : pins) gpio_sleep_sel_dis(static_cast<gpio_num_t>(pin));
  const gpio_num_t busy = static_cast<gpio_num_t>(board::EPD_BUSY);
  gpio_wakeup_enable(busy, GPIO_INTR_LOW_LEVEL);  // already low: wakes at once
  esp_sleep_enable_gpio_wakeup();
  esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(maxMs) * 1000);
  esp_light_sleep_start();
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  gpio_wakeup_disable(busy);
}

void Epd::command(uint8_t cmd) {
  digitalWrite(board::EPD_DC, LOW);
  digitalWrite(board::EPD_CS, LOW);
  epdSpi.transfer(cmd);
  digitalWrite(board::EPD_CS, HIGH);
}

void Epd::data(const uint8_t *buf, size_t len) {
  digitalWrite(board::EPD_DC, HIGH);
  digitalWrite(board::EPD_CS, LOW);
  epdSpi.writeBytes(buf, len);  // CPU-fed FIFO, so PSRAM sources are fine
  digitalWrite(board::EPD_CS, HIGH);
}

bool Epd::initController(bool full) {
  reset();
  if (!waitIdle(1000)) return false;
  command(0x12);  // SWRESET
  if (!waitIdle(1000)) return false;

  command(0x18);  // temperature sensor: internal
  data8(0x80);
  command(0x0C);  // booster soft start
  data(kBoosterSoftStart, sizeof kBoosterSoftStart);
  command(0x01);  // driver output control: 480 gates
  data(kDriverOutput, sizeof kDriverOutput);
  command(0x3C);  // border waveform: LUT1 for full, VCOM for partial (as vendor)
  data8(full ? 0x01 : 0x80);
  command(0x11);  // data entry: X increment, Y decrement
  data8(0x01);
  command(0x44);
  data(kRamXWindow, sizeof kRamXWindow);
  command(0x45);
  data(kRamYWindow, sizeof kRamYWindow);
  return waitIdle(1000);
}

void Epd::writeRam(uint8_t cmd, const uint8_t *frame) {
  command(0x4E);
  data(kRamXStart, sizeof kRamXStart);
  command(0x4F);
  data(kRamYStart, sizeof kRamYStart);
  command(cmd);
  data(frame, kFrameBytes);
}

uint32_t Epd::changedPixels(const uint8_t *frame) const {
  if (!prev_ || !hasPrev_) return kWidth * kHeight;
  uint32_t n = 0;
  for (size_t i = 0; i < kFrameBytes; ++i) n += __builtin_popcount(frame[i] ^ prev_[i]);
  return n;
}

void Epd::assumeShown(const uint8_t *frame) {
  if (!prev_) return;
  memcpy(prev_, frame, kFrameBytes);
  hasPrev_ = true;
}

bool Epd::show(const uint8_t *frame, bool full) {
  if (!prev_) return false;
  if (!hasPrev_) full = true;

  const uint32_t t0 = millis();
  powerOn();  // the last update cut the supply; the reset and init below are the same either way
  epdSpi.beginTransaction(kSpiSettings);
  bool ok = initController(full);
  if (ok) {
    writeRam(0x24, frame);                  // new image
    writeRam(0x26, full ? frame : prev_);   // old image, for the differential waveform
    command(0x22);
    data8(full ? 0xF7 : 0xFF);              // F7 = full (mode 1), FF = partial (mode 2)
    command(0x20);
    ok = waitIdle(full ? 10000 : 5000);
    command(0x10);  // deep sleep; the next show() wakes it with a hardware reset
    data8(0x01);
  }
  epdSpi.endTransaction();

  if (ok) {
    memcpy(prev_, frame, kFrameBytes);
    hasPrev_ = true;
  } else {
    hasPrev_ = false;  // unknown panel state: force a full refresh next time
  }
  // The panel holds its image unpowered, and its power switch draws about 0.3 mA while ALDO3 is
  // on, so the supply goes off again with every update.
  powerOff();
  log_i("EPD: %s refresh %s in %lu ms", full ? "full" : "partial", ok ? "done" : "FAILED",
        (unsigned long)(millis() - t0));
  return ok;
}
