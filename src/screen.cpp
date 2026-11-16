#include "screen.h"

#include <Arduino.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <algorithm>

#include "epd.h"
#include "typeset.h"

namespace screen {
namespace {

// Rotation 3 = the vendor's ROTATE_270 portrait mapping (memX = y, memY = 479 - x).
// Change to 1 if the enclosure ends up holding the board the other way up.
constexpr uint8_t kRotation = 3;
// Landscape is the panel's native orientation turned 180 degrees (memX = 799 - x,
// memY = 479 - y), so the rotary wheel is on the top edge rather than the bottom. The navigator
// swaps the dial's keys to match (ui/nav.cpp).
constexpr uint8_t kLandscapeRotation = 2;
// Ghosting builds up with the pixels that partial refreshes flip, so the full refresh comes
// when the flips since the last one add up to fullScreens whole screens, or after
// kPartialsPerScreen partial updates per screen however small, whichever is first. Small live
// updates (a reading, a playhead) flip a few thousand pixels, so they rarely force a flash.
constexpr uint32_t kScreenPixels = uint32_t{Epd::kWidth} * Epd::kHeight;
constexpr int kPartialsPerScreen = 15;
constexpr int kDefaultFullScreens = 2;
constexpr int kMaxFullScreens = 100;
constexpr const char *kPrefs = "screen";

Epd epd;
GFXcanvas1 *fb = nullptr;
int partials = 0;
uint64_t flips = 0;  // pixels flipped by partial refreshes since the last full one
int fullScreens = kDefaultFullScreens;  // 0: no automatic full refresh
SemaphoreHandle_t mutex = nullptr;

uint32_t crc32(const uint8_t *data, size_t n) {
  uint32_t crc = 0xFFFFFFFF;
  for (size_t i = 0; i < n; ++i) {
    crc ^= data[i];
    for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320 & (0 - (crc & 1)));
  }
  return ~crc;
}

}  // namespace

bool begin() {
  mutex = xSemaphoreCreateRecursiveMutex();
  // GFXcanvas1's native layout (row-major, MSB = leftmost, set bit = colour 1) is exactly the
  // panel's RAM format, so its buffer goes straight to the controller. malloc() puts 48 KB in
  // PSRAM (CONFIG_SPIRAM_USE_MALLOC, threshold 4 KB).
  fb = new GFXcanvas1(Epd::kWidth, Epd::kHeight);
  if (!fb || !fb->getBuffer()) {
    log_e("screen: framebuffer allocation failed");
    return false;
  }
  fb->setRotation(kRotation);
  fb->setTextWrap(false);
  clear();
  Preferences prefs;
  if (prefs.begin(kPrefs, true)) {  // read-only fails until the setting is first saved
    fullScreens = std::min<int>(prefs.getUChar("full", kDefaultFullScreens), kMaxFullScreens);
    prefs.end();
  }
  return epd.begin();
}

GFXcanvas1 &canvas() { return *fb; }

void clear() { fb->fillScreen(gfx::kWhite); }

bool present(bool forceFull) {
  const uint32_t changed = epd.changedPixels(fb->getBuffer());
  if (!forceFull && changed == 0) return false;  // nothing visible changed: skip the refresh
  const bool budgetSpent =
      fullScreens > 0 && (partials >= fullScreens * kPartialsPerScreen ||
                          flips + changed > uint64_t(fullScreens) * kScreenPixels);
  const bool full = forceFull || budgetSpent;
  // What had built up before this refresh: what a full one cleared, or a partial adds to.
  const int partialsBefore = partials;
  const uint64_t flipsBefore = flips;
  if (epd.show(fb->getBuffer(), full)) {
    partials = full ? 0 : partials + 1;
    flips = full ? 0 : flips + changed;
  }
  if (full) {
    log_i("screen: FULL refresh (%s) after %d partials, %.2f screens flipped",
          forceFull ? "asked" : "budget", partialsBefore, flipsBefore / float(kScreenPixels));
  } else {
    log_i("screen: partial, %lu px (%.2f screens); since full: %d partials, %.2f screens of %d",
          (unsigned long)changed, changed / float(kScreenPixels), partials,
          flips / float(kScreenPixels), fullScreens);
  }
  return true;
}

int fullRefreshScreens() { return fullScreens; }

void setFullRefreshScreens(int screens) {
  fullScreens = std::max(0, std::min(screens, kMaxFullScreens));
  Preferences prefs;
  if (!prefs.begin(kPrefs, false)) return;
  prefs.putUChar("full", static_cast<uint8_t>(fullScreens));
  prefs.end();
}

void assumeShown() { epd.assumeShown(fb->getBuffer()); }

void powerOff() {
  lock();  // not in the middle of a frame
  epd.powerOff();
  unlock();
}

void sleepWhileBusy(bool on) { epd.setSleepWhileBusy(on); }

void setLandscape(bool landscape) { fb->setRotation(landscape ? kLandscapeRotation : kRotation); }

bool landscape() { return fb->getRotation() == kLandscapeRotation; }

void lock() { xSemaphoreTakeRecursive(mutex, portMAX_DELAY); }

void unlock() { xSemaphoreGiveRecursive(mutex); }

void dump(Print &out) {
  static const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  lock();
  const uint8_t *buf = fb->getBuffer();
  const size_t n = Epd::kWidth / 8 * Epd::kHeight;
  out.printf("\n#FB %d %d %d\n", Epd::kWidth, Epd::kHeight, fb->getRotation());
  char line[8 + 76 + 2];
  int lineNo = 0;
  for (size_t i = 0; i < n; i += 57) {  // 57 bytes -> 76 base64 characters per line
    int len = snprintf(line, sizeof line, "#%d ", lineNo++);
    for (size_t j = i; j < i + 57 && j < n; j += 3) {
      const uint32_t v = buf[j] << 16 | (j + 1 < n ? buf[j + 1] << 8 : 0) |
                         (j + 2 < n ? buf[j + 2] : 0);
      line[len++] = kB64[v >> 18 & 63];
      line[len++] = kB64[v >> 12 & 63];
      line[len++] = j + 1 < n ? kB64[v >> 6 & 63] : '=';
      line[len++] = j + 2 < n ? kB64[v & 63] : '=';
    }
    line[len++] = '\n';
    out.write(reinterpret_cast<const uint8_t *>(line), len);
  }
  out.printf("#END %08lx\n", static_cast<unsigned long>(crc32(buf, n)));
  unlock();
}

}  // namespace screen
