// What the board does when nobody touches it (power.h has the mechanisms):
//   - On USB: nothing; it stays on. PWR shows the desk face (the Clock with the weather, kept
//     current) instead of powering off.
//   - On battery, after 10 minutes without a key: standby (the Clock face, redrawn each
//     minute), or Sleep (the frog) once the battery is below 15 %. Reading (the Reader open)
//     and playing music never power off until the battery is below 15 %, except a book:
//   - A book open in the Reader on battery, 15 minutes without a key (or 10 with the battery
//     below 15 %): the book closes. The panel shows its cover, the board powers off, and the
//     next start opens the book at its page (closed_book.h). Never on USB, and not while
//     music plays. The text reader and the picture viewer stay as above. PWR on battery
//     closes the book at once, music or not.
//   - Reading on battery: once the page is drawn the board light-sleeps until the next key or
//     the next minute (alarms ring on the minute), with the IMU in standby and the panel's
//     power (ALDO3) off. A paused player doesn't stop it (pause switches the audio off), and
//     the hourly weather sync waits for the next key (main.cpp). A page turn wakes it in
//     milliseconds. Nothing redraws while it naps, so the status-bar clock updates with
//     the next page. Under the picture viewer the IMU watches for motion instead, and turning
//     the board wakes it for a few seconds to see which way up it ended. Never on USB: light
//     sleep drops the USB console, and it stays dead until a reset, so USB power after naps
//     with no host talking restarts the board cleanly.
#pragma once
#include <stdint.h>

#include "power.h"

namespace apps {

// Call from loop(): powers off when power::idle() says so (or when requested). Up after a Comms
// wake (power.h, reachable on battery), steps that instead: the face holds, then powers off
// again.
void pollPower();

// A Comms wake (power.h): the face of the mode the board slept in comes back, holding while the
// board is up for the ring or the voice. Sleep, Standby, a closed book (the Reader under it, as
// main.cpp opens it), the Agent and News faces.
void reachFace(power::Mode mode);

// The PWR key (the navigator's handler): on USB the desk face; on battery Sleep, or with a book
// open in the Reader its cover (closed_book.h). On News its off face (news_edition.h),
// on the Agent its face (agent_face.h), live on USB. On a face, wake.
void powerKey();

// Call from loop() before the navigator: naps if the conditions above hold. Returns true if it
// napped (loop() should then start over).
bool napIfIdle();

// Between naps: from the first nap until a key, motion or anything else wakes the board for good.
// The minute's timer wakes in between don't end it.
bool isNapping();

// Light-sleep totals since boot, for the System screen.
struct NapStats {
  uint32_t naps = 0;
  uint32_t buttonWakes = 0;
  uint32_t asleepSeconds = 0;
};
NapStats napStats();

// Dev tools (any task): standby now; cycles > 0: stay on after that many wakes.
void requestStandby(int cycles);
// Dev tools (any task): close the book open in the Reader now, as 15 idle minutes on battery
// do; the RTC boots the board a minute later.
void requestCloseBook();

}  // namespace apps
