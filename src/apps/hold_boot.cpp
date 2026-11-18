#include "apps/hold_boot.h"

#include <atomic>

#include "apps/music.h"
#include "features.h"
#include "ui/nav.h"

#if INKDECK_VOICE
#include "apps/voice.h"
#include "audio.h"
#include "input.h"
#include "voice_client.h"
#endif

namespace apps {
namespace {

#if INKDECK_VOICE
// What the last hold's edge chose: talk, or (music or a podcast playing, or talking not allowed)
// not. key() follows it, so the two can't disagree if playback ends in between.
std::atomic<bool> holdTalks{false};

// HOLD BOOT's two ends, from the input timer: straight to the voice task, so the mic opens on
// time even while the panel is refreshing. Playback comes first: while it plays, HOLD BOOT is the
// player's. The release always goes (a stray one does nothing).
void edge(bool down) {
  if (!down) {
    voice::talkStop();
    return;
  }
  const bool talks = ui::nav::talkAllowed() && !audio::playing();
  holdTalks = talks;
  if (talks) voice::talkStart();  // with no session, opens one
}
#endif

// HOLD BOOT in the UI task, after edge(): the voice sheet if the hold talks (unless the Voice
// screen or the sheet is on top already), otherwise the player sheet.
void key(bool held) {
  if (!held) return;  // the release went to the voice task already; the navigator redraws
#if INKDECK_VOICE
  if (holdTalks) {
    if (voiceOnTop()) return;
    if (!ui::nav::talkAllowed()) {
      // The stack changed between the hold and now (the lock came up): undo what the hold began.
      voice::close();
      return;
    }
    raiseVoiceSheet();
    return;
  }
#endif
  openPlayer();  // does nothing if nothing plays
}

}  // namespace

void beginHoldBoot() {
#if INKDECK_VOICE
  voice::begin();
  input::onBackHold(edge);
#endif
  ui::nav::onTalkKey(key);
}

}  // namespace apps
