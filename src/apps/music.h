// Music (the "Music" artboard): now playing (title, artist, waveform overview with a playhead,
// time, play state, volume), the next tracks in the queue, and the signal chain.
//   ▲▼ volume   ● pause / play   HOLD ● skip   BOOT back (keeps playing)
// Playing an MP3 from Files queues the rest of its folder after it.
#pragma once
#include <memory>

#include "ui/nav.h"

namespace apps {

std::unique_ptr<ui::Screen> makeMusic();

// HOLD BOOT while music or a podcast plays (apps/hold_boot.h, before talking): closes any menu
// or sheet and raises the player sheet over the lower half of the screen: the title, the artist
// (or a podcast's show), the waveform with the playhead, the times, the play state and volume.
//   ●  pause / play   HOLD ●  next   ▲▼  volume   BOOT (or HOLD BOOT)  close
// On the full player it does nothing. Returns false, and HOLD BOOT talks, when nothing is
// playing (paused doesn't count), while locked, or under an off face; on the sheet it always
// closes it.
bool openPlayer();

}  // namespace apps
