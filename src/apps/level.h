// Level: a bubble level for surfaces and a spirit vial for the long edges, from the IMU
// (QMI8658A). Landscape, so the long side the vial measures runs along the bottom of the screen.
//
// Lying flat (gravity mostly through the screen) the bubble and pitch/roll are live; standing
// on a side, the vial and the edge angle are, and the live panel's header is inverted. The
// bubble and the vial rise to the high side, as real ones do.
//
// Keys: ▲▼ TONE (a short beep when it comes level), ● HOLD (freeze the reading), HOLD ● ZERO
// (take the current reading as level; HOLD ● again clears it), BOOT back. The zero is kept in
// NVS.
#pragma once
#include <memory>

#include "ui/nav.h"

namespace apps {

std::unique_ptr<ui::Screen> makeLevel();

}  // namespace apps
