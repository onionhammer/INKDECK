// COMMS (issue #66): boards talk to each other directly over the radio. Walkie-talkies in groups
// (push to talk, a latch for a room, half duplex), and a mesh: boards pass on each other's packets. This is the engine; the screens build on the API
// below. docs/comms-protocol.md has the packet format and the key derivation.
//
// Transport: ESP-NOW broadcasts, board to board with no router, each frame at most 250 bytes.
// ESP-NOW needs Wi-Fi started but no network (wlan::holdRadio()), and sends on the radio's
// channel: the network's while the board is on one, else mesh_channel. Receiving needs Wi-Fi's
// modem sleep off, so comms keeps it off while it listens (other features turn it back on when
// they finish; comms turns it off again within a quarter of a second).
//
// Groups: up to 4 mesh_group lines in SD:/inkdeck.ini (settings.h), each a secret its boards
// share. Each secret gives the group's AES-GCM key, its id (in every packet's header; the first 4
// hex digits show on screen) and a rendezvous id for later. Without a line, a board with an
// agent_token is in one group, HOME, made from the token under another label. Each board has a
// random id, made once and kept in NVS, and a name (mesh_name).
//
// Mesh: flooding, but for voice. A packet not seen before is handled, then sent on with one hop
// less (3 relays at most), also for groups this board isn't in (unread: by the hop count alone),
// while RELAY FOR OTHERS is on. A voice or message frame goes on only if a board this one hears
// straight would otherwise miss it: every board says every few seconds which boards it hears
// straight and how well (comms_mesh.h). For every board comms keeps how it's reached: straight
// over the air (with its signal), through relays (how many hops, and the last relay), or not
// lately (away).
//
// The mic, a walkie-talkie's: on while HOLD BOOT is held on the Comms screens (push to talk), or
// latched on (● while held: "hold transmit") until the next ● or BOOT there; switched on this
// board only (micOn(), micOff(), latchMic()); no packet switches a mic on, and it doesn't survive a
// restart. While it's on every frame it captures goes out. Half duplex: while this board
// transmits, nothing of comms plays on its speaker; it hears again when its mic goes off (or
// pauses). Several boards can transmit at once; each board plays the others' voices mixed (up to
// 4) while its own mic is off. A board left in a room with its mic latched is how the room is heard
// from elsewhere: there is no other feature for that.
//
// Voice: IMA ADPCM, 16 kHz mono (64 kbps), 20 ms a packet, each packet decodable on its own. Voice
// doesn't play over the Recorder, a ringing alarm, the agent listening or speaking, or a message
// being recorded; it stops music, which plays on once comms lets the speaker go. The last 60 s
// heard are kept (PLAY LAST), and a log in RAM: one entry per mic session (on to off).
//
// Conversations: a group has at most one. A mic that's on is in it: switching a mic on joins the one
// that's on, or opens one, and every board of the group that hears that mic is in it (the
// walkie-talkie); a mic to one board alone puts that board in. Boards in it play its voice;
// leave() takes this board out (its voice stops here, and this board's mic goes off) until
// join(). It's on while any mic in it is on, and for 60 s after the last word once none is, or
// until nobody is in it. Its boards say so every few seconds, so every board of the group sees
// it's on, and who is in it.
//
// Ringing: ringStart() rings all of a group or one board of it, once a second, until someone
// answers, the dialer gives up or 60 s pass (NO ANSWER). Every board of the target that isn't in
// the conversation already, and whose mic is off, rings: a ring chirp every few seconds (not over
// the Recorder, an alarm, the agent; over music and voice) and incomingRing() for the screens. One
// answer stops it everywhere; answering puts the board in the conversation, its mic still off. 5 s
// into a ring with no answer the dialer may leave a message instead (up to 60 s): it's sent to
// arrive complete (every board asks again for the frames it missed) and kept on the card
// (SD:/.inkdeck/comms/), in RAM without one, until played and for a day after. It never plays by
// itself.
//
// When it listens: on USB power, or while something holds it (hold(); the Comms screens and the
// call sheet), and for 10 s after; while this board's mic is on, or it's in a conversation where
// another board's mic is on; never without a group. On battery with none of that the radio is off,
// and nothing comes in, unless the board sleeps reachable (mesh_reachable, below): then it polls.
//
// Threading: one task ("comms") owns the radio, the keys and the audio paths. Every call below is
// safe from any task (the UI's included) and returns at once: the ones that act post a command to
// that task. Reads return snapshots. Nothing calls back into UI code: a screen polls generation()
// and redraws when it has changed.
//
// `via`, per group: air (the radio only), net (through inkharness only) or both. The inkharness
// link isn't built yet: netUp() is false, a net group sends nothing, and a both group goes over
// the air. A change made on the board (cycleVia()) is kept in NVS until the group's card line
// changes.
//
// INKDECK_COMMS=0 (features.h) leaves all of this out of the build; callers check the flag.
#pragma once
#include <stdint.h>
#include <time.h>

#include <string>
#include <vector>

#include "features.h"
#include "settings.h"

class Print;

namespace comms {

constexpr int kMaxGroups = settings::kMaxMeshGroups;  // 4
constexpr int kMaxBoards = 16;   // remembered a group; the one away longest makes room
constexpr int kMaxCalls = 48;    // in the log, all groups together; the oldest goes

using Via = settings::MeshVia;  // Both, Air, Net
const char *viaName(Via v);     // "AIR+NET", "AIR", "NET", as the screens show it

// After settings::loadCard(). Starts the task if a group is configured; does nothing otherwise.
void begin();
bool configured();  // at least one group

// The Comms screens and the call sheet hold the radio while they're up; each hold() is matched by
// a release(). On USB the board listens anyway.
void hold();
void release();

// Changes whenever anything below that a screen shows has changed: a group, a board's route,
// signal (in steps of 4 dB), name or flags, the call log, a setting, the state, a ring, a
// conversation, a message, this board's mic (on, off, paused, latched), the open mics heard
// (on, off, LOST, cut off, the alarm, their bars); while a mic is on, voice
// plays, a ring or a message is on, at most 4 times a second, for the meters and the seconds;
// and when a mic's level moves a segment (3 dB), at most 4 times a second. Seconds counting up
// (on for, heard ago, LOST for) don't change it: poll for them.
uint32_t generation();

// --- Groups --------------------------------------------------------------------------------

struct Group {
  std::string name;  // upper case: the line's name=, else GROUP 1...; HOME for the token's
  char tag[5] = {};  // "7F3A": the group id's first 4 hex digits
  uint32_t id = 0;   // the whole group id
  Via via = Via::Both;      // in use: the card's, or the board's change to it
  Via cardVia = Via::Both;  // what the card's line says
  bool fromToken = false;   // the default group, from agent_token
  std::string hub;          // the line's hub=, "" if none (not used yet)
  std::string rendezvous;   // 32 hex digits (not used yet)
  int boards = 0;   // other boards known in it (this one not counted)
  int onAir = 0;    // of them, reached over the air now, directly or through relays
  int onNet = 0;    // reached through inkharness now (0 until that link is built)
  int micsOn = 0;   // of them, with their mic on now (LOST ones included)
  int unheard = 0;  // mic sessions heard here while no screen was up, rings missed, messages not
                    // played
  bool active = false;  // the one on screen
};
std::vector<Group> groups();  // in the card's order
int activeGroup();            // index into groups(); 0 if there's one
void setActiveGroup(int group);  // kept in NVS
// AIR+NET -> AIR -> NET -> AIR+NET (the Groups sheet's HOLD ●), kept in NVS.
void cycleVia(int group);
void setVia(int group, Via via);

// --- Boards --------------------------------------------------------------------------------

enum class Route : uint8_t {
  Away,     // nothing from it for about 95 s (it says where it is every 30 s)
  Air,      // straight over the air
  Relayed,  // through other boards
  Net,      // through inkharness (not yet)
};

// Flags a board sends with its presence.
constexpr uint8_t kFlagUsb = 0x01;    // on USB power: it listens all the time
constexpr uint8_t kFlagRelay = 0x02;  // it relays for others
constexpr uint8_t kFlagMic = 0x04;    // its mic is on
constexpr uint8_t kFlagLong = 0x08;   // it sends at the long-range rate (mesh_range = long)

struct Board {
  uint32_t id = 0;
  std::string name;  // upper case; "" until its presence has come
  Route route = Route::Away;
  int hops = 0;          // Relayed: relays passed (1 HOP); 0 otherwise
  uint32_t relay = 0;    // Relayed: the board it came through last, 0 if unknown
  std::string relayName; // that board's name, "" if unknown (not in this board's groups)
  int rssi = 0;          // dBm of the last packet straight from it; 0 if none
  time_t heardAt = 0;    // last packet from it, UTC; 0 while the clock isn't set
  uint32_t heardAgoS = 0;  // the same, as seconds ago (works without the clock)
  time_t talkedAt = 0;   // its voice last heard here, 0 if none
  uint8_t flags = 0;     // kFlag*
  int battery = -1;      // percent, -1 if it has none or didn't say
  // The rate its last packet straight from it came at ("1M", "LR 500K"...; "" if none), and the
  // radio's noise floor then (dBm, 0 if none).
  std::string rxRate;
  int noise = 0;
  // Asleep and reachable on battery (mesh_reachable): its last packet was a poll, not a presence.
  // It wakes for a ring, voice it would hear or a message within pollS seconds.
  bool asleep = false;
  int pollS = 0;
};
// The other boards of a group: reached ones first (over the air by signal, then by hops), then
// the away ones, most recently heard first.
std::vector<Board> boards(int group);

// --- Status ----------------------------------------------------------------------------------

enum class State : uint8_t {
  Off,      // not listening: on battery with nothing holding it, or no group
  Quiet,    // listening
  Mic,      // this board's mic is on (others may be heard at once: hearing)
  Hearing,  // voice (or PLAY LAST, a message) plays here, this board's mic off
};

struct Status {
  State state = State::Off;
  int group = 0;         // the mic's group while it's on, else what plays, else the active group
  // Mic: the board its mic is to (0: all of the group). Hearing: the board heard (the loudest of
  // those playing; for PLAY LAST, the last heard).
  uint32_t peer = 0;
  std::string peerName;  // the same by name ("" for all, or a board not heard from yet)
  bool toAll = true;     // the mic, or the voice heard, is to all of the group (else to one board)
  bool replay = false;   // PLAY LAST or a message plays (alone, or with live voice)
  uint32_t ms = 0;       // Mic: on for; Hearing: the session heard so far, or the replay
  float levelDb = -96;   // Mic: the mic; Hearing: what plays; dBFS, for the meter
  bool hearing = false;  // voice plays here now (in any state)
  int voices = 0;        // boards whose voice plays here now (mixed)
  // Voice comes while this board transmits: it isn't played (half duplex). The loudest board's.
  bool talkedOver = false;
  uint32_t talkedOverBy = 0;
  std::string talkedOverName;
  float playLevelDb = -96;  // what plays, dBFS (the meter for what's heard, while the mic is on)
  // How the voice heard comes.
  Route route = Route::Away;
  int hops = 0;
  std::string relayName;
  int rssi = 0;
  int channel = 0;       // the radio's Wi-Fi channel; 0 while off
  bool onNetwork = false;  // on a Wi-Fi network: the channel is the network's, not mesh_channel
  bool netUp = false;    // the inkharness link (always false for now)
  bool longRange = false;  // this board sends at the long-range rate (range()); false while off
  // Changes when voice first plays here in a conversation (once a conversation, not each press):
  // the UI raises the call sheet over any screen when it does (unless the Recorder or an alarm is
  // up, which no voice plays over).
  uint32_t incoming = 0;
  uint32_t ringsSeen = 0;  // changes each time a ring starts on this board (incomingRing())
  // Changes when a conversation this board isn't in comes on (once each), in this group: the
  // screens offer to join it.
  uint32_t conversationsSeen = 0;
  int conversationGroup = 0;
  uint32_t messagesIn = 0;      // changes each time a message arrives here (messages())
  uint32_t playingMessage = 0;  // a message plays (replay is true too): its id; 0 otherwise
  bool alarm = false;           // LOST's alarm sounds (openMics() says whose)
  std::string error;  // why the last micOn(), playLast(), ringStart() and so on didn't happen,
                      // "" otherwise
};
Status status();

// --- The log ---------------------------------------------------------------------------------

enum class Kind : uint8_t {
  Voice,    // a mic session, on to off (this board's, or one heard here)
  Ring,     // a ring, answered or not
  Message,  // a message left (messages() has more)
};

struct Call {
  Kind kind = Kind::Voice;
  time_t at = 0;         // when it began, UTC; 0 while the clock isn't set
  uint32_t agoS = 0;     // seconds ago it began
  int group = 0;
  bool outgoing = false;  // this board's
  uint32_t from = 0;      // the mic's board (this board's id when outgoing), the dialer, the sender
  std::string fromName;
  uint32_t to = 0;        // 0: all of the group, else that board (this one, for one to us)
  std::string toName;
  uint32_t ms = 0;        // how long (Voice: the mic was on; a ring: it rang)
  bool live = false;      // still going
  // Incoming: how it came (Air, or Relayed with hops and the last relay), and its signal.
  Route route = Route::Air;
  int hops = 0;
  std::string relayName;
  int rssi = 0;
  bool played = false;  // incoming voice: some of it came out of the speaker here
  bool heard = false;   // incoming: it played while a Comms screen or the call sheet was up, or
                        // markHeard() since
  bool linkLost = false;  // Voice, incoming: it ended LOST (no off came), not with its off
  bool cutOff = false;    // Voice, incoming: held, it went silent with no off (out of range?)
  int heardBy = 0;        // outgoing message: boards that have it whole
  uint32_t lost = 0;      // incoming voice: 20 ms frames that never came (covered)
  uint32_t conversation = 0;  // Voice: the conversation it was in
  bool answered = false;  // Ring: someone answered (incoming: this board, or another)
  bool missed = false;    // Ring, incoming: it rang out here unanswered and not ignored (the
                          // dialer gave up, rang 60 s, or went silent); stays after markHeard()
  bool complete = true;   // Message: every frame came
  uint32_t message = 0;   // Message: its id, for playMessage()
};
// Newest first; group -1 for every group. A ring missed here isn't heard; a message is heard once
// played (markHeard() leaves messages as they are).
std::vector<Call> calls(int group, int max = kMaxCalls);
void markHeard(int group);  // the group's calls count as heard (-1: every group's)

// --- The mic ---------------------------------------------------------------------------------
//
// On: this board's mic captures from now until micOff(), a power-off, a restart, or leave(); it
// joins the group's conversation (or opens one) and its voice goes to all of it (target 0, or a
// board in it), or to one board alone. The screens key it with HOLD BOOT (push to talk), and ●
// while held latches it (latchMic(): it stays on after the release; apps/comms_ui.h). The cue
// plays as it opens, and nothing goes out until it has played (the first kMicSettleMs): from
// then every 20 ms frame goes out as captured. Every kStillHereMs the mic says it's on (still here:
// latched or not, the loudest frame since, its power, since when), and at once when it latches;
// micOff() sends a clear off (three times), after the last, short frame of what was captured.
// Half duplex: while it captures, nothing of comms reaches the speaker (status().talkedOver says
// when a board's voice comes meanwhile); it plays again once the mic is off or paused.
//
// Something else may take the mic for a while; it pauses (mic().paused says why) and picks up
// 0.4 s after by itself: the agent listening or speaking, the Recorder, a ringing alarm, music
// started on this board, a message this board records. Paused, it doesn't transmit: voice from
// others plays meanwhile where nothing stops it.
//
// While it's on, the board doesn't ring (nor answer rings with "ringing") and doesn't chirp. It
// sounds LOST's alarm only for a latched mic that was on before its own.

constexpr uint32_t kMicSettleMs = 280;  // dropped at the start: the codec's thump, and the cue
constexpr uint32_t kStillHereMs = 2000;
constexpr uint32_t kLostMs = 10000;   // a latched mic heard here silent this long, no off: LOST
constexpr uint32_t kCutMs = 5000;     // a held one silent this long, no off: cut off, no alarm
constexpr uint32_t kLongMicMs = 5 * 60000;  // a mic on longer chirps once where it's heard at off
constexpr int kHistoryBars = 60;       // 2 hours of
constexpr uint32_t kHistoryBarMs = 120000;  // 2-minute bars, the loudest frame of each
constexpr int8_t kNoBar = -128;        // a bar with no level: before the start, the mic off
constexpr int kMaxVoices = 4;          // boards whose voice plays at once (mixed)
constexpr int kMeterMinDb = -60;       // the level meters: -60 to 0 dBFS in 3 dB segments
constexpr int kMeterStepDb = 3;

struct Mic {
  bool on = false;
  int group = 0;
  uint32_t target = 0;     // 0: all of the group, else the board it's to
  std::string targetName;  // "" for all, or a board not heard from yet
  uint32_t conversation = 0;
  bool opened = false;     // it opened the conversation (boards that hear it join)
  bool latched = false;    // latched on (held transmit), not just held
  time_t since = 0;        // UTC; 0 while the clock isn't set
  uint32_t ms = 0;         // on for
  float levelDb = -96;     // the mic now: the loudest frame of the last 200 ms, as it goes out
  float gainDb = 0;        // the automatic gain on it now (comms_mic.h MicAgc)
  bool capturing = false;  // the mic runs: it transmits (else paused)
  std::string paused;  // why the mic is off now: THE AGENT IS LISTENING (or SPEAKING), THE
                       // RECORDER IS ON, AN ALARM IS RINGING, MUSIC IS PLAYING, A MESSAGE IS
                       // RECORDING, STARTING, THE MIC WON'T START; "" while it runs
  std::vector<int8_t> bars;    // kHistoryBars, oldest first, the last the bar now: dBFS or kNoBar
  uint32_t delayMs = 0;  // mouth to ear, to a board that plays it: the capture and the other
                         // board's buffers (computed from what was measured)
  uint32_t framesSent = 0, stillHeres = 0, histories = 0;
  uint32_t firstFrameMs = 0;  // the session's first frame sent, after micOn() (0: none yet)
  uint32_t count = 0;     // changes each time it goes on
  bool vanished = false;  // micVanish(): nothing goes out
};
Mic mic();
// Switches this board's mic on in a group, to all of it (target 0) or one board. Again while on:
// to the new group or target (a new session). False (status().error) if it can't: no such group,
// net only, the Recorder or an alarm on. The mic opens in the comms task; mic().paused says if it
// won't.
bool micOn(int group, uint32_t target);
void micOff();  // the off goes out; the mic closes
bool micEnabled();  // on, or switching on (from any task, at once)
// The mic that's on is latched (held transmit): it says so at once (a still-here), and the boards
// that hear it alarm if it goes silent (LOST) instead of cutting it off. Off goes with micOff().
void latchMic();

// --- Open mics heard ----------------------------------------------------------------------------

enum class Link : uint8_t {
  On,    // on: its voice comes (or it's paused)
  Lost,  // latched, and nothing from it for kLostMs and no off: LOST (the alarm, if heard here)
};

// A LOST outage of a mic, as this board saw it.
struct MicEvent {
  time_t at = 0;        // its start (the last packet before it), UTC; 0 while the clock isn't set
  uint32_t agoS = 0;
  uint32_t ms = 0;      // so far, while live
  bool live = false;
};

// Another board's mic that's on (or LOST), in one of this board's groups.
struct OpenMic {
  int group = 0;
  uint32_t id = 0;
  std::string name;
  uint32_t target = 0;   // 0: to all of the group; else the board it's to
  bool toMe = false;     // to this board alone
  bool forMe = true;     // to all, or to this board: its voice is for this board
  bool heard = false;    // its voice plays here: this board is in its conversation (or it's to
                         // this board alone), and didn't leave
  bool playing = false;  // its voice comes out of the speaker now
  bool opened = false;   // it opened the conversation
  bool latched = false;  // latched on (held transmit): it goes LOST, not cut off
  uint32_t conversation = 0;
  Link link = Link::On;
  uint32_t linkMs = 0;   // in this state for (LOST: since its last packet)
  uint32_t onS = 0;      // on for, as it says (and counted on here)
  time_t since = 0;      // its session's start, UTC; 0 while the clock isn't set
  float levelDb = -96;   // what plays (200 ms) while it plays; else its loudest since its last
                         // still-here
  bool paused = false;   // something else has its mic for now
  bool alarm = false;    // LOST's alarm sounds for it
  int battery = -1;      // percent, -1 none
  bool usb = false;
  uint32_t heardAgoMs = 0;
  Route route = Route::Away;
  int hops = 0;
  int rssi = 0;
  uint32_t delayMs = 0;  // from its mic to this speaker: the capture and the buffers (computed)
  std::vector<int8_t> bars;  // as Mic::bars, from its board (asked for when this board came late)
  std::vector<MicEvent> events;  // newest first: its LOSTs since this board heard it on
  uint32_t frames = 0, lost = 0, late = 0, histories = 0;
};
// The mics on in this board's groups (group -1: all), by group, then the longest on first.
std::vector<OpenMic> openMics(int group = -1);
// LOST's alarm stops, for every mic (LOST stays shown until the link is back, or the conversation
// is over here).
void ackLost();

// This board's mic is on, or it's in a conversation where another board's mic is on, or LOST's
// alarm sounds: it keeps its radio on and mustn't power off by itself (power.cpp).
bool keepAwake();

// --- Playing ---------------------------------------------------------------------------------

struct LastCall {
  bool have = false;
  int group = 0;
  uint32_t from = 0;    // the board heard last
  std::string fromName;
  uint32_t ms = 0;      // kept: the last 60 s heard (voice only, the quiet between left out)
  time_t at = 0;        // when what's kept began
};
LastCall lastCall();
// Plays the last 60 s heard again, over any voice that plays (it waits while this board's mic
// transmits: half duplex). False (status().error) if there's none, or a message is recording.
bool playLast();
void stopPlaying();  // PLAY LAST or a message stops (live voice goes on: leave() stops it)

// --- Conversations -------------------------------------------------------------------------------

struct Conversation {
  bool on = false;
  uint32_t id = 0;
  bool in = false;    // this board is in it: its voice plays here
  bool left = false;  // this board left it: its voice doesn't play here until join()
  std::vector<uint32_t> members;  // the other boards in it, as they say
  std::vector<std::string> names; // theirs, upper case ("" for a board not heard from yet)
  int micsOn = 0;        // boards in it with their mic on, this one included
  uint32_t ms = 0;       // on for this long
  uint32_t quietMs = 0;  // since the last word (0 while a mic is on in it)
  time_t since = 0;      // when it began, UTC; 0 while the clock isn't set
};
Conversation conversation(int group);
// This board is in the group's conversation from now on (its voice plays here). False if none is
// on. Its mic stays as it is.
bool join(int group);
// Out of it: its voice stops playing here until join(), and this board's mic goes off if it's on
// in that group.
void leave(int group);

// --- Ringing -------------------------------------------------------------------------------------

constexpr uint32_t kRingMs = 60000;        // a ring gives up after this long: NO ANSWER
constexpr uint32_t kMessageAfterMs = 5000;  // a message may be left from this far into a ring
constexpr uint32_t kMaxMessageMs = 60000;   // a message stops (and goes) after this long

enum class DialState : uint8_t {
  None,       // not ringing
  Ringing,    // ringing the target
  Answered,   // someone answered: the conversation is on (conversation(group))
  NoAnswer,   // rang kRingMs with no answer; a message may still be left
  Recording,  // recording a message (the ring has stopped: the others stopped ringing)
  Sent,       // the message went (it's kept a few minutes for boards that missed frames)
};

// This board's ring.
struct Dial {
  DialState state = DialState::None;
  int group = 0;
  uint32_t target = 0;     // 0: all of the group, else that board
  std::string targetName;  // "" for all, or a board not heard from yet
  uint32_t ms = 0;         // rung so far, or how long it rang
  int ringing = 0;         // boards that say they're ringing for it now
  uint32_t answeredBy = 0;
  std::string answeredByName;
  bool canLeaveMessage = false;  // kMessageAfterMs into a ring with no answer, or NO ANSWER
  uint32_t messageMs = 0;  // Recording: so far; Sent: its length
  int delivered = 0;       // Sent: boards that say they have every frame of it
  uint32_t count = 0;      // changes each time a ring starts
};
Dial dial();
// Rings all of a group (target 0) or one board of it. False (status().error) if it can't: no
// such group, net only, a ring or a message already going.
bool ringStart(int group, uint32_t target);
// A ring going: gives up (a cancel packet stops it on the other boards). One over (answered, NO
// ANSWER, a message sent): dial() goes back to None.
void ringStop();

// A ring on this board.
enum class RingEnd : uint8_t {
  None,          // ringing, or never rang
  Answered,      // answered here
  AnsweredThere, // another board answered it
  Ignored,       // ignoreRing() here
  GaveUp,        // the dialer gave up
  NoAnswer,      // the dialer rang kRingMs
  Lost,          // no ring packet for 5 s: the dialer went out of range or off
  Message,       // the dialer is leaving a message (messages() shows it coming)
};
struct Ring {
  bool on = false;
  int group = 0;
  uint32_t from = 0;
  std::string fromName;
  bool toAll = true;  // to all of the group; else to this board alone
  uint32_t ms = 0;    // ringing for this long (as the dialer counts)
  time_t since = 0;   // when it began, UTC; 0 while the clock isn't set
  uint32_t count = 0; // changes each time a ring starts here (status().ringsSeen too)
  RingEnd ended = RingEnd::None;  // how the last one ended, once it isn't on
};
Ring incomingRing();
// Answers the ring: the conversation is on, with this board and the dialer in it, and the ring
// stops on every board. This board's mic stays off (the screen switches it on if it wants). False
// if nothing rings.
bool answer();
void ignoreRing();  // stops it ringing here only

// --- Messages ------------------------------------------------------------------------------------

constexpr int kMaxMessages = 12;  // kept, all groups; the oldest played one goes first

// Records a message for the ring that wasn't answered (group -1: canLeaveMessage), or, for
// testing, for all of a group or one board of it directly. The ring stops (the boards it rang
// stop ringing), and the mic opens with the cue (this board's mic, latched, pauses meanwhile). Frames go out as
// they're recorded, numbered. False (status().error) if it can't.
bool messageStart(int group = -1, uint32_t target = 0);
void messageSend();    // the mic stops; the end goes out; dial() says Sent
void messageCancel();  // dropped: the others drop what came of it

struct Message {
  uint32_t id = 0;  // for playMessage()
  int group = 0;
  uint32_t from = 0;
  std::string fromName;
  uint32_t to = 0;   // 0: all of the group, else this board
  time_t at = 0;     // when it began, UTC; 0 if the clock wasn't set
  uint32_t agoS = 0;
  uint32_t ms = 0;
  bool receiving = false;  // still coming
  bool complete = false;   // every frame came (else the missing ones play as silence)
  bool played = false;
  bool onCard = false;     // on the card (else in RAM, gone at a power-off)
  uint32_t frames = 0;     // 20 ms each
  uint32_t sum = 0;        // the checksum of its frames (comms_calls.h)
};
std::vector<Message> messages(int group = -1);  // newest first
// Plays a message (status(): replay, playingMessage), over any voice that plays; it counts as
// played. False (status().error) if there's no such message, or a message is recording.
bool playMessage(uint32_t id);

// --- Settings (NVS) ----------------------------------------------------------------------------

bool relayForOthers();  // on by default
void setRelayForOthers(bool on);
bool callChirp();       // a chirp as voice first plays here in a conversation; on by default
void setCallChirp(bool on);

// The rate this board sends at: the card's mesh_range line (settings::meshRange()), unless the dev
// override says otherwise. Normal: 1 Mbps (802.11b). Long: Espressif's long-range rate, 500 kbps,
// which reaches further (each packet takes twice the air). Every board hears both rates (the
// radio takes the long-range protocol on top of 802.11b/g/n), so boards on either talk to each
// other; a board's presence says which it sends at (kFlagLong).
enum class Range : uint8_t { Normal, Long };
Range range();

bool netUp();            // the inkharness link: false (not built yet)
uint32_t boardId();      // this board's, made once and kept in NVS
std::string boardName(); // upper case: mesh_name, else tailscale_name, else INKDECK-XXXX
bool listening();        // the radio is up and receiving

// --- Reachable on battery (comms_reach.h; power.h decides when) ---------------------------------
//
// A board on battery that would power off can stay in light sleep with its radio off instead,
// waking every mesh_reachable seconds to ask the boards in range whether anything is waiting for
// it (a poll, on the channel the boards were last heard on). An awake board that knows of a ring
// that includes it, voice it would hear sounding now, or a message for it answers, and the
// sleeper stays up for it. Every board that listens answers for what it knows, so a ring needn't
// come from a board in range at that moment: rings flood. A poll that asks for it (every 3
// minutes) gets an answer from the boards on USB even with nothing, and three unanswered in a row
// sweep the channels.

// Seconds between a sleeping board's polls: the override (System's REACHABLE row, !commsreach),
// else mesh_reachable (settings::meshReachable()); 0 when off, or when no group is configured.
// Works before begin().
int reachInterval();
// The override: seconds (2 to 30) in place of mesh_reachable, 0 for off, -1 to remove it and go
// by the card's line again. Kept in NVS (reach/dev), so it survives restarts.
void setReachOverride(int seconds);
bool reachOverridden();
// A group to poll in: a mesh_group line, the agent token's group or the dev one. Before begin().
bool reachPossible();

// A ring nobody answered here (given up, 60 s with no answer, the dialer gone, or a message left
// instead) while audio::silent() and reachInterval() were both on: the off faces show it
// (apps/missed_ring.h). Kept in NVS until clearMissedRings(): a key, PWR or USB making the board
// an ordinary one. count 0: none. Works before begin().
struct MissedRing {
  uint32_t from = 0;
  std::string name;  // "" if its presence never came
  time_t at = 0;     // UTC; 0 if the clock wasn't set
  int count = 0;     // since the record began
};
MissedRing missedRing();
void clearMissedRings();

enum class ReachEnd : uint8_t {
  PowerKey,    // PWR (or the dev tools' 'o' while pretending)
  Due,         // the off face's next wake (its RTC alarm time) came
  Woken,       // an answer said something is waiting: the board goes on booting and listens
  Usb,         // USB power came
  LowBattery,  // under power::kLowBatteryPercent: the PMIC's power-off instead
  TestOver,    // !reachtest's stretch of real light sleep is over
  Failed,      // no group, or the radio wouldn't start
};
struct ReachArgs {
  time_t wakeAt = 0;      // UTC: the off face's next wake (0: none)
  int intervalS = 5;
  bool pretend = false;   // a delay for each light sleep: radio off, CPU waiting, console alive
  bool ignoreUsb = false; // !reachtest: on USB, behaving as on battery
  time_t testEnd = 0;     // UTC: TestOver then (0: none)
};
// power.cpp, early in setup() (nothing else started), in place of the PMIC's power-off: polls
// every intervalS with light sleep between, until one of ReachEnd. After Woken, begin() makes the
// board listen for what was waiting, as if held.
ReachEnd reachSleep(const ReachArgs &a);

// The board woke for comms (reachSleep() returned Woken): it listens and rings or plays through
// the usual paths. power.cpp asks each turn, saying whether a key has been pressed since: Stay;
// Sleep (30 s after the ring, a held mic's voice or the transfer is over, 60 s after that voice
// while a mic heard stays on: a latched mic's voice, a room's, counts for neither; 15 s if nothing
// came: the off face powers off again); On (a key: an ordinary board).
enum class Woken : uint8_t { None, Stay, Sleep, On };
Woken reachWokenStep(bool keyed);

// The battery log, for a board left unplugged (!reachlog, System): kept in RTC memory while it
// sleeps and in NVS every 10 minutes and at each restart. It starts again at the first sleep
// after the board was on USB (or with reachLogReset()).
struct ReachLog {
  uint32_t since = 0;      // UTC it began; 0: never
  int8_t startPct = -1;    // battery then
  int8_t lastPct = -1;     // battery at lastAt
  uint32_t lastAt = 0;     // UTC
  uint32_t polls = 0;      // sent (one a wake, whatever the groups)
  uint32_t answered = 0;   // polls that got an answer
  uint32_t echoes = 0;     // polls asking boards on USB to answer anyway (the channel check)
  uint32_t echoAnswered = 0;
  uint32_t sweeps = 0;     // channel sweeps, and those that found the boards
  uint32_t sweepFound = 0;
  uint32_t woken = 0;      // answers with something waiting
  uint32_t restarts = 0;   // for the off face's wakes (the clock's minute, check-ins, alarms)
  uint32_t pwr = 0;        // PWR woke it
  uint32_t radioMs = 0;    // the radio up for polls, summed
  uint32_t asleepMs = 0;   // in light sleep (or pretending), summed
  uint32_t awakeMs = 0;    // up after an answer woke it, summed
  uint8_t channel = 0;     // the channel it polls on
};
ReachLog reachLog();
void reachLogReset();
// !reachchan: the channel the next sleep polls on (as if the boards were last heard there), and
// the channel check's misses it starts with (3: the first echo unanswered sweeps). For testing.
void setReachChannel(int channel, int misses);
void reachLogFlush();      // to NVS (power.cpp, before a restart)
void dumpReach(Print &out);  // !reachlog: the log, the setting, the channel, the plan

// --- Diagnostics (devtools.cpp) ------------------------------------------------------------------

// Pings a group: every board that hears it answers. 0 if comms isn't listening or the group
// can't go over the air; else the ping's id.
uint32_t ping(int group);
struct Pong {
  uint32_t id = 0;
  std::string name;
  int rssiThere = 0;  // the ping as the other board heard it (0 if it came through relays)
  int hopsThere = 0;
  int rssiHere = 0;   // the answer as this board heard it (0 if relayed)
  int hopsBack = 0;
  uint32_t rttMs = 0;   // the round trip, heldMs included
  uint32_t heldMs = 0;  // how long the other board held its answer back (a random 5 to 155 ms)
};
std::vector<Pong> pongs();  // answers to the last ping, in the order they came

// !reachbench MODE N SLEEPMS LISTENMS: N light sleeps of SLEEPMS, each followed by a poll with
// the echo flag and up to LISTENMS for an answer, timed (docs/comms-protocol.md). Modes: a stops
// Wi-Fi before each sleep and starts it after; A also takes ESP-NOW down and up; b leaves Wi-Fi
// started across the sleep; c leaves it started in modem sleep and holds the radio awake around
// each poll (esp_wifi_force_wakeup_acquire()); p is a, with a delay for the light sleep; x runs
// a, A, b and c one after another. The report is kept in NVS ("reach"/"bench") for
// reachBenchShow(); after light sleep it ends in power::consoleRecover() (a power cycle).
void reachBench(char mode, int cycles, uint32_t sleepMs, uint32_t listenMs, Print &out);
void reachBenchShow(Print &out);

// Groups, boards, state, channel and rate, the boards heard straight and their links,
// conversations, the mic, open mics, rings, messages and counters: sent, received (by rate),
// dropped by reason, relayed and not; the duplex path's.
void dump(Print &out);
// A test of the parts that need no radio: HKDF and AES-GCM, the replay window, ADPCM, the
// packet format (the mic's types too), the seen-cache, a message's reassembly with frames lost and
// doubled, conversations on a made-up clock, the history, a mic heard (on, cut off when held,
// LOST when latched), the mixer, half duplex, push to talk's latch, once per conversation, and the
// relay rule. Prints PASS or FAIL a check; true if all passed.
bool selfTest(Print &out);

// Dev overrides (devtools.cpp's !commsgroup and !commschannel), kept in NVS: a test group from a
// secret, on top of the card's (the card's mesh_group lines are the real way), and a channel in
// place of mesh_channel. An empty secret or channel 0 removes them. They apply at once.
void setTestGroup(const std::string &secret, const std::string &name);
void setTestChannel(int channel);
// !commsrange: the rate in place of the card's mesh_range line, kept in NVS (set false: the card's
// line again); it applies at once. testRange() says if one is set.
void setTestRange(bool set, Range r = Range::Normal);
bool testRange();
// Relaying only when needed (comms_mesh.h), for testing with two boards: !commsdeaf drops whatever
// this board's radio gets from that board (its packets, and those it relays), as if it were out of
// range, so it's left out of this board's links (0: deaf to none); !commsflood relays every voice
// and message frame as before the rule (for comparing). Neither is kept.
void setDeaf(uint32_t id);
uint32_t deaf();
void setFlood(bool on);
bool flood();
// !commslrrx: the radio takes the long-range protocol (on, the default) or not (off: it no longer
// hears boards that send at the long-range rate), at once and for every join, until a restart. A
// change while joined drops the connection, which joins again. For testing what it costs.
void longRangeRx(bool on);
// Sends again, byte for byte, the last frame this board sent (firstVoice false: the others drop it
// as a duplicate) or the first voice frame of its last session (true: dropped as replayed, once the
// seen-cache has forgotten it). For showing those drops in !comms.
void resend(bool firstVoice);
// Testing (!commsringvanish, !commsdropmsg): the ring going stops sending ring packets, with no
// cancel, as if the dialer had gone out of range; and every nth message frame that comes is
// dropped as if lost (0: none), so the asking again shows.
void ringVanish();
void dropMessageFrames(int nth);
// Testing the mic (!commsvanish, !commstone, !commslong): this board sends nothing more, its off
// neither, as if it had gone out of range (false: it's back); for ms, a 1 kHz tone peaking at db
// dBFS is added to what the mic hears (nobody is there to speak: it's the voice); a mic heard
// counts as on long after ms instead of kLongMicMs (0: kLongMicMs again).
void micVanish(bool on);
void micTone(uint32_t ms, int db);
void setLongMicMs(uint32_t ms);

// The boards heard straight over the air (comms_mesh.h), for !commsloss: each one's packets since
// its boot began here, that came and that didn't (gaps in its sequence numbers), the gaps by
// length (1, 2-3, 4-15, 16 and more), its signal and the rate its last one came at.
struct LinkStat {
  uint32_t id = 0;
  uint32_t got = 0, missed = 0;
  uint32_t runs[4] = {};
  int rssi = 0;
  bool neighbour = false;  // heard straight in the last 20 s
};
std::vector<LinkStat> linkStats();

// The air survey (!commsair): for ms (1 to 30 s), the radio also takes every frame on its channel it
// can decode (promiscuous mode, comms and Wi-Fi going on meanwhile) and adds up how long each was
// on the air, from its length and rate: the share of the time the channel carried frames, by kind.
// Frames it can't decode (other channels' overlap, 2-stream frames, noise) aren't in it: the
// channel is busier than this says. Waits.
struct AirSurvey {
  bool ok = false;
  std::string error;
  int channel = 0;
  uint32_t ms = 0;
  // Frames and the share of the time on the air (0 to 1), by kind: ESP-NOW (this board's and
  // others'), data, beacons, other management, control, and what came without a payload
  // (2-stream and the like).
  enum Kind : uint8_t { kEspNow, kData, kBeacon, kMgmt, kCtrl, kOther, kKinds };
  uint32_t frames[kKinds] = {};
  float share[kKinds] = {};
  float total = 0;
  int loudestData = 0;  // dBm, the strongest data frame
};
AirSurvey airSurvey(uint32_t ms);

// The board is powering off: the mic's off goes first if it's on, then the radio's ESP-NOW side
// stops, for good (power.cpp, before Wi-Fi goes).
void forceOff();

}  // namespace comms
