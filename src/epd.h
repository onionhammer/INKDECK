// SSD1677 driver for the 3.97" 800x480 B/W panel. Write-only 4-wire SPI.
//
// Every update is self-contained: hardware reset -> SWRESET -> init -> write both RAMs ->
// refresh -> deep sleep. Because the "old" RAM (0x26) is rewritten from our own copy of the
// last frame each time, partial refreshes survive panel deep sleep and an ALDO3 power cut
// (powerOff()) without any extra bookkeeping.
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

  // Records frame as what the panel shows now, without refreshing: after a power-off the panel
  // still holds its image, and redrawing it here lets the next show() be partial.
  void assumeShown(const uint8_t *frame);

  // Light-sleeps the chip while the panel is busy, instead of polling BUSY at full clock;
  // BUSY going low wakes it. Through a refresh that's well under a milliamp instead of tens.
  // Light sleep drops a USB serial connection, so the standby wake turns it on only on battery.
  void setSleepWhileBusy(bool on) { sleepWhileBusy_ = on; }

  // Cuts the panel's power (ALDO3): the image stays, the controller and its RAM go, and the pins
  // are driven low so they don't feed the unpowered controller through its inputs. The next
  // show() powers it again, 10 ms before its usual reset. Saves the ~0.3 mA its power switch
  // (Q3's base and Q2's gate pull-up) draws while ALDO3 is on, deep sleep or not.
  void powerOff();

 private:
  void powerOn();
  void reset();
  bool waitIdle(uint32_t timeoutMs);
  void sleepUntilIdle(uint32_t maxMs);
  void command(uint8_t cmd);
  void data(const uint8_t *buf, size_t len);
  void data8(uint8_t value) { data(&value, 1); }
  bool initController(bool full);
  void writeRam(uint8_t cmd, const uint8_t *frame);

  uint8_t *prev_ = nullptr;  // last frame shown, PSRAM
  bool hasPrev_ = false;
  bool sleepWhileBusy_ = false;
  bool powered_ = true;  // ALDO3 is on at power-up
};
