#include "screen.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "epd.h"
#include "typeset.h"

namespace screen {
namespace {

// Rotation 3 = the vendor's ROTATE_270 portrait mapping (memX = y, memY = 479 - x).
// Change to 1 if the enclosure ends up holding the board the other way up.
constexpr uint8_t kRotation = 3;
// Ghosting builds up with the pixels that partial refreshes flip, so the full refresh comes
// when the flips since the last one add up to about two whole screens (roughly ten screen
// changes), or after kMaxPartials updates however small, whichever is first. Small live
// updates (a reading, a playhead) flip a few thousand pixels, so they rarely force a flash.
constexpr uint32_t kFlipBudget = 2u * Epd::kWidth * Epd::kHeight;
constexpr int kMaxPartials = 30;

Epd epd;
GFXcanvas1 *fb = nullptr;
int partials = 0;
uint32_t flips = 0;  // pixels flipped by partial refreshes since the last full one
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
  return epd.begin();
}

GFXcanvas1 &canvas() { return *fb; }

void clear() { fb->fillScreen(gfx::kWhite); }

bool present(bool forceFull) {
  const uint32_t changed = epd.changedPixels(fb->getBuffer());
  if (!forceFull && changed == 0) return false;  // nothing visible changed: skip the refresh
  const bool full = forceFull || partials >= kMaxPartials || flips + changed > kFlipBudget;
  if (epd.show(fb->getBuffer(), full)) {
    partials = full ? 0 : partials + 1;
    flips = full ? 0 : flips + changed;
  }
  log_i("screen: %lu px changed, flip budget %lu%% used", (unsigned long)changed,
        (unsigned long)(flips * 100ull / kFlipBudget));
  return true;
}

void setLandscape(bool landscape) { fb->setRotation(landscape ? 0 : kRotation); }

bool landscape() { return fb->getRotation() == 0; }

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
