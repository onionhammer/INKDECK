// The logical screen: a portrait 480x800 canvas over the panel's native 800x480 framebuffer,
// plus the refresh policy (partial by default, a full refresh every N updates for ghosting).
#pragma once
#include <Adafruit_GFX.h>
#include <Print.h>

namespace screen {

constexpr int kWidth = 480;
constexpr int kHeight = 800;

bool begin();
GFXcanvas1 &canvas();
void clear();  // fill white

// Pushes the canvas to the panel. Partial unless forceFull or the partial budget is spent.
void present(bool forceFull = false);

// Portrait (480x800, the default) or the panel's native landscape (800x480). Only changes how
// drawing maps onto the framebuffer; present() with a full refresh after switching.
void setLandscape(bool landscape);
bool landscape();

// Held while a frame is drawn and pushed, so dump() never sees half a frame.
void lock();
void unlock();

// Writes the framebuffer for tools/screenshot.py: "#FB <w> <h> <rotation>", numbered base64
// lines, then "#END <crc32>". Takes the lock.
void dump(Print &out);

}  // namespace screen
