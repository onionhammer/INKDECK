// Buttons -> debounced key events on a FreeRTOS queue.
//
// Sampled every 5 ms from an esp_timer, so presses made while the e-paper is refreshing are
// queued rather than lost.
#pragma once
#include <stdint.h>

namespace input {

enum class Key : uint8_t {
  Up,          // rotary wheel up (GPIO4); auto-repeats while held
  Down,        // rotary wheel down (GPIO6); auto-repeats while held
  Select,      // wheel press (GPIO5), released before the long-press time
  SelectLong,  // wheel press held about 1 s
  Back,        // BOOT (GPIO0) short press
  BackLong,    // BOOT held
};

const char *name(Key key);

void begin();

// Blocks up to timeoutMs for the next key. Returns false on timeout.
bool wait(Key &key, uint32_t timeoutMs);

// Queues a key as if it had been pressed (dev tools drive the UI over serial with this).
void inject(Key key);

}  // namespace input
