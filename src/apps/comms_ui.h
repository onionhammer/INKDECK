// COMMS (issue #66): the screens over the engine in comms.h. Home → COMMS (05). Only in a build
// with INKDECK_COMMS (features.h). Named comms_ui.h, not comms.h, so it can't shadow the engine's
// header for the files in apps/ (AGENTS.md, gotcha 8).
//
// The mic: push to talk, a walkie-talkie's. HOLD BOOT on the Comms screens and sheets keys this
// board's mic to the screen's target (on the talk screen the row selected in TALK TO: all of the
// group, or one board) while it's held, and letting go switches it off. ● while BOOT is held
// latches it: BOOT can go, and the mic stays on (how a room is heard) until the next BOOT or ●
// on these screens, which ends it and does nothing else (commsui's "mic's keys" below; the key
// filter is comms_sheet.cpp's). Elsewhere a latched mic stays on, and ● and BOOT do their own
// jobs. While it's on, nothing of comms's plays here (half duplex: comms.h); a board that talks
// meanwhile shows as KITCHEN TALKING TOO. The status bar shows an inverted MIC on every screen
// (outlined while something else has the mic for now: the agent listening), and so do the
// screens that draw their own (comms_sheet.cpp has the list).
//
// The talk screen: one group at a time. A band of the groups (the one on screen in a pill, a
// boxed count on a group with calls, missed rings, messages or LOSTs not seen, its via on the
// right), then CHANNEL (the state in large type: QUIET, MIC ON inverted, HEARING, LOST inverted,
// OFF, two lines under it, the 20-segment meter: this board's mic while it's on, else what
// plays; a small map of the boards reached), TALK TO (ALL OF the group, then its boards: how each
// is reached, or its mic: MIC ON <1 MIN · TO YOU) and HEARD (one row a mic session, rings and
// messages, newest first).
//   HOLD BOOT talk (to the row selected)   ● ring it (JOIN, LEAVE)   ▲▼ move   HOLD ● menu
//   BOOT back (STOP while PLAY LAST or a message plays)
//   while BOOT is held: ● HOLD (latches)   latched: BOOT STOP (● too)   HOLD ● menu
// It holds the radio while it exists (comms::hold()), so calls come in on battery too, and the
// group's calls count as heard while it shows them. It redraws only when what it shows changes:
// a meter at most once a second while voice goes or comes, nothing while it's quiet (lengths go
// in whole minutes then, all turning over with the status bar's clock).
//
// The menu (HOLD ●): KITCHEN'S MIC (when the row selected is a board with its mic on: the mic
// screen), PLAY LAST, MESSAGES (the Messages sheet: ● plays one), RELAY FOR OTHERS, CALL CHIRP,
// GROUPS (the Groups sheet: a row a group, ✓ on the one on screen;
//   ▲▼ move   ● switch to it   HOLD ● its via (AIR+NET → AIR → NET)   BOOT close).
//
// The mic screen (comms_mics.cpp): one board's open mic, the latched case (a room): ON or LOST and
// for how long, its level, two hours of it as a chart, its power and how it's reached, its LOSTs
// newest first.
//   ▲▼ volume   HOLD BOOT talk (to that board)   BOOT back
//
// The ringing sheet (comms_ringing.cpp): ● on the talk screen raises it over the talk screen while
// the ring goes: RINGING with the seconds and how many boards ring; from 5 s HOLD BOOT leaves a
// message instead (talkTo() says comms with message); NO ANSWER at 60 s (● rings again); BOOT
// gives up. Answered, it becomes the call sheet for the conversation, with HOLD BOOT TALK: the
// answer doesn't switch the dialer's mic on.
//
// The sheets that rise over the screen in use (comms_sheet.cpp; apps::pollComms() raises them):
// - the call sheet, once per conversation whose voice first plays here (not at every press in
//   it): whose mic, the meter, the calls before. HOLD BOOT talks to the conversation while held
//   (● latches), ▲▼ volume, HOLD ● leaves the conversation, BOOT closes. It closes 10 s after the
//   last word (heard, talked over or sent) and as long since a key, leaving this board in the
//   conversation and its mic as it is;
// - the ring sheet, when a ring starts here: who rings, ● ANSWER (this board's mic stays off),
//   HOLD BOOT answers and talks while held (then it's the call sheet), BOOT IGNORE; a ring that
//   ends unanswered shows MISSED, or LEFT A MESSAGE with ● PLAY, for a few seconds;
// - the conversation sheet, once per conversation this board isn't in, 5 s after it was seen
//   (boards that hear a mic open join by themselves, so it's rare; joined so while it's up, it
//   closes): who is in it, ● JOIN, BOOT LATER;
// - the LOST sheet, when LOST's alarm starts (a latched mic only): whose mic, not heard for how
//   long, ● SILENCE, BOOT closes (and silences); BACK when it's heard again.
// Not over the lock, an off face or the desk face, the Recorder, a ringing alarm, the agent sheet
// while it listens or replies. The call and conversation sheets don't rise over the Comms screen,
// which shows them in place (the call plays anyway, and stays not heard); the ring and LOST
// sheets do. In its first second the ring sheet's ● doesn't answer: a press meant for the screen
// under it mustn't answer a ring. Where the LOST sheet can't rise, the first key anywhere
// silences the alarm (and does nothing else), and the sheet rises once it may.
//
// HOLD BOOT on these screens and sheets is the mic's (apps/hold_boot.cpp calls commsTalk() and
// commsTalkStop()), or the ringing sheet's message; elsewhere it's the agent's, and the engine
// pauses an open mic while the agent listens or speaks (the screens say PAUSED).
//
// The status bar's marks: an inverted speech bubble with a count while a conversation is on in
// one of this board's groups and this board isn't in it (the count: the boards in it); MIC while
// this board's mic is on.
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

// From loop(): raises the ring sheet when a ring starts here, the call sheet once per
// conversation whose voice plays here, the conversation sheet once per conversation this board
// isn't in, and the LOST sheet when LOST's alarm starts.
void pollComms();

// HOLD BOOT's two ends where a screen's talkTo() says comms (apps/hold_boot.cpp, from the input
// timer): on the ringing sheet the message starts (the sheet shows status().error as a notice if
// it doesn't) and the release sends it; elsewhere the mic goes on to the target (the ring sheet
// answers the ring first) and the release switches it off, unless ● latched it meanwhile. A
// latched mic at the press: the hold takes it over, and the release ends it.
void commsTalk(const ui::TalkTo &to);
void commsTalkStop();

namespace commsui {

// The reason the last attempt (the mic, PLAY LAST, a ring, a join, a message) from these screens
// didn't happen, for a "!" notice: for a few seconds after it. "" otherwise.
std::string attemptError();
void noteAttempt();

// PLAY LAST or a message is playing (status().replay): BOOT stops it first.
bool playing();
// Stops it, if one plays; true if it did (BOOT's first use on the Comms screens and sheets).
bool stopIfPlaying();

// --- The mic's keys ------------------------------------------------------------------------------
// For the screens where this board's mic is keyed (the talk screen, the call sheet, the dialer's
// answered ring among them, the ring sheet, a board's mic screen):
//   HOLD BOOT     keys the mic to the screen's target while it's held; letting go switches it off
//   ● while held  latches it: it stays on after BOOT goes (the still-heres say so: comms.h)
//   latched       the next ● or BOOT on these screens ends it and does nothing else; a HOLD BOOT
//                 takes it over (on while held, off at the release)
// ● ANSWER on the ring sheet leaves the mic off; HOLD BOOT there answers and talks.
bool micHeld();     // BOOT keys the mic now
bool micLatched();  // ● latched it
// The mic's hint: [HOLD BOOT] TALK, [●] HOLD while BOOT keys it, [BOOT] STOP while latched.
ui::Hint micHint();
// HOLD BOOT's target there: the mic to `target` in `group` (0: all of it).
ui::TalkTo micTalkTo(int group, uint32_t target);
// The ring sheet's, while it rings: the ring answered, and then the mic to `target` while held
// (to the dialer alone for a ring to this board alone).
ui::TalkTo ringTalkTo(int group, uint32_t target);
// The key filter's part (ui::nav::onKeyFirst(), comms_sheet.cpp): ● while held latches; ● or BOOT
// while latched, on a screen whose talkTo() says comms, ends it. True: the key is used up.
bool latchKey(ui::Key key);
// From pollComms(): a latch whose mic went off another way (a leave, a power-off) is let go.
void syncLatch();

// The status bar's MIC: On while this board's mic is on, Paused while something else has it for
// now (not at its start).
ui::MicMark micMark();

// "QUIET", "MIC ON", "HEARING", "OFF".
const char *stateWord(comms::State s);
// Segments lit of 20 for a level in dBFS (-60 to 0, 3 dB each).
int meterFill(float levelDb);
// "0:06": a length in seconds.
std::string lengthText(uint32_t ms);
// A length that grows, for while nothing moves: only whole minutes, all of them turning over
// together with the status bar's clock ("<1 MIN", "12 MIN", "1 H 05").
std::string minutesText(uint32_t ms);
// LOST's length: 10 s steps under a minute ("0:20"), then minutesText().
std::string lostText(uint32_t ms);
// When a call began: "14:08" today, "21 NOV" before, "5 MIN" ago while the clock isn't set.
std::string whenText(time_t at, uint32_t agoS);
// "KITCHEN", or a board not heard from yet by its id.
std::string boardName(const std::string &name, uint32_t id);
// "KITCHEN → ALL", "YOU → KITCHEN", "KITCHEN → YOU".
std::string whoText(const comms::Call &c);
// A note for the log: for a mic session "LIVE", "LOST", "CUT OFF", "NOT PLAYED", "VIA KITCHEN";
// for a ring "RINGING", "ANSWERED", "RANG", "MISSED", "NO ANSWER"; for a message "MSG COMING",
// "NEW MSG", "MSG", "MSG PART", "RECORDING", "MSG SENT", "MSG · 1 GOT IT"; "" for none.
std::string noteText(const comms::Call &c);
// The note asks for attention (a missed ring, a LOST not seen, a message not played): inverted.
bool noteNeedsYou(const comms::Call &c);
// Its length: "0:06" once it's over, whole minutes while it's live (minutesText()).
std::string callLength(const comms::Call &c);

// "TODAY · 6" when every call listed is from today, else "6 CALLS"; "" for none.
std::string countText(const std::vector<comms::Call> &calls);
// "1 BOARD", "2 BOARDS".
std::string boardsText(int n);

// Who is in a conversation: "YOU + KITCHEN" (this board in it), "KITCHEN + GARAGE".
std::string membersText(const comms::Conversation &c);
// How long it has been on, in whole minutes: "JUST BEGUN", "FOR 1 MIN", "FOR 1 H 05".
std::string forText(uint32_t ms);
// The group's name, "" for none.
std::string groupName(int group);

// A board as the sheets say how it's reached: "ON AIR · −52 dBm", "VIA KITCHEN · 1 HOP"; "" if
// not known. The signal as steadyRssi() keeps it.
std::string reachText(uint32_t id, comms::Route route, int hops, const std::string &relay,
                      int rssi);
// A board's signal as the screens show it: it changes only when it has moved 6 dB from what was
// shown, so a signal wavering between two values doesn't redraw the screen.
int steadyRssi(uint32_t id, int rssi);
// "AGENT LISTENING": mic().paused, short enough for a line beside the map.
std::string pausedText(const std::string &why);
// A board's open mic for its row and the sheets: "MIC ON <1 MIN", "MIC ON 12 MIN · LATCHED",
// "MIC ON 1 MIN · PAUSED", "LOST 0:30"; with "TO YOU" or "TO GARAGE" after the minutes for a mic
// to one board.
std::string micLine(const comms::OpenMic &m);
// The open mic of board `id` in group `group`, or nullptr.
const comms::OpenMic *findMic(const std::vector<comms::OpenMic> &mics, int group, uint32_t id);
// Home's MICS: "none on", "KITCHEN on · 12 min", "2 on", "yours on", "yours + 1 on",
// "KITCHEN lost".
std::string micsGlance();

// A key came within the last 30 s: the meters move and lengths count seconds. Otherwise they move
// only while voice goes or comes.
bool keyedLately();

// A 30 px log row: the time, who to whom (and the note after a dot, without a note column), the
// length on the right; a 1 px dashed rule under it.
void drawCallRow(ui::Canvas &c, const ui::Rect &r, const comms::Call &call, bool noteColumn);
// What a log row shows, for a screen's record of what it drew.
std::string callRowKey(const comms::Call &call);

// A 20-segment meter (-60 to 0 dBFS) after its label (a 34 px column), the segments `h` px tall,
// in `color`.
void drawMicMeter(ui::Canvas &c, int x, int y, int w, int h, const char *label, int filled,
                  uint16_t color = ui::kInk);

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

// The sheets and screens, for each other: the call sheet for a conversation this board is in now
// (answered, joined, or the dialer's ring answered), with `peer` the board it's with (or 0), the
// label over its name (FROM, WITH, ANSWERED BY) and how it began (ANSWERED, JOINED); the dialer's
// ringing sheet; the Messages sheet for a group; a board's mic screen.
std::unique_ptr<ui::Screen> makeConversationCallSheet(int group, uint32_t peer, bool toAll,
                                                      const char *label, const char *how);
std::unique_ptr<ui::Screen> makeRingingSheet(int group, uint32_t target);
std::unique_ptr<ui::Screen> makeMessagesSheet(int group);
std::unique_ptr<ui::Screen> makeMicScreen(int group, uint32_t board);

}  // namespace commsui
}  // namespace apps

#endif  // INKDECK_COMMS
