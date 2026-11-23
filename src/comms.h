// COMMS (issue #66): boards talk to each other directly over the radio. Open mics in groups, and
// a mesh: boards pass on each other's packets. This is the engine; the screens build on the API
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
// The mic: a board's mic is on or off, switched on this board only (micOn(), micOff()); no packet
// switches a mic on, and it doesn't survive a restart. While it's on it captures all the time, and
// a sound gate sends only what's over a threshold, with the 160 ms before it and 2 s after, so an
// open mic in a quiet room costs almost no airtime. Several boards can have theirs on at once and
// speak at once: each board plays the others' voices mixed (up to 4) while its own mic captures
// (audio's duplex path), and holds back its own speaker's echo (the gate's echo margin). A board
// left in a room with its mic on is how the room is heard from elsewhere: there is no other
// feature for that.
//
// Voice: IMA ADPCM, 16 kHz mono (64 kbps), 20 ms a packet, each packet decodable on its own. Voice
// doesn't play over the Recorder, a ringing alarm, the agent listening or speaking, or a message
// being recorded; it stops music, which plays on once comms lets the speaker go. The last 60 s
// heard are kept (PLAY LAST), and a log in RAM: one entry per mic session (on to off).
//
// Conversations: a group has at most one. An open mic is in it: switching a mic on joins the one
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
// and nothing comes in.
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
// conversation, a message, this board's mic (on, off, paused, sending), the open mics heard
// (on, off, QUIET, SOUND, LOST, the alarm, their sounds and bars); while a mic is on, voice
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
  // Changes each time a mic session starts playing here (once a session, not each sound): the UI
  // raises the call sheet over any screen when it does (unless the Recorder or an alarm is up,
  // which no voice plays over).
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
  uint32_t sounds = 0;    // Voice: the gate's sounds in it (heard here, or sent)
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
// board in it), or to one board alone. The push-to-talk cue plays as it opens. Each 20 ms frame's
// level (its loudest sample, dBFS) is compared with the threshold: a frame over it starts a sound,
// which goes out with the kPreRollMs before it, and on for kHangMs after the level drops, each
// frame kPreRollMs after it was captured. While the speaker plays, a frame no louder than the
// speaker's echo could be (its level, kEchoCouplingDb and kEchoMarginDb) doesn't count, and goes as
// silence if it goes: a voice over the echo gets through, the echo doesn't. Every kStillHereMs the
// mic says it's on (still here: its threshold, the loudest frame since, its power, since when);
// micOff() sends a clear off. Switched off while a sound goes (a push-to-talk release, mid-word),
// the frames the gate still holds back (kPreRollMs) go first, 20 ms apart, as if silence had come
// after, and the off 40 ms after the last of them (its presence says the mic is on until then): the
// end of the word arrives. The first 200 ms after the mic starts are dropped (its thump).
//
// Something else may take the mic for a while; it pauses (mic().paused says why) and picks up
// 0.4 s after by itself: the agent listening or speaking, the Recorder, a ringing alarm, music
// started on this board, a message this board records. Voice from others plays meanwhile only
// where nothing stops it (a message recording does; music doesn't take what's heard: voice stops
// it). Playing others' voice doesn't pause it: both run at once.
//
// While it's on, the board doesn't ring (nor answer rings with "ringing") and doesn't chirp: it's
// in the conversation already, and a room with someone asleep in it stays quiet. It still plays
// the conversation's voice: that's how the room is talked to. It sounds LOST's alarm only for a
// mic that was on before its own (the room board doesn't alarm when the board listening to it,
// which opened its mic later, goes).

constexpr int kMicMinDb = -60;    // the threshold, in kMicStepDb steps: 20 meter segments
constexpr int kMicMaxDb = 0;
constexpr int kMicStepDb = 3;
constexpr int kMicDefaultDb = -30;
constexpr int kLoudMarginDb = 12;     // a sound this far over a mic's threshold is LOUD, else SOFT
constexpr int kLowVolume = 30;        // at or below this volume a LOUD sound chirps once
constexpr uint32_t kPreRollMs = 160;  // sent before the first frame over the threshold
constexpr uint32_t kHangMs = 2000;    // sent on after the last
constexpr uint32_t kStillHereMs = 2000;
constexpr uint32_t kLostMs = 20000;   // nothing from a mic heard here this long, no off: LOST
constexpr uint32_t kLongMicMs = 5 * 60000;  // a mic on longer chirps once where it's heard at off
constexpr int kHistoryBars = 60;       // 2 hours of
constexpr uint32_t kHistoryBarMs = 120000;  // 2-minute bars, the loudest frame of each
constexpr int8_t kNoBar = -128;        // a bar with no level: before the start, the mic off
constexpr int kMaxVoices = 4;          // boards whose voice plays at once (mixed)
// The echo: what the mic makes of the speaker, against the level played into the DAC after the
// volume (measured 2026-11-23 with !commsecho, a 1 kHz tone through the duplex path: -26 dB at
// -20 dBFS and VOL 100, -25 at -10, -22 at full scale, -25 at full scale and VOL 50; the tone's
// abrupt start came back at -15 dB), and the margin a frame must clear over that before it
// counts, so the echo's transients don't open the gate (-22 + 9 = -13 dB, over the -15).
constexpr int kEchoCouplingDb = -22;
constexpr int kEchoMarginDb = 9;

// A sound this board's mic sent.
struct MicSound {
  time_t at = 0;        // its first frame (the pre-roll's), UTC; 0 while the clock isn't set
  uint32_t agoS = 0;
  uint32_t ms = 0;      // pre-roll and hang included
  float peakDb = -96;
  bool loud = false;    // kLoudMarginDb over the threshold
  uint32_t muted = 0;   // its frames sent as silence: the speaker's echo
  bool live = false;    // going now
};

struct Mic {
  bool on = false;
  int group = 0;
  uint32_t target = 0;     // 0: all of the group, else the board it's to
  std::string targetName;  // "" for all, or a board not heard from yet
  uint32_t conversation = 0;
  bool opened = false;     // it opened the conversation (boards that hear it join)
  time_t since = 0;        // UTC; 0 while the clock isn't set
  uint32_t ms = 0;         // on for
  int thresholdDb = kMicDefaultDb;  // the threshold (NVS), also while off
  float levelDb = -96;     // the mic now: the loudest frame of the last 200 ms
  float echoDb = -96;      // what the speaker would put on the mic now (the gate's echo level,
                           // margin included); -96 while it's silent
  bool sending = false;    // its gate is open
  bool capturing = false;  // the mic runs (else paused)
  std::string paused;  // why the mic is off now: THE AGENT IS LISTENING (or SPEAKING), THE
                       // RECORDER IS ON, AN ALARM IS RINGING, MUSIC IS PLAYING, A MESSAGE IS
                       // RECORDING, STARTING, THE MIC WON'T START; "" while it runs
  std::vector<MicSound> sent;  // newest first, the last 32
  std::vector<int8_t> bars;    // kHistoryBars, oldest first, the last the bar now: dBFS or kNoBar
  uint32_t delayMs = 0;  // mouth to ear, to a board that plays it: the pre-roll, the capture and
                         // the other board's buffers (computed from what was measured)
  uint32_t framesSent = 0, framesMuted = 0, sounds = 0, stillHeres = 0, histories = 0;
  // While the speaker played: frames captured, those louder than the echo could be, the loudest
  // frame and the echo level it met (the echo gate's working margin).
  uint32_t playFrames = 0, playOver = 0;
  float playPeakDb = -96, playPeakGateDb = -96;
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
int micThreshold();  // NVS, kMicDefaultDb at first
void setMicThreshold(int db);  // snapped to kMicStepDb and kept; a mic that's on uses it at once

// --- Open mics heard ----------------------------------------------------------------------------

enum class Link : uint8_t {
  Quiet,  // on, nothing over its threshold
  Sound,  // its voice comes
  Lost,   // nothing from it for kLostMs and no off: LOST (the alarm, if this board heard it)
};

// An event of a mic, as this board saw it.
struct MicEvent {
  bool lost = false;    // else a sound
  time_t at = 0;        // its start, UTC; 0 while the clock isn't set
  uint32_t agoS = 0;
  uint32_t ms = 0;      // so far, while live
  bool loud = false;    // a sound: LOUD, else SOFT
  float peakDb = -96;
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
  uint32_t conversation = 0;
  Link link = Link::Quiet;
  uint32_t linkMs = 0;   // in this state for (LOST: since its last packet)
  uint32_t onS = 0;      // on for, as it says (and counted on here)
  time_t since = 0;      // its session's start, UTC; 0 while the clock isn't set
  int thresholdDb = kMicDefaultDb;
  float levelDb = -96;   // SOUND: what plays (200 ms); else its loudest since its last still-here
  bool sending = false;  // its gate is open
  bool paused = false;   // something else has its mic for now
  bool alarm = false;    // LOST's alarm sounds for it
  int battery = -1;      // percent, -1 none
  bool usb = false;
  uint32_t heardAgoMs = 0;
  Route route = Route::Away;
  int hops = 0;
  int rssi = 0;
  uint32_t delayMs = 0;  // from its mic to this speaker: its pre-roll and the buffers (computed)
  std::vector<int8_t> bars;  // as Mic::bars, from its board (asked for when this board came late)
  std::vector<MicEvent> events;  // newest first: sounds and LOSTs since this board heard it on
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
// Plays the last 60 s heard again, over any voice that plays. False (status().error) if there's
// none, or a message is recording.
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
// stop ringing), and the mic opens with the cue (an open mic pauses meanwhile). Frames go out as
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
bool callChirp();       // a chirp as a mic session starts playing here; on by default
void setCallChirp(bool on);

// How this board's mic is keyed on the screens (apps/comms_ui.h has both key maps): Sticky, ●
// switches it on and off; Push, it's on while HOLD BOOT is held. The card's mesh_mic line
// (settings::meshMic()), unless the dev override says otherwise. The engine doesn't use it: a
// mic is on or off either way, and no packet switches one on.
enum class MicMode : uint8_t { Sticky, Push };
MicMode micMode();

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

// Groups, boards, state, channel and rate, the boards heard straight and their links,
// conversations, the mic, open mics, rings, messages and counters: sent, received (by rate),
// dropped by reason, relayed and not; the duplex path's.
void dump(Print &out);
// A test of the parts that need no radio: HKDF and AES-GCM, the replay window, ADPCM, the
// packet format (the mic's types too), the seen-cache, a message's reassembly with frames lost and
// doubled, conversations on a made-up clock, the gate (pre-roll, hang, the echo held back, its
// drain at a mic's off), the history, an open mic heard (QUIET, SOUND, LOST, its sessions) and the
// mixer. Prints PASS or FAIL a check; true if all passed.
bool selfTest(Print &out);

// Dev overrides (devtools.cpp's !commsgroup, !commschannel and !commsmicmode), kept in NVS: a test
// group from a secret, on top of the card's (the card's mesh_group lines are the real way), a
// channel in place of mesh_channel, and the mic's mode in place of mesh_mic. An empty secret or
// channel 0 removes them. They apply at once.
void setTestGroup(const std::string &secret, const std::string &name);
void setTestChannel(int channel);
// !commsmicmode: the mic's mode in place of the card's mesh_mic line, kept in NVS (set false: the
// card's line again). testMicMode() says if one is set.
void setTestMicMode(bool set, MicMode mode = MicMode::Sticky);
bool testMicMode();
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
// as a duplicate) or the first voice frame of its last sound (true: dropped as replayed, once the
// seen-cache has forgotten it). For showing those drops in !comms.
void resend(bool firstVoice);
// Testing (!commsringvanish, !commsdropmsg): the ring going stops sending ring packets, with no
// cancel, as if the dialer had gone out of range; and every nth message frame that comes is
// dropped as if lost (0: none), so the asking again shows.
void ringVanish();
void dropMessageFrames(int nth);
// Testing the mic (!commsvanish, !commstone, !commslong): this board sends nothing more, its off
// neither, as if it had gone out of range (false: it's back); for ms, a 1 kHz tone peaking at db
// dBFS is added to what the mic hears (nobody is there to speak: it's the voice, over the room
// and the speaker's echo); a mic heard counts as on long after ms instead of kLongMicMs (0:
// kLongMicMs again).
void micVanish(bool on);
void micTone(uint32_t ms, int db);
void setLongMicMs(uint32_t ms);
// The loopback test (!commsecho): with the mic off and nothing playing, a 1 kHz tone at levelDb
// (into the DAC, before the volume) for ms, through audio's duplex path, and what the mic made of
// it. The echo delay measured is used from then on, until a restart (the coupling stays
// kEchoCouplingDb: the test reports it). Waits (up to ms + 4 s).
struct EchoTest {
  bool ok = false;
  std::string error;
  int volume = 0;
  float playedDb = -96, micDb = -96, noiseDb = -96, couplingDb = 0;
  int delayMs = -1;  // from the tone written to the speaker to the mic hearing it
};
EchoTest echoTest(float levelDb, uint32_t ms);

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
