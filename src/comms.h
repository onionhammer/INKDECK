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
// signal (in steps of 4 dB), name or flags, the call log, a setting, the state; and while a call
// is on, at most 4 times a second, for the meter and the seconds.
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
                    // didn't play
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
  std::string error;     // why the last talkStart() or playLast() didn't happen, "" otherwise
};
Status status();

// --- Calls -----------------------------------------------------------------------------------

struct Call {
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
  int heardBy = 0;      // outgoing: boards that said it played there
  uint32_t lost = 0;    // incoming: 20 ms frames that never came (played as silence)
};
// Newest first; group -1 for every group.
std::vector<Call> calls(int group, int max = kMaxCalls);
void markHeard(int group);  // the group's calls count as heard (-1: every group's)

// --- Talking and playing -----------------------------------------------------------------------

// Talks to a group (target 0) or one board of it: the mic opens with the push-to-talk cue, and
// the voice goes out until talkStop() or kMaxTalkMs. A call being heard stops. False, with the
// reason in status().error, if it can't start now: no such group, a net-only group (the link isn't
// there yet), the Recorder or an alarm, the agent listening. The mic itself opens in the comms
// task; if it won't, status().error says so too.
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
void stopPlaying();  // what's playing stops (a live call goes on in the log, unheard)

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

// Groups, boards, state, channel and counters: sent, received, dropped by reason, relayed.
void dump(Print &out);
// A test of the parts that need no radio: HKDF and AES-GCM, the replay window, ADPCM, the
// packet format, the seen-cache. Prints PASS or FAIL a check; true if all passed.
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

// The board is powering off: the radio's ESP-NOW side stops now, for good (power.cpp, before
// Wi-Fi goes).
void forceOff();

}  // namespace comms
