// The lock (issue #8): a PIN entered like a combination padlock, one wheel per digit. Portrait,
// with chrome and the tag LOCKED. A pixel-art padlock (drawn like the Sleep frog), then
// "// PIN" with DIGIT n/N, and the wheels. The active wheel is inverted, with its neighbouring
// digits small above and below it. Entered wheels show a block, the ones still to come a dash.
//   ▲▼ digit (to the one drawn above / below)   ● next (UNLOCK on the last)   BOOT previous digit
// A wrong PIN starts again from the first wheel. The PIN and lock_minutes come from
// SD:/inkdeck.ini (settings.h), or System's PIN LOCK (makePinTask()).
//
// When it locks: every boot that reaches the UI, under each off screen (Sleep, Standby and the
// desk face, so waking the desk face or cancelling a power-off lands on it), and after
// lock_minutes without a key. While locked, alarms still ring over it (stopping and snoozing need
// no PIN), PWR works as usual, and HOLD BOOT stops at it (ui/nav.h).
#pragma once
#include <functional>
#include <memory>
#include <string>

#include "ui/nav.h"

namespace apps {

// Pushes the lock screen if a PIN is set and the stack isn't locked already. UI task only.
void lockNow();

// Call from loop(): locks after settings::lockMinutes() without a key. Not while an alarm rings
// (covering the ring sheet would silence it) or a memo records (covering the Recorder stops it).
void pollLock();

// System's PIN LOCK: the same wheels under System's tag. Both tasks ask for the current PIN
// first when one is set (CURRENT PIN). Set then takes a new one of 4 to 8 digits (NEW PIN:
// eight wheels, HOLD ● when done), and the same again (NEW PIN AGAIN) before it saves it. Clear
// removes the PIN once the current one is right. BOOT on the first wheel cancels. `done` gets a
// line for System's notice ("PIN SET") when the task completes.
enum class PinTask : uint8_t { Set, Clear };
std::unique_ptr<ui::Screen> makePinTask(PinTask task,
                                        std::function<void(const std::string &)> done);

}  // namespace apps
