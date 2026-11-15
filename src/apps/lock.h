// The lock (issue #8): a PIN entered like a combination padlock, one wheel per digit. Portrait,
// with chrome and the tag LOCKED. A pixel-art padlock (drawn like the Sleep frog), then
// "// PIN" with DIGIT n/N, and the wheels. The active wheel is inverted, with its neighbouring
// digits small above and below it. Entered wheels show a block, the ones still to come a dash.
//   ▲▼ digit (to the one drawn above / below)   ● next (UNLOCK on the last)   BOOT previous digit
// A wrong PIN starts again from the first wheel. After three wrong PINs in a row (on any of
// these screens), each further try waits: 30 s, then twice as long each time, up to 30 min. The
// count is kept in NVS, so a restart doesn't clear it (the wait starts again from boot); the
// right PIN does. The PIN and lock_minutes come from SD:/inkdeck.ini (settings.h), or System's
// PIN LOCK (makePinTask()).
//
// Where the PIN is asked is chosen at build time (INKDECK_PIN_ON, pin_places.h): power_on,
// settings, files, or any mix of them.
//
// power_on, the lock: every boot that reaches the UI, under each off screen (Sleep, Standby and
// the desk face, so waking the desk face or cancelling a power-off lands on it), and after
// lock_minutes without a key. While locked, alarms still ring over it (stopping and snoozing need
// no PIN), PWR works as usual, and HOLD BOOT stops at it (ui/nav.h).
//
// settings and files: the same wheels in front of System or Files as Home opens them
// (openGuarded()), unless that module was on screen in the last minute. Under the module's tag
// with ● OPEN on the last wheel. That isn't a lock: BOOT on the first wheel and HOLD BOOT go back
// as usual. Opening either with the PIN applies a PIN change waiting on the card, as unlocking
// does, and so does unlocking the lock for both.
//
// The minute also holds while the module is open: when one of its screens comes back on top
// (a book or picture closed, the desk face woken, a sheet gone) more than a minute after it was
// last in use, or sits on top a minute without a key, the wheels come up in front of it. The
// right PIN goes back to it; BOOT on the first wheel goes home instead.
#pragma once
#include <functional>
#include <memory>
#include <string>

#include "ui/nav.h"

namespace apps {

// Pushes the lock screen if a PIN is set, the build asks for it at power_on, and the stack isn't
// locked already. UI task only.
void lockNow();

// Call from loop(): locks after settings::lockMinutes() without a key. Not while an alarm rings
// (covering the ring sheet would silence it) or a memo records (covering the Recorder stops it).
// Also puts the wheels in front of a guarded module's screen that has been on top a minute
// without a key.
void pollLock();

// System's PIN LOCK: the same wheels under System's tag. Both tasks ask for the current PIN
// first when one is set (CURRENT PIN). Set then takes a new one of 4 to 8 digits (NEW PIN:
// eight wheels, HOLD ● when done), and the same again (NEW PIN AGAIN) before it saves it. Clear
// removes the PIN once the current one is right. BOOT on the first wheel cancels. `done` gets a
// line for System's notice ("PIN SET") when the task completes.
enum class PinTask : uint8_t { Set, Clear };
std::unique_ptr<ui::Screen> makePinTask(PinTask task,
                                        std::function<void(const std::string &)> done);

// The modules the PIN can guard (INKDECK_PIN_ON's settings and files).
enum class Guarded : uint8_t { Settings, Files };

// Home opens them through this. Pushes open()'s screen (or puts it in place of the top screen,
// with replace), behind the PIN wheels when the build asks for the PIN there, a PIN is set, and
// the module hasn't been in use in the last minute (since boot counts as not). The right PIN
// replaces the wheels with open()'s screen.
void openGuarded(Guarded module, std::function<std::unique_ptr<ui::Screen>()> open,
                 bool replace = false);

// The module's screens call these: guardedOnTop() from onEnter() (it may push the wheels in front
// of the screen, see above), guardedInUse() from onKey(), guardedLeft() from onLeave(). A screen
// pushed straight from another of the module's screens counts as the same use. (The Reader's and
// Music's folder browsers don't call them: they stay in their folder, apps/files.h.)
void guardedOnTop(Guarded module);
void guardedInUse(Guarded module);
void guardedLeft(Guarded module);

}  // namespace apps
