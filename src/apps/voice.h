// Voice: talk to the assistant on the home network (voice_client.h; the daemon is voiced/).
//
// From any screen, HOLD BOOT (about half a second) talks: the mic opens at once, even while
// the panel is refreshing, and the voice sheet rises over the lower half of the screen with the
// session's state, the mic's level, what the daemon heard and the reply's text as it is spoken.
// Let go to send. On the sheet:
//   HOLD BOOT  talk again; during a reply it stops the reply and listens
//   ▲▼         volume
//   BOOT       close (ends the session and lets Wi-Fi go)
// It closes by itself 30 s after the last reply. Not over the lock, an off face, the Recorder or
// a ringing alarm (ui::nav::talkAllowed()). While music or a podcast plays, HOLD BOOT raises the
// player sheet instead (apps::openPlayer()): playback comes first. Talking stops a paused track,
// as the mic needs the codec.
//
// The Voice screen (Home → VOICE) is the same session full-screen, also with the wheel to talk:
//   hold ●     talk; release to send. During a reply it stops the reply and listens again
//   ▲▼         volume
//   BOOT       back (ends the session and lets Wi-Fi go)
// After a failed connection, ● tries again.
#pragma once
#include <memory>

#include "ui/nav.h"

namespace apps {

std::unique_ptr<ui::Screen> makeVoice();

// From setup(): starts the voice client's task and puts talking on HOLD BOOT.
void beginTalkKey();

}  // namespace apps
