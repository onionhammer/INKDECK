// The missed-ring mark on the off faces (issue #66): rings nobody answered here while silent
// (audio::silent()) and reachable (comms::reachInterval()) were both on, kept by comms
// (comms::missedRing()) until a key, PWR or USB makes the board an ordinary one. An inverted tag,
// `MISSED RING · INKDECK-0000 RANG 18:02` (`3 MISSED RINGS · ...` for more), shortened to fit
// the room each face gives it: Sleep over the top of its dot field, the Clock face in its hint
// bar, a closed book over the top of its cover, the Agent face in a strip over its band, News's
// face in its foot. Faces redrawn in setup() (the RTC wakes) draw it in both the old frame and the
// new, so the partial refresh's base matches the panel.
#pragma once
#include <stdint.h>

#include "ui/draw.h"

namespace apps {

// There is a missed ring to show (false in a build without comms).
bool missedRingShown();
// Draws the mark h px tall and at most maxW wide, vertically centred on centerY, its left edge at
// x (its right edge with alignRight), in `color` (kPaper on ink). rim: a 4 px paper rim around it,
// over a picture. Returns its width; 0 when there's none to show.
int drawMissedRing(ui::Canvas &c, int x, int centerY, int h, int maxW, bool alignRight = false,
                   uint16_t color = ui::kInk, bool rim = false);

}  // namespace apps
