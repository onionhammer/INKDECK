// COMMS (issue #66): the screens over the engine in comms.h. Home → COMMS (05). Only in a build
// with INKDECK_COMMS (features.h). Named comms_ui.h, not comms.h, so it can't shadow the engine's
// header for the files in apps/ (AGENTS.md, gotcha 8).
//
// The talk screen: one group at a time. A band of the groups (the one on screen in a pill, a
// boxed count on a group with calls not heard, its via on the right), then CHANNEL (the state in
// large type: QUIET, TALKING, HEARING or OFF, a line under it, the 20-segment meter, a small map
// of the boards reached), TALK TO (ALL OF the group, then its boards: how each is reached) and
// HEARD (the group's calls, newest first).
//   ▲▼ move in TALK TO   ● menu   HOLD ● groups   HOLD BOOT talk to the row selected   BOOT back
// It holds the radio while it exists (comms::hold()), so calls come in on battery too, and the
// group's calls count as heard while it shows them.
//
// The menu (●): PLAY LAST, RELAY FOR OTHERS, CALL CHIRP, GROUPS. The Groups sheet (HOLD ●, or
// GROUPS): a row a group, ✓ on the one on screen;
//   ▲▼ move   ● switch to it   HOLD ● its via (AIR+NET → AIR → NET)   BOOT close
//
// The call sheet (comms_sheet.cpp): when a call starts playing, it rises over the screen in use,
// like the agent sheet: who calls, the meter, the calls before. HOLD BOOT replies (to all of the
// group, or to the caller for a call to this board); it closes 10 s after the last word, or on
// BOOT. Not over the lock, an off face or the desk face, the Recorder, a ringing alarm, the agent
// sheet while it listens or replies, or the Comms screen, which shows the call in place. The call
// plays anyway (the engine sees to that), and stays not heard.
//
// HOLD BOOT on these screens is push-to-talk to the boards, not the agent: the screens' talkTo()
// says comms, and apps/hold_boot.cpp starts the call (commsTalk()) from the input timer.
#pragma once
#include "features.h"

#if INKDECK_COMMS

#include <stdint.h>

#include <memory>
#include <string>
#include <vector>

#include "comms.h"
#include "ui/components.h"
#include "ui/nav.h"

namespace apps {

std::unique_ptr<ui::Screen> makeComms();

// From loop(): raises the call sheet when a call starts playing (status().incoming changed).
void pollComms();

// HOLD BOOT's two ends on a screen whose talkTo() says comms (apps/hold_boot.cpp, from the input
// timer): the call starts, and the screens show status().error as a notice if it doesn't.
void commsTalk(int group, uint32_t board);
void commsTalkStop();

namespace commsui {

// The reason the last talk (or PLAY LAST) from these screens didn't happen, for a "!" notice:
// for a few seconds after the attempt, while the board isn't talking. "" otherwise.
std::string attemptError();
void noteAttempt();  // PLAY LAST from the menu

// "QUIET", "TALKING", "HEARING", "OFF".
const char *stateWord(comms::State s);
// Segments lit of 20 for a level in dBFS (-60 to 0, 3 dB each).
int meterFill(float levelDb);
// "0:06": a call's length.
std::string lengthText(uint32_t ms);
// When a call began: "14:08" today, "21 NOV" before, "5 MIN" ago while the clock isn't set.
std::string whenText(time_t at, uint32_t agoS);
// "KITCHEN", or a board not heard from yet by its id.
std::string boardName(const std::string &name, uint32_t id);
// "KITCHEN → ALL", "YOU → KITCHEN", "KITCHEN → YOU".
std::string whoText(const comms::Call &c);
// A note for the log: "VIA KITCHEN", "2 HEARD", "NOT PLAYED", "LIVE"; "" for none.
std::string noteText(const comms::Call &c);

// "TODAY · 6" when every call listed is from today, else "6 CALLS"; "" for none.
std::string countText(const std::vector<comms::Call> &calls);

// A 30 px log row: the time, who to whom (and the note after a dot, without a note column), the
// length on the right; a 1 px dashed rule under it.
void drawCallRow(ui::Canvas &c, const ui::Rect &r, const comms::Call &call, bool noteColumn);

}  // namespace commsui
}  // namespace apps

#endif  // INKDECK_COMMS
