// Updater (firmware addition, opened from System → UPDATE OVER WI-FI): listens for a firmware
// image pushed from the computer over Wi-Fi (src/ota.h) and shows how it's going: the state and
// the time left in the listening window, a progress gauge while an image arrives, the address
// to send it to, the command to run on the Mac, and the firmware running now.
//   ● listen again (once stopped or timed out)   BOOT cancel and back (not while receiving)
// Opening it starts listening; leaving it stops. A good image restarts the board into it, even
// if the screen was left meanwhile.
//
// Named updater because apps/ota.h would shadow src/ota.h for files in apps/ (AGENTS.md
// gotcha 8), and an update.h would shadow Arduino's <Update.h> on a case-insensitive disk.
#pragma once
#include <memory>

#include "ui/nav.h"

namespace apps {

std::unique_ptr<ui::Screen> makeUpdater();

}  // namespace apps
