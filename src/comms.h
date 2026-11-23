// COMMS (issue #66): boards talk to each other directly over the radio. Push-to-talk voice in
// groups, and a mesh: boards pass on each other's packets. This is the engine; the screens build
// on the API below. docs/comms-protocol.md has the packet format and the key derivation.
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
// Mesh: flooding. A packet not seen before is handled, then sent on with one hop less (3 relays
// at most), also for groups this board isn't in (unread: by the hop count alone), while RELAY FOR
// OTHERS is on. For every board comms keeps how it's reached: straight over the air (with its
// signal), through relays (how many hops, and the last relay), or not lately (away).
//
// Voice: IMA ADPCM, 16 kHz mono (64 kbps), 20 ms a packet, each packet decodable on its own.
// Half-duplex: while this board talks, incoming calls are ignored; of two calls at once, the first
// plays and the other is only logged. A call doesn't interrupt the Recorder, a ringing alarm, the
// agent listening or speaking, or a call already playing; it does stop music, which plays on
// afterwards. The last call heard is kept (PLAY LAST), and a log of calls in RAM.
//
// Conversations: a group has at most one, on from an answered ring or the first voice until 60 s
// after the last word, or until the last board leaves. Voice plays only on the boards in it: the
// ones that answered, joined, talked in it, or were listening and played its first call (the
// walkie-talkie: HOLD BOOT with no conversation on starts one, and every board that plays it is
// in), plus a call to one board alone, which puts that board in. leave() takes this board out
// until join(). Its boards say so every few seconds, so every board of the group sees it's on,
// and who is in it, even one that came late, was busy or left.
//
// Ringing: ringStart() rings all of a group or one board of it, once a second, until someone
// answers, the dialer gives up or 60 s pass (NO ANSWER). Every board of the target that isn't in
// the conversation already rings: a ring chirp every few seconds (not over the Recorder, an
// alarm, the agent or a call; over music, which plays on) and incomingRing() for the screens.
// One answer stops it everywhere. 5 s into a ring with no answer the dialer may leave a message
// instead (up to 60 s): it's sent to arrive complete (every board asks again for the frames it
// missed) and kept on the card (SD:/.inkdeck/comms/), in RAM without one, until played and for
// a day after. It never plays by itself.
//
// When it listens: on USB power, or while something holds it (hold(); the Comms screens and the
// call sheet), and for 10 s after; never without a group. On battery with nothing holding it the
// radio is off, and no calls come in.
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
constexpr uint32_t kMaxTalkMs = 60000;  // a call stops by itself after this long

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
// conversation, a message; and while a call, a ring or a message is on, at most 4 times a
// second, for the meter and the seconds.
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
  int unheard = 0;  // calls to the group or this board that played while no screen was up, or
                    // didn't play; rings missed; messages not played
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
constexpr uint8_t kFlagRoom = 0x04;   // monitoring its room (the baby monitor, later)

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
  time_t talkedAt = 0;   // its last call heard here, 0 if none
  uint8_t flags = 0;     // kFlag*
  int battery = -1;      // percent, -1 if it has none or didn't say
};
// The other boards of a group: reached ones first (over the air by signal, then by hops), then
// the away ones, most recently heard first.
std::vector<Board> boards(int group);

// --- Status ----------------------------------------------------------------------------------

enum class State : uint8_t {
  Off,      // not listening: on battery with nothing holding it, or no group
  Quiet,    // listening
  Talking,  // this board's mic is on the air
  Hearing,  // a call (or PLAY LAST) is playing
};

struct Status {
  State state = State::Off;
  int group = 0;         // the call's group while talking or hearing, else the active group
  uint32_t peer = 0;     // hearing: the caller; talking: the board talked to (0: all of the group)
  std::string peerName;  // the same by name ("" for all, or a board not heard from yet)
  bool toAll = true;     // the call is to all of the group (else to one board: this one, when hearing)
  bool replay = false;   // hearing PLAY LAST, not a live call
  uint32_t ms = 0;       // of the call so far
  float levelDb = -96;   // the mic (talking) or what plays (hearing), dBFS, for the meter
  // How the call being heard comes.
  Route route = Route::Away;
  int hops = 0;
  std::string relayName;
  int rssi = 0;
  int channel = 0;       // the radio's Wi-Fi channel; 0 while off
  bool onNetwork = false;  // on a Wi-Fi network: the channel is the network's, not mesh_channel
  bool netUp = false;    // the inkharness link (always false for now)
  // Changes each time a live call starts playing here: the UI raises the call sheet over any
  // screen when it does (unless the Recorder or an alarm is up, which no call plays over).
  uint32_t incoming = 0;
  uint32_t ringsSeen = 0;  // changes each time a ring starts on this board (incomingRing())
  // Changes when a conversation this board isn't in comes on (once each), in this group: the
  // screens offer to join it.
  uint32_t conversationsSeen = 0;
  int conversationGroup = 0;
  uint32_t messagesIn = 0;      // changes each time a message arrives here (messages())
  uint32_t playingMessage = 0;  // hearing a message (replay is true too): its id; 0 otherwise
  std::string error;  // why the last talkStart(), playLast(), ringStart() and so on didn't
                      // happen, "" otherwise
};
Status status();

// --- Calls -----------------------------------------------------------------------------------

enum class Kind : uint8_t {
  Voice,    // a call: push-to-talk
  Ring,     // a ring, answered or not
  Message,  // a message left (messages() has more)
};

struct Call {
  Kind kind = Kind::Voice;
  time_t at = 0;         // when it began, UTC; 0 while the clock isn't set
  uint32_t agoS = 0;     // seconds ago it began
  int group = 0;
  bool outgoing = false;  // this board talked
  uint32_t from = 0;      // the caller (this board's id when outgoing)
  std::string fromName;
  uint32_t to = 0;        // 0: all of the group, else that board (this one, for a call to us)
  std::string toName;
  uint32_t ms = 0;        // how long
  bool live = false;      // still going
  // Incoming: how it came (Air, or Relayed with hops and the last relay), and its signal.
  Route route = Route::Air;
  int hops = 0;
  std::string relayName;
  int rssi = 0;
  bool played = false;  // incoming: it came out of the speaker (else the board was busy)
  bool heard = false;   // incoming: it played while a Comms screen or the call sheet was up, or
                        // markHeard() since
  int heardBy = 0;      // outgoing: boards that said it played there (a message: that have it
                        // whole)
  uint32_t lost = 0;    // incoming: 20 ms frames that never came (played as silence)
  uint32_t conversation = 0;  // Voice: the conversation it was in (0: none, an older board's)
  bool answered = false;  // Ring: someone answered (incoming: this board, or another)
  bool complete = true;   // Message: every frame came
  uint32_t message = 0;   // Message: its id, for playMessage()
};
// Newest first; group -1 for every group. Ms of a ring is how long it rang. A ring missed here
// isn't heard; a message is heard once played (markHeard() leaves messages as they are).
std::vector<Call> calls(int group, int max = kMaxCalls);
void markHeard(int group);  // the group's calls count as heard (-1: every group's)

// --- Talking and playing -----------------------------------------------------------------------

// Talks to a group (target 0) or one board of it: the mic opens with the push-to-talk cue, and
// the voice goes out until talkStop() or kMaxTalkMs. A call being heard stops. False, with the
// reason in status().error, if it can't start now: no such group, a net-only group (the link isn't
// there yet), the Recorder or an alarm, the agent listening. The mic itself opens in the comms
// task; if it won't, status().error says so too.
// In a conversation: this board joins the group's conversation if one is on (else the call
// starts one, and the boards that play it are in it) and talks in it: to all of it (target 0, or
// a board in it), or to one board alone.
bool talkStart(int group, uint32_t target);
void talkStop();
bool talking();

struct LastCall {
  bool have = false;
  int group = 0;
  uint32_t from = 0;
  std::string fromName;
  uint32_t ms = 0;  // kept (the first 60 s of a longer one)
  time_t at = 0;
};
LastCall lastCall();
// Plays the last call heard again. False (status().error says why) if there's none, or the board
// is talking, hearing or busy as for a call.
bool playLast();
void stopPlaying();  // what's playing stops (a live call goes on in the log, unheard; a message)

// --- Conversations -------------------------------------------------------------------------------

struct Conversation {
  bool on = false;
  uint32_t id = 0;
  bool in = false;    // this board is in it: its voice plays here
  bool left = false;  // this board left it: its voice doesn't play here until join()
  std::vector<uint32_t> members;  // the other boards in it, as they say
  std::vector<std::string> names; // theirs, upper case ("" for a board not heard from yet)
  uint32_t ms = 0;       // on for this long
  uint32_t quietMs = 0;  // since the last word
  time_t since = 0;      // when it began, UTC; 0 while the clock isn't set
};
Conversation conversation(int group);
// This board is in the group's conversation from now on (its voice plays here, HOLD BOOT talks
// to it). False if none is on.
bool join(int group);
// Out of it: its voice stops playing here (a call of it playing stops) until join().
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
// stops on every board. False if nothing rings.
bool answer();
void ignoreRing();  // stops it ringing here only

// --- Messages ------------------------------------------------------------------------------------

constexpr int kMaxMessages = 12;  // kept, all groups; the oldest played one goes first

// Records a message for the ring that wasn't answered (group -1: canLeaveMessage), or, for
// testing, for all of a group or one board of it directly. The ring stops (the boards it rang
// stop ringing), and the mic opens with the cue. Frames go out as they're recorded, numbered.
// False (status().error) if it can't.
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
// Plays a message (status(): Hearing, replay, playingMessage); it counts as played. False
// (status().error) if there's no such message, or the board is busy as for PLAY LAST.
bool playMessage(uint32_t id);

// --- Settings (NVS) ----------------------------------------------------------------------------

bool relayForOthers();  // on by default
void setRelayForOthers(bool on);
bool callChirp();       // a chirp before a call plays; on by default
void setCallChirp(bool on);

bool netUp();            // the inkharness link: false (not built yet)
uint32_t boardId();      // this board's, made once and kept in NVS
std::string boardName(); // upper case: mesh_name, else tailscale_name, else INKDECK-XXXX
bool listening();        // the radio is up and receiving

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

// Groups, boards, state, channel, conversations, rings, messages and counters: sent, received,
// dropped by reason, relayed.
void dump(Print &out);
// A test of the parts that need no radio: HKDF and AES-GCM, the replay window, ADPCM, the
// packet format (the new types too), the seen-cache, a message's reassembly with frames lost and
// doubled, and conversations on a made-up clock. Prints PASS or FAIL a check; true if all passed.
bool selfTest(Print &out);

// Dev overrides (devtools.cpp's !commsgroup and !commschannel), kept in NVS: a test group from a
// secret, on top of the card's (the card's mesh_group lines are the real way), and a channel in
// place of mesh_channel. An empty secret or channel 0 removes them. The group applies at once.
void setTestGroup(const std::string &secret, const std::string &name);
void setTestChannel(int channel);
// Sends again, byte for byte, the last frame this board sent (firstVoice false: the others drop it
// as a duplicate) or the first voice frame of its last call (true: dropped as replayed, once the
// seen-cache has forgotten it). For showing those drops in !comms.
void resend(bool firstVoice);
// Testing (!commsringvanish, !commsdropmsg): the ring going stops sending ring packets, with no
// cancel, as if the dialer had gone out of range; and every nth message frame that comes is
// dropped as if lost (0: none), so the asking again shows.
void ringVanish();
void dropMessageFrames(int nth);

// The board is powering off: the radio's ESP-NOW side stops now, for good (power.cpp, before
// Wi-Fi goes).
void forceOff();

}  // namespace comms
