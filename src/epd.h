// SSD1677 driver for the 3.97" 800x480 B/W panel. Write-only 4-wire SPI.
//
// Every update is self-contained: hardware reset -> SWRESET -> init -> write both RAMs ->
// refresh -> deep sleep. Because the "old" RAM (0x26) is rewritten from our own copy of the
// last frame each time, partial refreshes survive panel deep sleep (and an ALDO3 power cut,
// once we start doing that) without any extra bookkeeping.
#pragma once
#include <stddef.h>
#include <stdint.h>

class Epd {
 public:
  static constexpr int kWidth = 800;   // native orientation (landscape)
  static constexpr int kHeight = 480;
  static constexpr size_t kFrameBytes = kWidth / 8 * kHeight;  // 48,000

  bool begin();

  // frame: native-layout 1 bpp buffer, row-major, MSB = leftmost pixel, 1 = white.
  // full=false does a differential (partial) refresh against the previously shown frame.
  // The first update after begin() is always full.
  bool show(const uint8_t *frame, bool full);

  // Pixels that differ between frame and the frame on the panel; every pixel when the panel's
  // content is unknown (before the first update, after a failed one).
  uint32_t changedPixels(const uint8_t *frame) const;

 private:
  void reset();
  bool waitIdle(uint32_t timeoutMs);
  void command(uint8_t cmd);
  void data(const uint8_t *buf, size_t len);
  void data8(uint8_t value) { data(&value, 1); }
  bool initController(bool full);
  void writeRam(uint8_t cmd, const uint8_t *frame);

  uint8_t *prev_ = nullptr;  // last frame shown, PSRAM
  bool hasPrev_ = false;
};
