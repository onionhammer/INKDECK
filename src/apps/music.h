// Music (the "Music" artboard): now playing (title, artist, waveform overview with a playhead,
// time, play state, volume), the next tracks in the queue, and the signal chain.
//   ▲▼ volume   ● pause / play   HOLD ● skip   BOOT back (keeps playing)
// Playing an MP3 from Files queues the rest of its folder after it.
#pragma once
#include <memory>

#include "ui/nav.h"

namespace apps {

std::unique_ptr<ui::Screen> makeMusic();

// HOLD BOOT (ui::nav::onHoldBack): while music or a podcast is playing or paused, closes any
// menu and opens the player; false (go home) when nothing is playing or the player is showing.
bool openPlayer();

}  // namespace apps
