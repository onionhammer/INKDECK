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
#include "agent_client.h"
#endif
#if INKDECK_COMMS
#include "apps/comms_ui.h"
#endif
#if INKDECK_AGENT || INKDECK_COMMS
#include "input.h"
#endif

namespace apps {
namespace {

#if INKDECK_AGENT || INKDECK_COMMS
// What the last hold's edge chose: the agent, a message to the boards (the dialer's ringing sheet
// says so in its talkTo()), or neither (music or a podcast playing, or talking not allowed).
// key() follows it, so the two can't disagree if playback ends in between.
enum class Hold : uint8_t { None, Agent, Comms };
std::atomic<Hold> holdTalks{Hold::None};

// HOLD BOOT's two ends, from the input timer: straight to the agent's task or comms', so the mic
// opens on time even while the panel is refreshing. Over the ringing sheet the message comes
// first; elsewhere playback does: while it plays, HOLD BOOT is the player's. The release always
// goes (a stray one does nothing).
void edge(bool down) {
  if (!down) {
#if INKDECK_AGENT
    agent::talkStop();
#endif
#if INKDECK_COMMS
    commsTalkStop();
#endif
    return;
  }
  const bool allowed = ui::nav::talkAllowed();
  const ui::TalkTo to = allowed ? ui::nav::talkTarget() : ui::TalkTo{};
  Hold hold = Hold::None;
#if INKDECK_COMMS
  if (allowed && to.comms) hold = Hold::Comms;
#endif
#if INKDECK_AGENT
  if (allowed && !to.comms && !audio::playing()) hold = Hold::Agent;
#endif
  holdTalks = hold;
#if INKDECK_COMMS
  if (hold == Hold::Comms) commsTalk(to);  // the message
#endif
#if INKDECK_AGENT
  if (hold == Hold::Agent) agent::talkStart();  // with no session, opens one
#endif
}
#endif

// HOLD BOOT in the UI task, after edge(): the agent sheet if the hold talks to the agent (unless
// it is on top already); nothing more for a message (the ringing sheet shows it); otherwise the
// player sheet.
void key(bool held) {
  if (!held) return;  // the release went to the agent or comms already; the navigator redraws
#if INKDECK_COMMS
  if (holdTalks == Hold::Comms) {
    // The stack changed between the hold and now (the lock came up): undo what the hold began.
    if (!ui::nav::talkAllowed()) commsTalkStop();
    return;
  }
#endif
#if INKDECK_AGENT
  if (holdTalks == Hold::Agent) {
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
#endif
#if INKDECK_AGENT || INKDECK_COMMS
  input::onBackHold(edge);
#endif
  ui::nav::onTalkKey(key);
}

}  // namespace apps
