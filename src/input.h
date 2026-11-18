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
  Back,        // BOOT (GPIO0) released before the hold time
  BackLong,    // BOOT held about half a second (HOLD BOOT: talk, ui/nav.h)
  Power,       // PWR released within 2 s (longer is someone forcing the PMIC off)
  SelectDown,  // wheel press went down / came up: only while onSelectEdge() has a handler,
  SelectUp,    // and then instead of Select and SelectLong
  BackLongUp,  // BOOT let go after a BackLong
};

const char *name(Key key);

void begin();

// Blocks up to timeoutMs for the next key. Returns false on timeout.
bool wait(Key &key, uint32_t timeoutMs);

// True if the key wait() last returned is a fast repeat: ▲ or ▼ held past kFastAfterMs. Lists
// then step a page at a time (ui::ListCursor). False after a wait() that timed out.
bool lastKeyFast();

// True if the key wait() last returned is an auto-repeat of a held ▲ or ▼ rather than its press
// (fast repeats included). False after a wait() that timed out.
bool lastKeyRepeat();

// Queues a key as if it had been pressed (dev tools drive the UI over serial with this).
// fast: as a repeat from a dial held long enough to page (lastKeyRepeat() is true too).
// BackLong and BackLongUp go to the onBackHold() handler too, as a real hold does.
void inject(Key key, bool fast = false);

// Ends a wait() early, as a timeout would, from any task: work for the UI task has come in
// (apps/voice_tools.h).
void wake();

// Push-to-talk: while a handler is set, the wheel press reports its two edges instead of
// Select / SelectLong. The handler runs at once in the timer task, so a press made while the
// panel is refreshing isn't late (keep it short: post to a queue), and Key::SelectDown /
// Key::SelectUp follow through wait(). nullptr puts Select back.
void onSelectEdge(void (*handler)(bool down));

// HOLD BOOT's two ends, for push-to-talk from any screen: the handler runs in the timer task
// with true when BOOT has been held long enough to be a hold (as Key::BackLong is queued) and
// with false at its release (Key::BackLongUp), so the mic opens on time even while the panel
// refreshes. Keep it short. The release goes to the handler that had the hold.
void onBackHold(void (*handler)(bool down));

// Milliseconds since the last key (injected ones count).
uint32_t idleMs();

// A button (PWR included) is down right now, or its release hasn't been debounced yet.
bool anyDown();

// Keys queued and not yet taken by wait().
bool pending();

}  // namespace input
