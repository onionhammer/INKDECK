// Updater (firmware addition, opened from System → CHECK FOR UPDATE): asks GitHub for the latest
// release (src/ota.h) and installs it. A // STATE panel (the phase, a headline such as the new
// version or UP TO DATE, a detail line, a progress gauge while it downloads), then the release
// found (version and commit, date, size, where from) and the firmware running now.
//   new release: ● install                older release (a build from the computer): HOLD ● install
//   up to date, stopped or failed: ● check again
//   BOOT back; while joining, checking or downloading, BOOT cancels (the running firmware stays)
// Opening it starts a check; leaving it cancels whatever runs. A good image restarts the board
// into it.
//
// Named updater because apps/ota.h would shadow src/ota.h for files in apps/ (AGENTS.md
// gotcha 8), and an update.h would shadow Arduino's <Update.h> on a case-insensitive disk.
#pragma once
#include <memory>

#include "ui/nav.h"

namespace apps {

std::unique_ptr<ui::Screen> makeUpdater();

}  // namespace apps
