// What the board does when nobody touches it (power.h has the mechanisms):
//   - On USB: nothing; it stays on.
//   - On battery, after 10 minutes without a key: standby (the Clock face, redrawn each
//     minute), or Sleep (the frog) once the battery is below 15 %. Reading (the Reader open)
//     and playing music never power off until the battery is below 15 %.
//   - Reading on battery: once the page is drawn the board light-sleeps until the next key or
//     the next minute (alarms ring on the minute), with the IMU in standby. A page turn wakes
//     it in milliseconds. Nothing redraws while it naps, so the status-bar clock updates with
//     the next page. Never on USB: light sleep drops the USB console, and it stays dead until
//     a reset, so USB power after naps with no host talking restarts the board cleanly.
#pragma once
#include <stdint.h>

namespace apps {

// Call from loop(): powers off when power::idle() says so (or when requested).
void pollPower();

// Call from loop() before the navigator: naps if the conditions above hold. Returns true if it
// napped (loop() should then start over).
bool napIfIdle();

// Light-sleep totals since boot, for the System screen.
struct NapStats {
  uint32_t naps = 0;
  uint32_t buttonWakes = 0;
  uint32_t asleepSeconds = 0;
};
NapStats napStats();

// Dev tools (any task): standby now; cycles > 0: stay on after that many wakes.
void requestStandby(int cycles);

}  // namespace apps
