// The logical screen: a portrait 480x800 canvas over the panel's native 800x480 framebuffer,
// plus the refresh policy (partial by default, a full refresh every N updates for ghosting).
#pragma once
#include <Adafruit_GFX.h>

namespace screen {

constexpr int kWidth = 480;
constexpr int kHeight = 800;

bool begin();
GFXcanvas1 &canvas();
void clear();  // fill white

// Pushes the canvas to the panel. Partial unless forceFull or the partial budget is spent.
void present(bool forceFull = false);

}  // namespace screen
