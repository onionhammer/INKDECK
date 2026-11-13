// Sleep (the "Sleep" artboard): the last frame before a power-off, in landscape. Left, a
// dot-matrix field with a pixel-art frog asleep on a lily pad, the halt time above it and what
// to resume below. Right, an inverted panel with the wake plan: the time the RTC will boot the
// board (the next alarm or snooze), why, the wake path, battery and alarm, "hold PWR 1 s".
// A key in the second before the power-off cancels it.
#pragma once
#include <memory>

#include "ui/nav.h"

namespace apps {

// Shows the Sleep screen, then powers off (power::off(Sleep)). System's SLEEP NOW and a PWR
// press call this. Does nothing if a power-off is already on its way.
void sleepNow();

}  // namespace apps
