// Podcasts (the "Podcasts" artboard): the shows, the selected show's episodes with their state
// ([NEW], [DL 62%], [▶ 23m], [✓]), a card for the selection, and how much of the card the
// podcasts use. The library is podcast_library.h.
//   Shows:     ▲▼ move   ● open   HOLD ● refresh the feeds   BOOT back
//   Episodes:  ▲▼ move   ● play (or GET one that isn't on the card)
//              HOLD ● mark played / unplayed (cancel a download)   BOOT back to the shows
// Playing opens the player; HOLD BOOT comes back to it from anywhere while it plays.
#pragma once
#include <memory>

#include "ui/nav.h"

namespace apps {

std::unique_ptr<ui::Screen> makePodcasts();

}  // namespace apps
