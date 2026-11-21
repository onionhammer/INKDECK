// The morning edition (the "Edition" artboard on the News design canvas): News's off face, the
// way a closed book shows its cover.
//
// PWR on a News screen (on battery; and the idle power-off from one) draws a front page of the
// feeds, then powers off as power::Mode::Edition. No chrome: a masthead (NEWS, the date, the
// edition's number and time, how many new stories, and the weather when the forecast reaches
// today), the lead story (its feed and time, the headline in 28 px capitals, three lines of its
// summary in Literata), four briefs in a 2 x 2 grid, from different feeds where it can, three
// more headlines under // ALSO with how many more there are, and a band: HOLD [PWR] TO RESUME,
// then NEXT EDITION SUN 06:30. New stories come first, newest first; then the rest.
//
// With news_edition = 06:30 in inkdeck.ini (settings::newsEdition()), the RTC wakes the board at
// that time each day: it syncs the feeds (90 s at most; PWR boots into News instead), draws the
// next edition with a full refresh and powers off again. Below 15 % battery it skips the sync,
// says EDITIONS PAUSED · BATTERY LOW on the band and stops waking. Alarms still boot the board,
// and an agent check-in (apps/agent_face.h) still comes, then the edition carries on. HOLD PWR
// from off opens News.
//
// On USB the face stays up instead, LIVE on its band: it redraws as News syncs (hourly), and any
// key wakes it.
#pragma once
#include <time.h>

namespace apps {

// PWR's choice (apps::powerKey(), pollPower()): a News screen is the base.
bool editionWanted();
// Shows the front page: on battery it powers off behind it, on USB it stays live. devWake (dev
// tools): on battery's path wherever it is, with the RTC a minute later.
void showEdition(bool usb, bool devWake = false);
// PWR while the face is up (before its power-off, or on USB): back to what was under it.
bool wakeEdition();
bool editionShowing();

// The next edition's wake as kept for it (0 for none), for an agent check-in from behind the
// face to power off with.
time_t editionWakeAt();

// From setup(), right after agentWake(): an RTC wake from behind the edition. Draws the next
// one and powers off again. True when PWR was pressed during the sync: boot into News.
bool editionWake();

// Dev tools (any task): the face's battery path now, with a wake a minute later.
void requestEdition();
void pollEdition();  // from pollPower(): acts on requestEdition()

}  // namespace apps
