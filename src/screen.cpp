#include "screen.h"

#include <Arduino.h>

#include "epd.h"
#include "typeset.h"

namespace screen {
namespace {

// Rotation 3 = the vendor's ROTATE_270 portrait mapping (memX = y, memY = 479 - x).
// Change to 1 if the enclosure ends up holding the board the other way up.
constexpr uint8_t kRotation = 3;
constexpr int kPartialsBeforeFull = 10;

Epd epd;
GFXcanvas1 *fb = nullptr;
int partials = 0;

}  // namespace

bool begin() {
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

void present(bool forceFull) {
  const bool full = forceFull || partials >= kPartialsBeforeFull;
  if (epd.show(fb->getBuffer(), full)) {
    partials = full ? 0 : partials + 1;
  }
}

}  // namespace screen
