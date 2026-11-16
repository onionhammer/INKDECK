// Voice: hold the wheel and talk to the assistant on the home network (voice_client.h; the daemon is
// voiced/). The screen shows the session's state, the mic's level while listening, what the
// daemon heard and the reply's text as it is spoken.
//
// Keys:
//   hold ●   talk; release to send. During a reply it stops the reply and listens again
//   ▲▼       volume
//   BOOT     back (ends the session and lets Wi-Fi go)
// After a failed connection, ● tries again.
#pragma once
#include <memory>

#include "ui/nav.h"

namespace apps {

std::unique_ptr<ui::Screen> makeVoice();

}  // namespace apps
