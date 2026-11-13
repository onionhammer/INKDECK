// Buttons -> debounced key events on a FreeRTOS queue.
//
// Sampled every 5 ms from an esp_timer, so presses made while the e-paper is refreshing are
// queued rather than lost. PWR goes to the PMIC; GPIO1 (PWR_KEY_SENSE) reads HIGH while it's
// held, so it can be a key too. A button already down at begin() (PWR held to power on)
// reports nothing until it has been released.
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
  Power,       // PWR released within 2 s (longer is someone forcing the PMIC off)
};

const char *name(Key key);

void begin();

// Blocks up to timeoutMs for the next key. Returns false on timeout.
bool wait(Key &key, uint32_t timeoutMs);

// Queues a key as if it had been pressed (dev tools drive the UI over serial with this).
void inject(Key key);

// Milliseconds since the last key (injected ones count).
uint32_t idleMs();

// A button (PWR included) is down right now.
bool anyDown();

}  // namespace input
