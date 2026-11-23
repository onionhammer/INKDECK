// COMMS (issue #66): the screens over the engine in comms.h. Home → COMMS (05). Only in a build
// with INKDECK_COMMS (features.h). Named comms_ui.h, not comms.h, so it can't shadow the engine's
// header for the files in apps/ (AGENTS.md, gotcha 8).
//
// The talk screen: one group at a time. A band of the groups (the one on screen in a pill, a
// boxed count on a group with calls, missed rings or messages not heard, its via on the right),
// then CHANNEL (the state in large type: QUIET, MIC ON, HEARING, OFF, or CONVERSATION while the
// group has one, a line or two under it, the 20-segment meter, a small map of the boards
// reached), TALK TO (ALL OF the group, then its boards: how each is reached) and HEARD (the
// group's calls, rings and messages, newest first).
//   ▲▼ move in TALK TO   ● ring the row selected (JOIN the group's conversation, or LEAVE the one
//   this board is in)   HOLD ● menu   HOLD BOOT talk to the row selected (to the conversation
//   this board is in)   BOOT back (STOP while PLAY LAST or a message plays)
// It holds the radio while it exists (comms::hold()), so calls come in on battery too, and the
// group's calls count as heard while it shows them.
//
// The menu (HOLD ●): PLAY LAST, MESSAGES, RELAY FOR OTHERS, CALL CHIRP, GROUPS. The Messages sheet
// (MESSAGES): the group's messages, ● plays one. The Groups sheet (GROUPS): a row a group, ✓ on
// the one on screen;
//   ▲▼ move   ● switch to it   HOLD ● its via (AIR+NET → AIR → NET)   BOOT close
//
// The ringing sheet (comms_ringing.cpp): ● RING raises it over the talk screen while the ring
// goes: RINGING with the seconds and how many boards ring; from 5 s HOLD BOOT leaves a message
// instead; NO ANSWER at 60 s (● rings again); BOOT gives up. It closes when someone answers.
//
// The sheets that rise over the screen in use (comms_sheet.cpp; apps::pollComms() raises them):
// - the call sheet, when a call starts playing: who calls, the meter, the calls before. HOLD BOOT
//   replies (to all of the group, or to the caller for a call to this board); during a
//   conversation it says so and lists who is in it, and BOOT leaves the conversation. It closes
//   10 s after the last word, or on BOOT;
// - the ring sheet, when a ring starts here: who rings, ● ANSWER (then it's the call sheet), HOLD
//   BOOT answers and talks at once, BOOT IGNORE; a ring that ends unanswered shows MISSED, or LEFT A
//   MESSAGE with ● PLAY, for a few seconds;
// - the conversation sheet, once per conversation this board isn't in: who is in it, ● JOIN, BOOT
//   LATER; it closes by itself 10 s after it rose or the last key.
// Not over the lock, an off face or the desk face, the Recorder, a ringing alarm, the agent sheet
// while it listens or replies. The call and conversation sheets don't rise over the Comms screen,
// which shows them in place (the call plays anyway, and stays not heard); the ring sheet does.
//
// HOLD BOOT on these screens switches this board's mic on while held, for now (the engine's open
// mic replaced push-to-talk; a later slice gives the mic its own switch), not the agent: the
// screens' talkTo() says comms, and apps/hold_boot.cpp calls commsTalk() (comms::micOn()) from the
// input timer and commsTalkStop() (comms::micOff()) at the release; on the ringing sheet it
// records a message, on the ring sheet it answers first.
//
// The status bar's mark: an inverted speech bubble with a count while a conversation is on in one
// of this board's groups and this board isn't in it (the count: the boards in it).
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

// From loop(): raises the ring sheet when a ring starts here, the call sheet when a call starts
// playing, and the conversation sheet once per conversation this board isn't in.
void pollComms();

// HOLD BOOT's two ends on a screen whose talkTo() says comms (apps/hold_boot.cpp, from the input
// timer): the call starts (or the message, or the answer and the call), and the screens show
// status().error as a notice if it doesn't. The release ends what the hold began.
void commsTalk(const ui::TalkTo &to);
void commsTalkStop();

namespace commsui {

// The reason the last talk (or PLAY LAST, ring, join) from these screens didn't happen, for a "!"
// notice: for a few seconds after the attempt, while the board isn't talking. "" otherwise.
std::string attemptError();
void noteAttempt();  // PLAY LAST from the menu, a ring, a join

// PLAY LAST or a message is playing (status().replay): BOOT stops it first.
bool playing();
// Stops it, if one plays; true if it did (BOOT's first use on the Comms screens and sheets).
bool stopIfPlaying();

// "QUIET", "MIC ON", "HEARING", "OFF".
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
// A note for the log: "VIA KITCHEN", "2 SENT" (this board's mic session: its sounds), "LOST",
// "NOT PLAYED", "LIVE"; for a ring "RINGING",
// "ANSWERED", "RANG", "MISSED", "NO ANSWER"; for a message "MSG COMING", "NEW MSG", "MSG",
// "MSG PART", "RECORDING", "MSG SENT", "MSG · 1 GOT IT"; "" for none.
std::string noteText(const comms::Call &c);
// The note asks for attention (a missed ring, a message not played): drawn inverted.
bool noteNeedsYou(const comms::Call &c);

// "TODAY · 6" when every call listed is from today, else "6 CALLS"; "" for none.
std::string countText(const std::vector<comms::Call> &calls);
// "1 BOARD", "2 BOARDS".
std::string boardsText(int n);

// Who is in a conversation: "YOU + KITCHEN" (this board in it), "KITCHEN + GARAGE".
std::string membersText(const comms::Conversation &c);
// How long it has been on, in whole minutes so it needn't redraw every second: "JUST BEGUN",
// "FOR 1 MIN", "FOR 1 H 05".
std::string forText(uint32_t ms);
// The group's name, "" for none.
std::string groupName(int group);

// A 30 px log row: the time, who to whom (and the note after a dot, without a note column), the
// length on the right; a 1 px dashed rule under it.
void drawCallRow(ui::Canvas &c, const ui::Rect &r, const comms::Call &call, bool noteColumn);

// The sheets' 52 px state row: a word in 22 px ExtraBold on the left, then the 20-segment meter
// (filled >= 0) or a text on the right in the same size (`right`), a dashed rule under it.
void drawStateRow(ui::Canvas &c, const ui::Rect &row, const std::string &word, int filled,
                  const std::string &right = "");
// The sheets' big block: a small label, a name in 38 px, then up to two lines; returns the y
// under it.
int drawNameBlock(ui::Canvas &c, const ui::Rect &r, const std::string &label,
                  const std::string &name, const std::string &line,
                  const std::string &line2 = "");
// // EARLIER with the count, then the calls in log rows as many as fit over a 25 px foot with
// `foot` in it (all of r).
void drawEarlier(ui::Canvas &c, ui::Rect r, const std::vector<comms::Call> &calls,
                 const std::string &foot);

// The sheets, for each other: the call sheet for a conversation this board is in now (answered
// or joined), with `peer` the board it's with (the dialer, or 0); the dialer's ringing sheet;
// the Messages sheet for a group.
std::unique_ptr<ui::Screen> makeConversationCallSheet(int group, uint32_t peer, bool toAll);
std::unique_ptr<ui::Screen> makeRingingSheet(int group, uint32_t target);
std::unique_ptr<ui::Screen> makeMessagesSheet(int group);

}  // namespace commsui
}  // namespace apps

#endif  // INKDECK_COMMS
