// The agent sheet: talk to inkharness on the home network (agent_client.h; the daemon is
// inkharness/). Only in a build with INKDECK_AGENT (features.h). The sessions are the Agent's
// screens (apps/agent.h, issue #52); talking happens here, on every screen.
//
// From any screen, HOLD BOOT (about half a second; apps/hold_boot.h) talks: a push-to-talk chirp (three quick pips), and the mic opens
// (at once, even while the panel is refreshing; the chirp is the signal to speak, and the mic
// doesn't hear it). The agent sheet rises over the lower half of the screen with the
// session's state, the mic's level, what the daemon heard and the reply's text as it is spoken.
// Let go to send. On the sheet:
//   HOLD BOOT  talk again; during a reply it stops the reply and listens
//   ▲▼         volume; once a reply too long for the sheet has played, they scroll it instead
//              ("1-6 OF 14" beside REPLY), a page less a line at a time
//   BOOT       close (ends the session and lets Wi-Fi go)
// It closes by itself 30 s after the last reply, or once a reply has played that opens a screen
// (a book, a module: apps/board_tools.h). Over the Agent's screens the words go to the agent (a
// new session, or the one on screen) and the sheet says AGENT; anywhere else they are a talk turn,
// the limited assistant that sees no sessions (issue #52, "Security: talk and agent"). Not over the lock, an off face, the Recorder or
// a ringing alarm (ui::nav::talkAllowed()). While music or a podcast plays, HOLD BOOT raises the
// player sheet instead (apps::openPlayer()): playback comes first. Talking stops a paused track,
// as the mic needs the codec.
#pragma once
#include <memory>

#include "ui/nav.h"

namespace apps {

// For HOLD BOOT (apps/hold_boot.cpp), which has already started the session when it asks:
bool agentSheetOnTop();       // the agent sheet is on top: the hold just talks
void raiseAgentSheet();  // the agent sheet over the screen on top

}  // namespace apps
