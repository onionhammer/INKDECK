// The logical screen: a portrait 480x800 canvas over the panel's native 800x480 framebuffer,
// plus the refresh policy: partial by default, and a full refresh once partial updates have
// flipped about two screens' worth of pixels (or after 30 of them), against ghosting.
#pragma once
#include <Adafruit_GFX.h>
#include <Print.h>

namespace screen {

constexpr int kWidth = 480;
constexpr int kHeight = 800;

bool begin();
GFXcanvas1 &canvas();
void clear();  // fill white

// Pushes the canvas to the panel. Partial unless forceFull or the ghosting budget is spent.
// A partial update with no changed pixels is skipped; returns false then.
bool present(bool forceFull = false);

// Tells the panel driver the canvas holds what the panel shows now (it kept its image through a
// power-off), so the next present() can be a partial refresh against it.
void assumeShown();

// Light sleep instead of polling while the panel refreshes (Epd::setSleepWhileBusy). For the
// standby wake on battery; it drops a USB serial connection.
void sleepWhileBusy(bool on);

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
