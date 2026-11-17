// The closed book: the last frame before the power-off that ends a reading session nobody is at.
// Portrait, no chrome: the book's cover across the whole panel (covers::full(); a book without
// one gets its title in type), over an inverted band, "HOLD [PWR] 1 s TO RESUME" and how far
// through the book it is.
//
// On battery, 15 minutes without a key under the Reader close the book (power::idleBook(),
// apps/idle.h); so does a low battery after 10, and PWR at once (apps::powerKey()). The board
// powers off as power::Mode::Book, wakes
// for alarms as Sleep does, and the next start opens the book at its page (main.cpp). A key in
// the second before the power-off, PWR included, goes back to the page.
#pragma once

namespace apps {

// Saves the open book's place, shows its cover, then powers off. Does nothing if a power-off
// is already on its way. devWake (dev tools): the RTC boots the board again a minute later.
void closeBook(bool devWake = false);

// PWR while the cover is still up: back to the page. False if it isn't showing.
bool reopenBook();

}  // namespace apps
