// Clock (the "Clock" artboard), the first landscape screen: the time in 172 px, weekday and
// date, ISO week and day of the year, a 24-cell day bar with sunrise and sunset, the month
// calendar and the three alarms. Not on the canvas: a weather strip under the date (the
// forecast for now with today's high and low, and the room from the SHTC3).
//   ▲▼ alarm   ● on/off   HOLD ● edit   BOOT back
// The editor is a panel over the right column:
//   ▲▼ set   ● next field   HOLD ● save   BOOT cancel
//
// pollAlarms() rings them on any screen: a sheet over whatever is showing, with the tone from
// audio::ring(). ● or BOOT stops it, HOLD ● snoozes 9 minutes; it goes quiet by itself after
// 2 minutes and the sheet stays until a key.
#pragma once
#include <memory>

#include "ui/nav.h"

namespace apps {

std::unique_ptr<ui::Screen> makeClock();

// Call from loop(), about once a second: pushes the ring sheet when an alarm is due.
void pollAlarms();

// The desk face, PWR's "off" screen while on USB: the Clock face with the weather strip, kept
// current with the board left on. Any key wakes it; unplugging USB turns it into Sleep.
void enterDesk();
// Closes the desk face (and a ring sheet over it); false if it isn't showing.
bool wakeDesk();

// Standby (power.h): the Clock face stays on the panel while the board is off, and the RTC
// wakes it every minute to redraw it. Shows the face, then powers off (Sleep instead if the
// clock isn't set). cycles > 0 (dev tool): stay on after that many wakes. UI task only.
void enterStandby(int cycles = 0);
// Early in setup(): if this start is a standby wake with no alarm due, redraws the face and
// powers off again (no return). Returns false when the board should boot normally.
bool standbyWake();

}  // namespace apps
