#include "apps/hold_boot.h"

#include <atomic>

#include "apps/music.h"
#include "features.h"
#include "ui/nav.h"

#if INKDECK_AGENT
#include "apps/agent.h"
#include "apps/agent_sheet.h"
#include "apps/board_tools.h"
#include "audio.h"
#include "input.h"
#include "agent_client.h"
#endif

namespace apps {
namespace {

#if INKDECK_AGENT
// What the last hold's edge chose: talk, or (music or a podcast playing, or talking not allowed)
// not. key() follows it, so the two can't disagree if playback ends in between.
std::atomic<bool> holdTalks{false};

// HOLD BOOT's two ends, from the input timer: straight to the agent task, so the mic opens on
// time even while the panel is refreshing. Playback comes first: while it plays, HOLD BOOT is the
// player's. The release always goes (a stray one does nothing).
void edge(bool down) {
  if (!down) {
    agent::talkStop();
    return;
  }
  const bool talks = ui::nav::talkAllowed() && !audio::playing();
  holdTalks = talks;
  if (talks) agent::talkStart();  // with no session, opens one
}
#endif

// HOLD BOOT in the UI task, after edge(): the agent sheet if the hold talks (unless it is on
// top already), otherwise the player sheet.
void key(bool held) {
  if (!held) return;  // the release went to the agent task already; the navigator redraws
#if INKDECK_AGENT
  if (holdTalks) {
    if (agentSheetOnTop()) return;
    if (!ui::nav::talkAllowed()) {
      // The stack changed between the hold and now (the lock came up): undo what the hold began.
      agent::close();
      return;
    }
    raiseAgentSheet();
    return;
  }
#endif
  openPlayer();  // does nothing if nothing plays
}

}  // namespace

void beginHoldBoot() {
#if INKDECK_AGENT
  beginBoardTools();
  agent::begin();
  beginAgent();  // what a hold talks to: the agent on its screens, else a talk turn
  input::onBackHold(edge);
#endif
  ui::nav::onTalkKey(key);
}

}  // namespace apps
