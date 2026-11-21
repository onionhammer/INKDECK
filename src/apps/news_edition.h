// News's off face (the "News Off Face" design canvas), the way Sleep shows its frog and a closed
// book its cover: the stories of the last 12 hours, one at a time, with the time and the weather.
//
// Portrait, no chrome. The INKDECK/OS strip (NEWS, the date, the last sync); one short dark
// block, the time in 80 px beside the forecast for now (its pixel icon, the temperature, the
// sky, today's high and low, the next rain); the story in ink on paper (feed, time, NEW,
// reading time, the headline in 28 px capitals, then its summary; one with no summary gets its
// headline in 38 px); and a foot with a dot-matrix frog reading the paper, where the cycle is
// (a cell a story), when the next fetch comes and HOLD [PWR] TO READ.
//
// PWR on a News screen on battery (and the idle power-off from one) draws it and powers off as
// power::Mode::Edition. The RTC wakes the board every minute, as standby does: the clock
// redraws (a partial refresh, from what was drawn before: no card, no radio), and every
// news_face_every minutes (settings.h; 5 by default) the next story comes up instead. When the
// cycle has shown every story, the next wake fetches the feeds and the forecast (90 s at most;
// PWR boots into News), unless the last fetch is under an hour old: then the cycle starts again,
// and the fetch waits for the end of a lap after the hour. With nothing in the last 12 hours it
// says NOTHING NEW and fetches hourly. Below 15 % battery: one last draw, STOPPED AT over the
// time, and no wakes but alarms. HOLD PWR from off opens News on the story on the face.
//
// On USB the face stays up instead, LIVE: the clock each minute, the stories in turn, and News's
// hourly sync (pollNews()); any key wakes it.
#pragma once
#include <stdint.h>
#include <time.h>

namespace apps {

// PWR's choice (apps::powerKey(), pollPower()): a News screen is the base.
bool editionWanted();
// Shows the face: on battery it powers off behind it, on USB it stays live. devWake (dev
// tools): on battery's path wherever it is; after three minute wakes the board boots into News
// on the story, as HOLD PWR does.
void showEdition(bool usb, bool devWake = false);
// PWR while the face is up (before its power-off, or on USB): back to what was under it.
bool wakeEdition();
bool editionShowing();

// The face's next minute wake (0 for none), for an agent check-in from behind it to power off
// with.
time_t editionWakeAt();

// The story on the face (0 for none): HOLD PWR from off opens News on it.
uint32_t faceStory();

// From setup(), right after agentWake(): an RTC wake from behind the face. Redraws it and powers
// off again. True when PWR was pressed: boot into News.
bool editionWake();

// Dev tools (any task): the face's battery path now, for three wakes.
void requestEdition();
void pollEdition();  // from pollPower(): acts on requestEdition()

}  // namespace apps
