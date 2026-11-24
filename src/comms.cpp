#include "comms.h"

#if INKDECK_COMMS  // the whole file (features.h)

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_mac.h>
#include <esp_now.h>
#include <esp_random.h>
#include <esp_sleep.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <driver/gpio.h>
#include <stddef.h>
#include <sys/time.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <mbedtls/platform_util.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <new>

#include "adpcm.h"
#include "audio.h"
#include "board_pins.h"
#include "comms_calls.h"
#include "comms_crypto.h"
#include "comms_mesh.h"
#include "comms_mic.h"
#include "comms_reach.h"
#include "comms_store.h"
#include "comms_wire.h"
#include "kv_store.h"
#include "pmic.h"
#include "power.h"
#include "rtc.h"
#include "screen.h"
#include "text.h"
#include "wlan.h"

namespace comms {
namespace {

using namespace wire;

// NVS ("comms"): id, the board's; boot, the last boot number; active, the active group's id;
// norelay and nochirp, 1 when those are off; via0..via3, a group's via changed on the board
// (the card line's hash with the via in its low 2 bits); devgrp and devchan, the dev overrides;
// devrange, the dev override of mesh_range (0 none, else the setting plus 1).
constexpr const char *kNs = "comms";

constexpr uint32_t kPresenceMs = 30000;   // each group hears where the board is this often
constexpr uint32_t kAwayMs = 95000;       // three presences missed: away
constexpr uint32_t kLingerMs = 10000;     // listening on after the last hold or unplugging
constexpr uint32_t kUsbPollMs = 5000;
constexpr uint32_t kHelloAnswerGapMs = 5000;
constexpr uint32_t kTickMs = 1000;
constexpr uint32_t kSleepCheckMs = 250;
constexpr uint32_t kChannelCheckMs = 1000;
constexpr uint32_t kCallBumpMs = 250;
constexpr uint32_t kLinksMs = 5000;       // a links packet this often (+ up to 1 s), comms_mesh.h
constexpr size_t kRxDepth = 96;     // frames from the radio waiting for the task (PSRAM)
// Relays and delayed packets waiting to go: a mic switched off queues its 3 offs (stopMic()), a
// message its end, with room left for relays meanwhile.
constexpr size_t kPending = 24;
constexpr size_t kLastFrames = 3000;  // PLAY LAST keeps 60 s
constexpr uint8_t kFlagHello = 0x80;  // in a presence: answer with yours
// Conversations, rings and messages (docs/comms-protocol.md).
constexpr uint32_t kConvPresenceMs = 4000;  // in a conversation, presence this often (+ 0-1 s)
constexpr uint32_t kRingEveryMs = 1000;     // a ring packet
constexpr uint32_t kRingLostMs = 5000;      // none for this long: the dialer has gone
constexpr uint32_t kRingChirpMs = 3000;     // a ringing board's chirp
constexpr uint32_t kRingingFreshMs = 3500;  // a board's "ringing" counts this long
constexpr uint8_t kRingingOn = 1, kRingingStopped = 2;  // a ringing packet's state
constexpr uint8_t kEndGaveUp = 1, kEndNoAnswer = 2, kEndMessage = 3;  // a ring end's reason
constexpr uint8_t kMsgCancelled = 0x01;     // message end flags
constexpr uint8_t kAskComplete = 0x01, kAskNoEnd = 0x02;  // message ask flags
constexpr uint32_t kMessageKeepMs = 5 * 60000;  // the sender keeps a message for asks
constexpr uint32_t kResendGapMs = 10;           // frames asked for again: 100 a second
constexpr uint32_t kAskQuietMs = 700;    // frames stopped coming this long: ask for what's missing
constexpr uint32_t kNoEndMs = 2000;      // no frame and no end for this long: ask for the end
constexpr int kStaleAsks = 6;  // asks in a row that brought nothing: kept as it is
constexpr uint32_t kGiveUpMs = 3 * 60000;  // a message still not whole is kept as it is
// An ask's map of the frames missing: a bit a frame, as many as fit the 250 bytes.
constexpr size_t kAskMapBytes = kMaxBody - (4 + 4 + 4 + 1 + 2 + 2 + 2);  // 195
constexpr size_t kAskMapFrames = kAskMapBytes * 8;                        // 1560
constexpr size_t kInbound = 2;              // messages coming in at once
constexpr size_t kMessageRam = 1536 * 1024; // kept in PSRAM without a card: about 3 minutes
constexpr time_t kMessageDayS = 24 * 3600;  // a played message is kept this long
constexpr int kMaxNameLen = 16;
// The mic (docs/comms-protocol.md, comms_mic.h).
constexpr size_t kMaxOpenMics = 12;          // other boards' mics kept, all groups together
constexpr uint32_t kResumeAfterMs = 400;     // the mic, once what paused it is over
constexpr uint32_t kMicRetryMs = 2000;       // after the mic wouldn't start
// Frames dropped as the mic starts: after the duplex path starts, 100 ms of zeros, then a thump
// from -37 dBFS falling to the room by about 200 ms (measured 2026-11-23 and 24); and the cue,
// 150 ms written into the speaker from the start, which the mic hears 60 ms later (the loopback:
// it's over at the mic by 210 ms). 280 ms covers both with 70 ms for the room's tail: nothing of
// either goes on the air, nor into a bar's peak.
constexpr uint32_t kMicSettleFrames = kMicSettleMs / kFrameMs;
constexpr uint32_t kDuplexIdleMs = 1500;     // with the mic off, the duplex path goes this long
                                             // after the last voice
constexpr uint32_t kMixAheadMs = 30;         // queued for the speaker ahead of what plays
constexpr uint32_t kPlayingMs = 300;         // a board's voice played this recently plays now
constexpr uint32_t kAlarmEveryMs = 2500;     // LOST's alarm
constexpr uint32_t kHistoryAskMs = 2000;
constexpr int kHistoryAsks = 3;
constexpr uint32_t kLostKeepMs = 2 * 3600000u;  // a LOST mic is forgotten after this, at most
// The delay from a mic to the other board's speaker, as measured 2026-11-23
// (two boards, !commstone): the capture (a 10 ms DMA block, the 20 ms frame filling, comms's
// turn), the air and both radio tasks (half a ping's 9 ms), the mixer's jitter buffer (2 to 30 ms
// to the second frame), what the mixer keeps queued (25 ms on average), and the speaker's DMA
// (the loopback's 60 ms less the mic's 10).
constexpr uint32_t kCaptureMs = 22, kAirMs = 5, kJitterMs = 20, kQueueMs = 25, kSpeakerMs = 50;
static_assert(HeardMic::kLostMs == kLostMs && HeardMic::kCutMs == kCutMs,
              "comms.h's mic constants are comms_mic.h's");
static_assert(MicHistory::kBars == kHistoryBars && MicHistory::kBarMs == kHistoryBarMs,
              "comms.h's history is comms_mic.h's");
static_assert(Mixer::kSources == kMaxVoices, "comms.h's voices at once are the mixer's");
static_assert(kHeader + 4 + 4 + 4 + 2 + 2 + 1 + 1 + 2 + 1 + 1 + kFrameBytes + kTag <= kMaxFrame,
              "a voice packet must fit ESP-NOW v1's 250 bytes");
const uint8_t kBroadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static_assert(kHeader + 4 + 4 + 4 + 2 + 2 + 1 + 1 + kFrameBytes + kTag <= kMaxFrame,
              "a message frame must fit ESP-NOW v1's 250 bytes");
static_assert(kHeader + 4 + 4 + 4 + 1 + 2 + 2 + 2 + kAskMapBytes + kTag <= kMaxFrame,
              "a message ask must fit ESP-NOW v1's 250 bytes");

enum Drop : uint8_t {
  kDropMalformed,
  kDropQueueFull,
  kDropOwn,
  kDropDuplicate,
  kDropOtherGroup,
  kDropBadTag,
  kDropReplay,
  kDropStale,
  kDropViaNet,
  kDropNotForUs,
  kDropRecording,
  kDropLate,
  kDropRelayFull,
  kDropNotInConversation,
  kDropMessageTest,
  kDropUnknownType,
  kDropNoVoiceRoom,
  kDropCantPlay,
  kDropDeaf,
  kDropCount
};
const char *const kDropNames[kDropCount] = {
    "malformed", "queue full", "own (relayed back)", "duplicate", "other group (not relayed)",
    "bad tag",   "replayed",   "stale",              "via net",   "not for us",
    "while recording a message", "late frame", "relay queue full", "not in the conversation",
    "message frame (dropped to test)", "a later type (relayed only)",
    "voice: more boards at once than the mixer plays", "voice: the speaker is busy",
    "deaf to its sender (!commsdeaf)"};

struct Counters {
  std::atomic<uint32_t> sent{0}, sendFailed{0}, txFailed{0}, received{0}, handled{0}, relayed{0};
  std::atomic<uint32_t> relayedUnread{0};  // of relayed: groups this board isn't in
  // Voice and message frames (comms_mesh.h): relayed, and not relayed since no board this one
  // hears straight needed them. Links packets sent and taken.
  std::atomic<uint32_t> relayedAudio{0}, audioNotNeeded{0}, linksSent{0}, linksHeard{0};
  std::atomic<uint32_t> dropped[kDropCount];
};

struct RxFrame {
  uint8_t mac[6];
  int8_t rssi;
  int8_t noise;  // the radio's noise floor as it came, dBm
  uint8_t rate;  // how it came: rx_ctrl's sig_mode in the top 3 bits, its rate in the low 5
  uint8_t len;
  uint8_t data[kMaxFrame];
};

struct BoardRec {
  uint32_t id = 0;
  std::string name;
  uint8_t flags = 0;
  int battery = -1;
  uint32_t heardMs = 0;    // millis(); 0: never (every time stamp here is made non-zero)
  time_t heardAt = 0;
  uint32_t directMs = 0;   // its last packet straight from it
  int rssi = 0;
  uint8_t rxRate = 0xFF;   // how that packet came (RxFrame::rate); 0xFF none
  int noise = 0;
  uint32_t relayedMs = 0;  // its last packet through relays
  int hops = 0;
  uint32_t relay = 0;
  time_t talkedAt = 0;
  Route shown = Route::Away;  // as the last generation had it
  int shownRssi = 0;
  int shownHops = 0;
  uint32_t pollMs = 0;  // asleep: its last poll (a presence since clears it); 0 none
  int pollS = 0;        // the seconds between its polls
};

struct GroupRec {
  std::string name, hub;
  uint32_t line = 0;  // what the via change in NVS belongs to
  bool fromToken = false;
  uint32_t id = 0;
  uint8_t rendezvous[crypto::kRendezvous] = {};
  Via cardVia = Via::Both, via = Via::Both;
  crypto::Sealer *sealer = nullptr;
  std::vector<BoardRec> boards;
  uint32_t presenceDue = 0;
  bool hello = true;       // the next presence asks the others to answer
  uint32_t answerDue = 0;  // answering a hello; 0: none due
  uint32_t answeredMs = 0;
  ConvState conv;          // its conversation (under mu)
  uint32_t convSeen = 0;   // the last conversation status().conversationsSeen counted
};

struct CallRec {
  Kind kind = Kind::Voice;
  time_t at = 0;
  uint32_t startMs = 0;  // 0 for a message kept from before this boot
  int group = 0;
  bool outgoing = false;
  uint32_t from = 0, to = 0;
  uint16_t call = 0;  // Voice: the mic's session; Ring: the ring's number
  uint32_t ms = 0;
  bool live = true;
  int hops = 0;
  uint32_t relay = 0;
  int rssi = 0;
  bool played = false, heard = false;
  bool linkLost = false;  // Voice: it ended LOST
  bool cutOff = false;    // Voice, incoming: held, it went silent with no off
  uint32_t lost = 0;      // frames that never came, covered
  std::vector<uint32_t> heardBy;  // a message: the boards that have it whole
  uint32_t conv = 0;      // Voice: its conversation
  bool answered = false;  // Ring
  bool missed = false;    // Ring, incoming: it rang out here, nobody answering or ignoring it
  bool complete = true;   // Message
  uint32_t message = 0;   // Message: its id in messages()
};

// PLAY LAST or a message, played into the mix a frame at a time.
struct Replay {
  bool on = false;
  const std::vector<uint8_t> *src = nullptr;  // replayAudio (PLAY LAST) or msgAudio (a message)
  uint32_t message = 0;  // the message playing, 0 for PLAY LAST
  int group = 0;
  uint32_t from = 0;
  size_t pos = 0;  // into src, in stored frames
  uint32_t frames = 0;
  bool held = false;
  Concealer conceal;
};

struct Pending {
  bool used = false;
  bool relay = false;
  bool unread = false;  // a relay for a group this board isn't in
  bool audio = false;   // a relayed voice or message frame
  uint32_t due = 0;
  uint8_t len = 0;
  uint8_t data[kMaxFrame];
};

struct MacId {
  uint8_t mac[6] = {};
  uint32_t id = 0;
  uint32_t used = 0;
};

struct Music {
  bool pending = false;
  std::vector<std::string> list;
  uint32_t ms = 0;
};

// This board's ring, and the message it leaves.
struct Dialing {
  DialState state = DialState::None;
  int group = 0;
  uint32_t target = 0;
  uint16_t ring = 0;
  uint32_t conv = 0;  // the conversation an answer puts the boards in
  uint32_t startMs = 0, nextSendMs = 0, endMs = 0;
  bool vanished = false;  // !commsringvanish: ring packets stop, no cancel
  uint32_t answeredBy = 0;
  struct Ack {
    uint32_t id = 0, ms = 0;
    uint8_t state = 0;
  } acks[kMaxBoards];
  int shownRinging = 0;
};

// A ring on this board.
struct Ringing {
  bool on = false;
  int group = 0;
  uint32_t from = 0;
  uint16_t ring = 0;
  uint32_t conv = 0;
  bool toAll = true;
  uint32_t startMs = 0, lastMs = 0, nextChirpMs = 0;
  time_t at = 0;
  RingEnd ended = RingEnd::None;
  uint32_t doneFrom = 0;  // the last ring that ended here: its stray packets don't ring again
  uint16_t doneRing = 0;
};

// The message this board leaves: sent as it's recorded, then kept a few minutes for the boards
// that ask for frames again.
struct Outbound {
  bool recording = false, sent = false;
  int group = 0;
  uint32_t to = 0, msg = 0;
  uint16_t ring = 0;
  uint32_t startMs = 0, sentMs = 0;
  adpcm::State enc;
  size_t fillBytes = 0;
  int16_t pcm[kFrameSamples];
  std::vector<uint8_t> audio;   // kStoredFrame a frame
  uint16_t frames = 0;
  uint32_t sum = 0;
  std::vector<uint8_t> resend;  // a bit a frame to send again
  bool resending = false;
  uint32_t nextResendMs = 0;
  std::vector<uint32_t> delivered;  // boards that said they have it whole
};

// A message coming in.
struct Inbound {
  bool used = false;
  int group = 0;
  uint32_t sender = 0, msg = 0, to = 0;
  uint32_t local = 0;  // its id in messages()
  MessageAssembly a;
  uint32_t firstMs = 0, lastMs = 0, nextAskMs = 0;
  int asks = 0;
  int stale = 0;           // asks in a row after which no frame came
  uint16_t haveAtAsk = 0;
  uint16_t shownHave = 0;
};

// A message kept (under mu).
struct MsgRec {
  uint32_t local = 0;
  store::Entry e;
  int group = -1;  // the index of e.group; -1 while that group isn't configured
  bool receiving = false;
  bool onCard = false;
  std::vector<uint8_t> audio;  // in PSRAM, when not on the card
  uint32_t arrivedMs = 0;
};

// This board's mic.
struct MicRec {
  bool on = false;
  int group = 0;
  uint32_t target = 0;
  uint32_t conv = 0;     // the group's conversation, as of the last tick
  bool opened = false;   // its session opened the conversation
  bool latched = false;  // held transmit: it stays on after the hold (comms.h latchMic())
  uint16_t session = 0;
  uint32_t startMs = 0;
  time_t since = 0;
  MicHistory hist;
  bool capturing = false;  // the duplex path's mic blocks come: it transmits
  const char *paused = "STARTING";  // why it isn't capturing (a literal)
  uint32_t resumeMs = 0;   // it may capture again from this
  uint32_t retryMs = 0;
  bool cue = false;        // the cue plays once it captures
  adpcm::State enc;
  size_t fill = 0;         // samples in pcm
  int16_t pcm[kFrameSamples];
  uint32_t settle = 0;     // frames still to drop (the thump, the cue)
  int8_t settlePeaks[16];  // their peaks, dBFS, for the first frame's log line
  uint8_t settled = 0;
  uint16_t frame = 0;      // within the session
  float peaks[10] = {-96, -96, -96, -96, -96, -96, -96, -96, -96, -96};
  int peakAt = 0;
  float level = -96, shownLevel = -96;
  uint32_t shownMs = 0;
  uint8_t peakSince = kNoLevel;  // the loudest frame since the last still-here
  uint32_t nextStillMs = 0;
  uint32_t framesSent = 0, stillHeres = 0, histories = 0;
  uint32_t firstFrameMs = 0;  // micOn() to its first frame sent (0: none yet)
  bool vanished = false;         // !commsvanish: nothing goes out
  uint32_t toneUntilMs = 0;      // !commstone: the mic's input is a tone until then
  float toneAmp = 0;
  uint32_t tonePhase = 0;
};

// Another board's mic that's on, or LOST (under mu).
struct OpenMicRec {
  int group = 0;
  uint32_t id = 0;
  uint32_t boot = 0;     // its board's boot when the mic was heard on
  uint32_t lastSeq = 0;  // the seq of its last mic packet (a presence older than it says nothing)
  HeardMic st;
  MicHistory hist;
  MicStill last;         // its last still-here
  bool haveStill = false;
  uint32_t target = 0, conv = 0;
  bool opened = false;
  bool latched = false;
  bool haveHistory = false;
  int historyAsks = 0;
  uint32_t nextAskMs = 0;
  uint32_t playedMs = 0;       // its voice last came out of the speaker
  bool logged = false;         // its session has its log entry
  uint32_t frames = 0, lost = 0, late = 0, histories = 0;
  float peaks[10] = {-96, -96, -96, -96, -96, -96, -96, -96, -96, -96};
  int peakAt = 0;
  float level = -96, shownLevel = -96;
  uint32_t shownMs = 0;
  int hops = 0;
  uint32_t relay = 0;
  int rssi = 0;
};

enum class Cmd : uint8_t {
  Wake, MicOn, MicOff, MicLatch, MicVanish, MicTone, AckLost, PlayLast, StopPlay,
  Ping, Reload, Resend, Ring, RingStop, Answer, Ignore, Join, Leave, MessageStart, MessageSend,
  MessageCancel, PlayMessage, RingVanish, PowerOff, Range, Fetch,
};
struct Command {
  Cmd cmd;
  int group;
  uint32_t arg;
};

// Everything the task keeps. Allocated with new, so it lives in PSRAM (psram_new.cpp).
struct Core {
  // Shared with the UI, under mu.
  std::mutex mu;
  std::vector<GroupRec> groups;
  int active = 0;
  std::vector<CallRec> log;  // oldest first
  LastCall last;
  std::vector<Pong> pongs;
  Status pub;
  Dial pubDial;
  Ring pubRing;
  Mic pubMic;
  std::vector<MsgRec> msgs;  // oldest first
  uint32_t msgCounter = 0;
  bool relayOn = true, chirpOn = true;
  int devChannel = 0;
  std::vector<OpenMicRec> mics;  // other boards' mics, on or LOST
  uint32_t micWantTarget = 0;  // micOn()'s, for the task

  // The task's own.
  uint32_t id = 0, boot = 0, seq = 0;
  std::string name;
  uint16_t ringCounter = 0;
  uint32_t pingId = 0;
  Dialing dialing;
  uint32_t dialCount = 0;
  Ringing ringing;
  uint32_t ringsSeen = 0;
  RingWatch rings;    // rings on in the groups, for sleeping boards' polls
  WokenHold woken;    // this board woke for comms (reachSleep()): how long it stays up
  uint32_t fetchSender = 0, fetchMsg = 0, fetchTo = 0;  // the message it woke for
  int fetchGroup = -1;  // asked for once listening; -1 none
  uint8_t homeChannel = 0;  // the channel boards were last heard on directly, as kept in NVS
  uint32_t conversationsSeen = 0;
  int conversationGroup = 0;
  uint32_t messagesIn = 0;
  Outbound out;
  Inbound inbound[kInbound];
  std::vector<uint8_t> msgAudio;  // the message playing
  SeenCache seen;
  ReplayGuard guard;
  Pending pending[kPending];
  Replay replay;
  std::vector<uint8_t> replayAudio;  // PLAY LAST's copy of heardAudio, as it was
  MacId macs[32];
  uint32_t macTick = 0;
  Music music;
  // The duplex path: comms started it, and it runs.
  bool duplexOurs = false;
  uint32_t duplexStartMs = 0, duplexRetryMs = 0, lastPlayMs = 0;
  HalfDuplex half;       // nothing plays while this board transmits (comms_mic.h)
  NoiseGate gate;        // what this board sends, its mic's frames and its messages: the room
                         // turned down,
  MicAgc agc;            // and the voice brought up
  uint32_t talkOverMs = 0, talkOverBy = 0;
  uint32_t halfDueMs = 0;  // transmitting: the mixer's next frame is due (pumpPlay())  // voice came while it transmitted: when, whose
  FirstHeard firstHeard;  // the call sheet and chirp: once a conversation
  // The mix: the voices playing, and the last 60 s heard (a ring of stored frames).
  Mixer mixer;
  int16_t mixPcm[kFrameSamples];
  int16_t replayPcm[kFrameSamples];
  audio::DuplexBlock block;
  std::vector<uint8_t> heardAudio;
  size_t heardHead = 0, heardCount = 0;  // the next frame written; frames kept
  adpcm::State heardEnc;
  uint32_t heardFrom = 0;
  int heardGroup = 0;
  uint32_t heardLastMs = 0;
  float level = -96;     // what plays: the loudest of the last 200 ms of frames
  float peaks[10] = {};
  int peakAt = 0;
  float msgLevel = -96;  // a message recording: the mic
  bool onUsb = false;
  int battery = -1;  // percent, -1 none (with onUsb, polled every 5 s)
  MicRec mic;
  uint32_t micCount = 0;
  uint16_t micSession = 0;
  bool alarm = false;      // LOST's alarm sounds for one of the mics
  uint32_t nextAlarmMs = 0;
  uint32_t longMicMs = kLongMicMs;  // !commslong
  std::string error;
  uint32_t incoming = 0;
  RxFrame rx;  // the frame being handled: too big for the task's stack
  // For !commsresend: the last frame this board sent, and the first voice frame of its last
  // sound.
  uint8_t lastSent[kMaxFrame];
  uint8_t lastSentLen = 0;
  uint8_t firstVoice[kMaxFrame];
  uint8_t firstVoiceLen = 0;
  bool markFirstVoice = false;
  // Comms frames received, by how they came (RxFrame::rate): the Wi-Fi task counts, dump() reads.
  uint32_t rxRates[256] = {};
  uint32_t txRates[64] = {};  // this board's frames sent, by the rate the driver says (onSent())
  // Relaying only when needed (comms_mesh.h): the boards heard straight and what they hear
  // (under mu), the link group's key, and the links packets going and coming.
  Neighbours links;
  crypto::Sealer *linkSealer = nullptr;
  uint32_t linkGroup = 0;
  uint32_t linksDue = 0;
  Links linksTx, linksRx;
  bool rangeLong = false;  // the rate set for this board's packets (applyRadio())
  bool rangeFailed = false;  // the rate wanted wouldn't take (long range without its protocol)
  uint32_t sleepFixes = 0; // times modem sleep was found on again (another feature) and put off
};

Core *core = nullptr;
Counters counters;
QueueHandle_t rxQueue = nullptr;
QueueHandle_t commands = nullptr;
TaskHandle_t taskHandle = nullptr;
std::atomic<int> holders{0};
std::atomic<uint32_t> gen{1};
std::atomic<bool> isListening{false};
std::atomic<bool> micWanted{false};  // a MicOn command is on its way, or the mic is on
std::atomic<bool> hasGroups{false};
std::atomic<bool> poweringOff{false};  // forceOff(): the radio stays off
std::atomic<int> dropNth{0};  // !commsdropmsg: every nth message frame that comes is dropped
std::atomic<bool> awake{false};         // keepAwake()
std::atomic<bool> powerOffSent{false};  // forceOff(): the mic's off has gone
// A full-size frame's time from esp_now_send() to its send callback, when no other frame is in
// the radio meanwhile: the air time (at the rate) plus waiting for a clear channel.
std::atomic<int> txInflight{0};
std::atomic<bool> txTiming{false};
std::atomic<uint32_t> txStartUs{0}, txUsSum{0}, txTimed{0};
std::atomic<int> devRangeMode{-1};      // devrange in NVS: -1 not read yet, 0 none, 1 normal, 2 long
std::atomic<uint32_t> deafTo{0};        // !commsdeaf: what this board's radio gets from it is dropped
std::atomic<bool> floodAll{false};      // !commsflood: every voice and message frame relayed
uint32_t dropTick = 0;
std::mutex radioMu;  // the ESP-NOW calls, against forceOff()
bool espnowUp = false;
// Reachable on battery: the sleeping loop (or !reachbench) has the radio; the task keeps off it.
std::atomic<bool> radioLent{false};
std::atomic<uint32_t> rxAtUs{0}, sentAtUs{0};  // the last frame received, the last send done

void bump() { gen.fetch_add(1, std::memory_order_relaxed); }

// x as freshly made, built where it is: `x = T()` would build a T on the task's stack first,
// and MicRec is 4 KB.
template <typename T>
void renew(T &x) {
  x.~T();
  new (&x) T();
}
void drop(Drop d) { counters.dropped[d].fetch_add(1, std::memory_order_relaxed); }

uint32_t stamp() {  // millis() made non-zero, for "0: never"
  const uint32_t m = millis();
  return m ? m : 1;
}

bool within(uint32_t at, uint32_t now, uint32_t window) { return at && now - at < window; }

time_t utcNow() { return rtc::timeValid() ? time(nullptr) : 0; }

uint32_t sentAt() { return static_cast<uint32_t>(utcNow()); }

// A packet's sentAt is close enough to this board's clock (or one of them isn't set).
bool fresh(uint32_t at) {
  const time_t now = utcNow();
  if (!at || !now) return true;
  const int64_t d = static_cast<int64_t>(now) - at;
  return d <= ReplayGuard::kFreshS && d >= -static_cast<int64_t>(ReplayGuard::kFreshS);
}

// Printable ASCII, upper case, at most max bytes.
std::string upperName(const std::string &s, size_t max = kMaxNameLen) {
  std::string out;
  for (const char c : text::toAscii(s)) {
    if (c < 0x20 || c > 0x7E) continue;
    if (out.size() >= max) break;
    out += static_cast<char>(toupper(static_cast<unsigned char>(c)));
  }
  return text::trim(out);
}

void post(Cmd cmd, int group = 0, uint32_t arg = 0, uint32_t waitMs = 50) {
  if (!commands) return;
  const Command c{cmd, group, arg};
  xQueueSend(commands, &c, pdMS_TO_TICKS(waitMs));
}

// --- The board ---------------------------------------------------------------------------------

uint32_t loadId() {
  uint32_t id = kv::loadU32(kNs, "id");
  if (!id) {
    // The RNG before Wi-Fi starts is only pseudo-random, so the chip's MAC goes in too: two
    // boards can't make the same id.
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    const uint32_t m = text::fnv1a(std::string(reinterpret_cast<const char *>(mac), 6));
    do {
      id = esp_random() ^ m;
    } while (!id);
    kv::saveU32(kNs, "id", id);
    log_i("comms: this board's id is %08lX", static_cast<unsigned long>(id));
  }
  return id;
}

// A number per boot that never repeats while the board keeps its id: the nonce's middle part.
uint32_t nextBoot() {
  uint32_t b = kv::loadU32(kNs, "boot") + 1;
  if (!b) b = 1;
  kv::saveU32(kNs, "boot", b);
  if (kv::loadU32(kNs, "boot") != b) {
    b = esp_random() | 0x80000000u;  // NVS didn't keep it: random rather than one used before
    log_w("comms: NVS won't keep the boot number; using a random one");
  }
  return b;
}

std::string makeName(uint32_t id) {
  std::string n = settings::meshName();
  if (n.empty() && settings::tailscaleName() != "inkdeck") n = settings::tailscaleName();
  if (n.empty()) {
    char b[16];
    snprintf(b, sizeof b, "INKDECK-%04X", static_cast<unsigned>(id & 0xFFFF));
    n = b;
  }
  return upperName(n);
}

int meshChannel() {
  return core && core->devChannel ? core->devChannel : settings::meshChannel();
}

// --- Groups ----------------------------------------------------------------------------------

uint32_t viaRecord(uint32_t line, Via v) { return (line & ~3u) | (static_cast<uint32_t>(v) + 1); }

void addGroup(std::vector<GroupRec> &out, const std::string &secret, bool fromToken,
              const std::string &name, Via via, const std::string &hub, uint32_t line) {
  crypto::GroupKeys k = crypto::derive(secret, fromToken);
  for (const GroupRec &g : out) {
    if (g.id == k.id) {
      log_w("comms: two groups with the same secret; the second (%s) left out", name.c_str());
      mbedtls_platform_zeroize(k.key, sizeof k.key);
      return;
    }
  }
  GroupRec g;
  g.name = upperName(name);
  g.hub = hub;
  g.line = line;
  g.fromToken = fromToken;
  g.id = k.id;
  memcpy(g.rendezvous, k.rendezvous, sizeof g.rendezvous);
  g.cardVia = g.via = via;
  g.sealer = new crypto::Sealer();
  g.sealer->setKey(k.key);
  mbedtls_platform_zeroize(k.key, sizeof k.key);
  char key[8];
  snprintf(key, sizeof key, "via%u", static_cast<unsigned>(out.size()));
  const uint32_t v = kv::loadU32(kNs, key);
  if (v && (v & ~3u) == (line & ~3u) && (v & 3u) >= 1) g.via = static_cast<Via>((v & 3u) - 1);
  out.push_back(std::move(g));
}

// The dev override (!commsgroup): "<secret>\t<name>" in NVS.
std::string devGroupRecord() {
  Preferences prefs;
  std::string v;
  if (prefs.begin(kNs, true)) {
    if (prefs.isKey("devgrp")) v = prefs.getString("devgrp", "").c_str();
    prefs.end();
  }
  return v;
}

std::vector<GroupRec> loadGroups() {
  std::vector<GroupRec> out;
  const std::vector<settings::MeshGroup> lines = settings::meshGroups();
  for (const settings::MeshGroup &l : lines) {
    const std::string name =
        l.name.empty() ? "GROUP " + std::to_string(out.size() + 1) : l.name;
    addGroup(out, l.secret, false, name, l.via, l.hub, l.line);
  }
  const std::string token = settings::agentToken();
  if (lines.empty() && !token.empty()) {
    addGroup(out, token, true, "HOME", Via::Both, "", text::fnv1a("agent_token " + token));
  }
  const std::string dev = devGroupRecord();
  const size_t tab = dev.find('\t');
  if (!dev.empty() && out.size() < static_cast<size_t>(kMaxGroups)) {
    const std::string secret = dev.substr(0, tab);
    const std::string name = tab == std::string::npos ? "TEST" : dev.substr(tab + 1);
    addGroup(out, secret, false, name.empty() ? "TEST" : name, Via::Both, "",
             text::fnv1a("devgrp " + secret));
  }
  return out;
}

void freeGroups(std::vector<GroupRec> &groups) {
  for (GroupRec &g : groups) delete g.sealer;
  groups.clear();
}

int groupIndex(uint32_t id) {
  for (size_t i = 0; i < core->groups.size(); ++i) {
    if (core->groups[i].id == id) return static_cast<int>(i);
  }
  return -1;
}

// Under mu.
BoardRec *findBoard(GroupRec &g, uint32_t id) {
  for (BoardRec &b : g.boards) {
    if (b.id == id) return &b;
  }
  return nullptr;
}

// Under mu. Makes room by forgetting the board heard from longest ago.
BoardRec &board(GroupRec &g, uint32_t id) {
  if (BoardRec *b = findBoard(g, id)) return *b;
  if (g.boards.size() >= static_cast<size_t>(kMaxBoards)) {
    auto oldest = std::min_element(g.boards.begin(), g.boards.end(),
                                   [](const BoardRec &a, const BoardRec &b) {
                                     return a.heardMs < b.heardMs;
                                   });
    g.boards.erase(oldest);
  }
  BoardRec b;
  b.id = id;
  g.boards.push_back(b);
  bump();
  return g.boards.back();
}

// Under mu: a board's name, from any group; "" if none has it.
std::string nameOf(uint32_t id) {
  if (!id) return "";
  if (id == core->id) return core->name;
  for (GroupRec &g : core->groups) {
    if (const BoardRec *b = findBoard(g, id)) {
      if (!b->name.empty()) return b->name;
    }
  }
  return "";
}

Route routeOf(const BoardRec &b, uint32_t now, int &hops) {
  hops = 0;
  if (within(b.directMs, now, kAwayMs)) return Route::Air;
  if (within(b.relayedMs, now, kAwayMs)) {
    hops = b.hops;
    return Route::Relayed;
  }
  return Route::Away;
}

// Under mu: bumps the generation if what a screen shows of the board has changed.
void noteShown(BoardRec &b, uint32_t now) {
  int hops = 0;
  const Route r = routeOf(b, now, hops);
  if (r != b.shown || hops != b.shownHops ||
      (r == Route::Air && abs(b.rssi - b.shownRssi) >= 4)) {
    b.shown = r;
    b.shownHops = hops;
    b.shownRssi = b.rssi;
    bump();
  }
}

// --- The log ---------------------------------------------------------------------------------

// Under mu. A full log loses its oldest entry, though not a message still kept.
CallRec &logCall(const CallRec &c) {
  if (core->log.size() >= static_cast<size_t>(kMaxCalls)) {
    auto victim = core->log.begin();
    for (auto it = core->log.begin(); it != core->log.end(); ++it) {
      if (it->kind != Kind::Message || it->outgoing) {
        victim = it;
        break;
      }
    }
    core->log.erase(victim);
  }
  core->log.push_back(c);
  bump();
  return core->log.back();
}

// Under mu.
CallRec *findLogged(Kind kind, uint32_t from, uint32_t key, bool outgoing) {
  for (auto it = core->log.rbegin(); it != core->log.rend(); ++it) {
    if (it->kind != kind || it->outgoing != outgoing) continue;
    if (kind == Kind::Message ? it->message == key : it->from == from && it->call == key) {
      return &*it;
    }
  }
  return nullptr;
}

// --- The radio -----------------------------------------------------------------------------------

// In the Wi-Fi task: copy and go.
void onRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  static RxFrame f;  // only the Wi-Fi task calls this, one frame at a time
  counters.received.fetch_add(1, std::memory_order_relaxed);
  rxAtUs.store(static_cast<uint32_t>(esp_timer_get_time()), std::memory_order_relaxed);
  if (len < static_cast<int>(kHeader + kTag) || len > static_cast<int>(kMaxFrame) ||
      data[0] != kMagic) {
    drop(kDropMalformed);
    return;
  }
  memcpy(f.mac, info->src_addr, 6);
  const wifi_pkt_rx_ctrl_t *rx = info->rx_ctrl;
  f.rssi = rx ? static_cast<int8_t>(rx->rssi) : 0;
  f.noise = rx ? static_cast<int8_t>(rx->noise_floor) : 0;
  f.rate = rx ? static_cast<uint8_t>((rx->sig_mode & 7) << 5 | (rx->rate & 0x1F)) : 0xFF;
  if (core) ++core->rxRates[f.rate];
  f.len = static_cast<uint8_t>(len);
  memcpy(f.data, data, len);
  if (!rxQueue || xQueueSend(rxQueue, &f, 0) != pdTRUE) drop(kDropQueueFull);
}

void onSent(const esp_now_send_info_t *info, esp_now_send_status_t status) {
  const uint32_t t = micros();
  if (txInflight.fetch_sub(1) == 1 && txTiming.exchange(false)) {
    txUsSum.fetch_add(t - txStartUs.load());
    txTimed.fetch_add(1);
  }
  if (core && info) ++core->txRates[static_cast<unsigned>(info->rate) & 63];
  sentAtUs.store(static_cast<uint32_t>(esp_timer_get_time()) | 1, std::memory_order_relaxed);
  if (status != ESP_NOW_SEND_SUCCESS) counters.txFailed.fetch_add(1, std::memory_order_relaxed);
}

bool radioSend(const uint8_t *frame, size_t len) {
  std::lock_guard<std::mutex> g(radioMu);
  if (!espnowUp) return false;
  if (frame[0] == kMagic && get32(frame + 8) == core->id) {  // one of ours, not a relay
    memcpy(core->lastSent, frame, len);
    core->lastSentLen = static_cast<uint8_t>(len);
    if (core->markFirstVoice) {  // sendVoiceFrame()'s frame 0
      core->markFirstVoice = false;
      memcpy(core->firstVoice, frame, len);
      core->firstVoiceLen = static_cast<uint8_t>(len);
    }
  }
  const bool alone = txInflight.fetch_add(1) == 0;
  if (alone && len >= 200) txStartUs = micros();
  txTiming = alone && len >= 200;  // a frame sent while another is in the radio spoils the timing
  if (esp_now_send(kBroadcast, frame, len) != ESP_OK) {
    txInflight.fetch_sub(1);
    txTiming = false;
    counters.sendFailed.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  counters.sent.fetch_add(1, std::memory_order_relaxed);
  return true;
}

// The channel the radio is on; 0 if it's off.
int radioChannel() {
  uint8_t primary = 0;
  wifi_second_chan_t second;
  if (!espnowUp || esp_wifi_get_channel(&primary, &second) != ESP_OK) return 0;
  return primary;
}

// Off a network, the radio sits on mesh_channel (a join or a scan moves it elsewhere).
void keepChannel() {
  if (WiFi.isConnected() || wlan::joining()) return;
  const int want = meshChannel();
  if (radioChannel() == want) return;
  const esp_err_t e = esp_wifi_set_channel(static_cast<uint8_t>(want), WIFI_SECOND_CHAN_NONE);
  if (e == ESP_OK) {
    log_i("comms: radio on channel %d", want);
  } else {
    log_d("comms: can't set channel %d yet (%s)", want, esp_err_to_name(e));
  }
}

// What the radio's rx_ctrl says of how a frame came (RxFrame::rate), as dump() shows it.
// 26 is how a long-range frame comes (sent at WIFI_PHY_RATE_LORA_500K, seen 2026-11-23).
const char *rateName(uint8_t r) {
  static const char *const kLegacy[32] = {
      "1M",  "2M",  "5.5M", "11M", "?4",  "2M short", "5.5M short", "11M short",
      "48M", "24M", "12M",  "6M",  "54M", "36M",      "18M",        "9M",
      "?16", "?17", "?18",  "?19", "?20", "?21",      "?22",        "?23",
      "?24", "?25", "LR",   "?27", "?28", "?29",      "?30",        "?31"};
  if (r == 0xFF) return "";
  if ((r >> 5) == 0) return kLegacy[r & 0x1F];
  static char other[16];  // dump() only (one task)
  snprintf(other, sizeof other, "mode%u/%u", static_cast<unsigned>(r >> 5),
           static_cast<unsigned>(r & 0x1F));
  return other;
}

int devRange() {
  int v = devRangeMode.load();
  if (v < 0) {
    v = static_cast<int>(kv::loadU32(kNs, "devrange"));
    devRangeMode = v;
  }
  return v;
}

bool wantLong() {
  switch (devRange()) {
    case 1: return false;
    case 2: return true;
    default: break;
  }
  return settings::meshRange() == settings::MeshRange::Long;
}

// This board's packets go at its rate: 1 Mbps, or long range at 500 kbps. The radio takes the
// long-range protocol on top of 802.11b/g/n, so it hears boards that send at the long-range rate:
// wlan sets that whenever the station comes up or joins (an ordinary access point is joined and
// talked to as before); here only while it isn't joined, since changing the protocols drops a
// connection (but for !commslrrx, which tests it). Called with radioMu held, ESP-NOW up: at the
// start, every second and when the setting changes (force).
void applyRadio(bool force, bool protoTest = false) {
  const uint8_t want = wlan::stationProtocols();
  uint8_t proto = 0;
  if (esp_wifi_get_protocol(WIFI_IF_STA, &proto) == ESP_OK && proto != want &&
      (protoTest || (!WiFi.isConnected() && !wlan::joining()))) {
    const esp_err_t e = esp_wifi_set_protocol(WIFI_IF_STA, want);
    log_i("comms: radio protocols %02X -> %02X (%s)", proto, want, esp_err_to_name(e));
    force = true;
  }
  const bool lng = wantLong();
  if (!force && lng == core->rangeLong && !core->rangeFailed) return;
  if (!force && core->rangeFailed && proto == want) return;  // tried: it waits for the protocol
  esp_now_rate_config_t rc = {};
  rc.phymode = lng ? WIFI_PHY_MODE_LR : WIFI_PHY_MODE_11B;
  rc.rate = lng ? WIFI_PHY_RATE_LORA_500K : WIFI_PHY_RATE_1M_L;
  esp_err_t e = esp_now_set_peer_rate_config(kBroadcast, &rc);
  core->rangeFailed = e != ESP_OK;
  if (e != ESP_OK && lng) {
    // The long-range rate takes only with the long-range protocol on (ESP_ERR_ESPNOW_ARG
    // without it, seen 2026-11-23): 1 Mbps until it is.
    log_w("comms: can't send at the long-range rate (%s); 1 Mbps for now", esp_err_to_name(e));
    rc.phymode = WIFI_PHY_MODE_11B;
    rc.rate = WIFI_PHY_RATE_1M_L;
    e = esp_now_set_peer_rate_config(kBroadcast, &rc);
    if (e == ESP_OK) core->rangeLong = false;
  } else if (e == ESP_OK) {
    core->rangeLong = lng;
    log_i("comms: sending at %s", lng ? "the long-range rate, 500 kbps" : "1 Mbps");
  }
  if (e != ESP_OK) log_e("comms: can't set the rate (%s)", esp_err_to_name(e));
}

bool radioStart() {
  wlan::holdRadio();
  {
    std::lock_guard<std::mutex> g(radioMu);
    esp_err_t e = esp_now_init();
    if (e == ESP_OK) e = esp_now_register_recv_cb(onRecv);
    if (e == ESP_OK) e = esp_now_register_send_cb(onSent);
    if (e == ESP_OK) {
      esp_now_peer_info_t peer = {};
      memcpy(peer.peer_addr, kBroadcast, 6);
      peer.channel = 0;  // whichever the radio is on
      peer.ifidx = WIFI_IF_STA;
      peer.encrypt = false;
      e = esp_now_add_peer(&peer);
    }
    if (e == ESP_OK) applyRadio(true);
    if (e != ESP_OK) {
      log_e("comms: ESP-NOW won't start (%s)", esp_err_to_name(e));
      esp_now_deinit();
      wlan::releaseRadio();
      return false;
    }
    espnowUp = true;
  }
  WiFi.setSleep(false);  // modem sleep would sleep through other boards' packets
  keepChannel();
  isListening = true;
  log_i("comms: listening on channel %d as %s (%08lX)", radioChannel(), core->name.c_str(),
        static_cast<unsigned long>(core->id));
  return true;
}

void radioStop() {
  {
    std::lock_guard<std::mutex> g(radioMu);
    if (espnowUp) {
      esp_now_unregister_recv_cb();
      esp_now_unregister_send_cb();
      esp_now_deinit();
      espnowUp = false;
    }
  }
  isListening = false;
  WiFi.setSleep(true);  // as the other users leave it
  wlan::releaseRadio();
  if (rxQueue) xQueueReset(rxQueue);
  for (Pending &p : core->pending) p.used = false;
  log_i("comms: not listening");
}

bool queuePending(const uint8_t *frame, size_t len, uint32_t delayMs, bool relay,
                  bool unread = false, bool audio = false) {
  for (Pending &p : core->pending) {
    if (p.used) continue;
    p.used = true;
    p.relay = relay;
    p.unread = unread;
    p.audio = audio;
    p.due = millis() + delayMs;
    p.len = static_cast<uint8_t>(len);
    memcpy(p.data, frame, len);
    return true;
  }
  return false;
}

// What's due, in the order it's due (not the slots'): a mic's last frames before its off.
void sendPending(uint32_t now) {
  for (;;) {
    Pending *next = nullptr;
    for (Pending &p : core->pending) {
      if (!p.used || static_cast<int32_t>(now - p.due) < 0) continue;
      if (!next || static_cast<int32_t>(p.due - next->due) < 0) next = &p;
    }
    if (!next) return;
    next->used = false;
    if (radioSend(next->data, next->len) && next->relay) {
      counters.relayed.fetch_add(1);
      if (next->unread) counters.relayedUnread.fetch_add(1);
      if (next->audio) counters.relayedAudio.fetch_add(1);
    }
  }
}

// Seals and sends one of this board's packets in group g (now, or after delayMs). False if the
// group doesn't go over the air or the radio is off.
bool sendOwn(GroupRec &g, uint8_t type, const uint8_t *body, size_t len, uint32_t delayMs = 0) {
  if (g.via == Via::Net || !espnowUp) return false;
  uint8_t f[kMaxFrame];
  Header h;
  h.type = type;
  h.ttl = kTtl;
  h.group = g.id;
  h.sender = core->id;
  h.boot = core->boot;
  h.seq = ++core->seq;
  putHeader(h, f);
  if (len > kMaxBody) return false;
  memcpy(f + kHeader, body, len);
  const size_t n = g.sealer->seal(f, len);
  if (!n) return false;
  if (delayMs) return queuePending(f, n, delayMs, false);
  return radioSend(f, n);
}

void sendPresence(GroupRec &g, bool hello) {
  uint8_t body[8 + kMaxNameLen + 9 + 4 * ConvState::kMaxMembers];
  Writer w(body, sizeof body);
  w.u32(sentAt());
  bool relay;
  // The conversation this board is in: its id, how long it has been on, since its last word,
  // and the other boards in it that this one has heard say so themselves.
  uint32_t conv = 0, ageS = 0, quietS = 0;
  uint32_t members[ConvState::kMaxMembers];
  size_t n = 0;
  {
    std::lock_guard<std::mutex> l(core->mu);
    relay = core->relayOn;
    const ConvState &c = g.conv;
    if (c.on() && c.mine()) {
      const uint32_t now = millis();
      conv = c.id();
      ageS = std::min<uint32_t>((now - c.startMs()) / 1000, 0xFFFF);
      quietS = std::min<uint32_t>((now - c.lastWordMs()) / 1000, 0xFFFF);
      for (size_t i = 0; i < c.count(); ++i) {
        if (!c.member(i).listed) members[n++] = c.member(i).id;
      }
    }
  }
  w.u8(static_cast<uint8_t>((core->onUsb ? kFlagUsb : 0) | (relay ? kFlagRelay : 0) |
                            (core->mic.on ? kFlagMic : 0) |
                            (core->rangeLong ? kFlagLong : 0) | (hello ? kFlagHello : 0)));
  const pmic::Status p = pmic::status();
  w.u8(p.ok && p.battery && p.percent >= 0 ? static_cast<uint8_t>(p.percent) : 0xFF);
  w.u8(static_cast<uint8_t>(core->name.size()));
  w.bytes(core->name.data(), core->name.size());
  w.u32(conv);
  if (conv) {
    w.u16(static_cast<uint16_t>(ageS));
    w.u16(static_cast<uint16_t>(quietS));
    w.u8(static_cast<uint8_t>(n));
    for (size_t i = 0; i < n; ++i) w.u32(members[i]);
  }
  sendOwn(g, kPresence, body, w.size());
}

// The group hears this board's presence within a moment (a conversation joined or left).
void presenceSoon(GroupRec &g) { g.presenceDue = millis() + 20 + esp_random() % 100; }

// The links packet (comms_mesh.h): the boards this one hears straight and how well, in the link
// group, ttl 0 (never relayed), every kLinksMs while it listens. Every board reads it, whatever
// its groups: it's how a board knows whether passing a voice frame on helps anyone.
void sendLinks() {
  if (!espnowUp || !core->linkSealer) return;
  Links &l = core->linksTx;
  {
    std::lock_guard<std::mutex> lk(core->mu);
    core->links.mine(l, stamp());
    l.flags = static_cast<uint8_t>((core->relayOn ? kLinksRelays : 0) |
                                   (core->rangeLong ? kLinksLong : 0));
  }
  uint8_t f[kMaxFrame];
  Header h;
  h.type = kExt;
  h.ttl = 0;
  h.group = core->linkGroup;
  h.sender = core->id;
  h.boot = core->boot;
  h.seq = ++core->seq;
  putHeader(h, f);
  const size_t len = putLinks(f + kHeader, kMaxBody, sentAt(), l);
  if (!len) return;
  const size_t n = core->linkSealer->seal(f, len);
  if (n && radioSend(f, n)) counters.linksSent.fetch_add(1, std::memory_order_relaxed);
}

// --- Audio -------------------------------------------------------------------------------------

// Why comms can't have the speaker and the mic now, or nullptr: something voice doesn't play over
// (and that takes the mic from an open mic).
const char *duplexBlocked() {
  if (core->out.recording) return "A MESSAGE IS RECORDING";
  if (audio::ringing()) return "AN ALARM IS RINGING";
  if (audio::recording()) return "THE RECORDER IS ON";
  if (audio::capturing()) return "THE AGENT IS LISTENING";
  if (audio::streaming()) return "THE AGENT IS SPEAKING";
  return nullptr;
}

// A ring's chirp, or a message's, may sound now: this board's mic is off (an open mic doesn't
// chirp: it's in the conversation, and a room with someone asleep in it stays quiet), and nothing
// that voice doesn't play over is on. Over voice and music it plays (audio mixes it in).
bool canChirp() { return !core->mic.on && !duplexBlocked(); }

// Music that voice stops plays on afterwards.
void rememberMusic() {
  if (core->music.pending || !audio::playing()) return;
  const audio::Status s = audio::status();
  core->music.list = audio::queue(s.track, 64);
  core->music.ms = s.elapsedMs;
  core->music.pending = !core->music.list.empty();
}

void resumeMusicWhenFree() {
  Music &m = core->music;
  if (!m.pending || core->mic.on || core->duplexOurs || core->replay.on) return;
  if (core->mixer.active() || audio::streaming() || audio::capturing()) return;
  m.pending = false;
  if (audio::active() || audio::recording() || audio::ringing()) return;  // something else began
  audio::play(std::move(m.list), 0, m.ms);
  m.list.clear();
}

float peakDb(const int16_t *pcm, size_t n) {
  int peak = 0;
  for (size_t i = 0; i < n; ++i) peak = std::max(peak, abs(static_cast<int>(pcm[i])));
  return peak > 0 ? 20 * log10f(peak / 32768.0f) : -96;
}

void notePlayedLevel(float db) {
  core->peaks[core->peakAt] = db;
  core->peakAt = (core->peakAt + 1) % 10;
  float m = -96;
  for (const float p : core->peaks) m = std::max(m, p);
  core->level = m;
}

void resetLevel() {
  for (float &p : core->peaks) p = -96;
  core->level = -96;
}

void setError(const char *why) {
  std::lock_guard<std::mutex> l(core->mu);
  core->error = why;
  bump();
}

// The speaker and the mic, for comms: audio's duplex path, started if it isn't (music that plays
// stops, to play on later). False if something voice doesn't play over has the port.
bool ensureDuplex(uint32_t now) {
  if (core->duplexOurs && audio::duplexOn()) return true;
  core->duplexOurs = false;  // taken, if it was ours
  if (duplexBlocked() || static_cast<int32_t>(now - core->duplexRetryMs) < 0) return false;
  rememberMusic();
  if (!audio::duplexStart()) {
    core->duplexRetryMs = now + kMicRetryMs;
    log_w("comms: the duplex path won't start");
    return false;
  }
  core->duplexOurs = true;
  core->duplexStartMs = core->lastPlayMs = now;
  return true;
}

// --- Stored frames, PLAY LAST and messages ------------------------------------------------------

// Decodes one stored frame (or a lost one, as silence) into pcm; returns the samples.
size_t decodeStored(const uint8_t *f, int16_t *pcm) {
  const uint8_t bytes = std::min<uint8_t>(f[3], kFrameBytes);
  if (!bytes) {
    memset(pcm, 0, kFrameSamples * sizeof(int16_t));
    return kFrameSamples;
  }
  adpcm::State st;
  st.predictor = static_cast<int16_t>(f[0] | f[1] << 8);
  st.index = f[2];
  adpcm::decode(st, f + 4, bytes, pcm);
  for (size_t i = bytes * 2u; i < kFrameSamples; ++i) pcm[i] = 0;
  return kFrameSamples;
}

// A mixed frame that had voice in it goes into the last 60 s heard (PLAY LAST), as a stored frame
// of its own.
void keepHeard(const int16_t *pcm, uint32_t from, int group, uint32_t now) {
  if (core->heardAudio.empty()) core->heardAudio.assign(kLastFrames * kStoredFrame, 0);
  uint8_t *s = &core->heardAudio[core->heardHead * kStoredFrame];
  adpcm::State &e = core->heardEnc;
  s[0] = static_cast<uint8_t>(e.predictor);
  s[1] = static_cast<uint8_t>(static_cast<uint16_t>(e.predictor) >> 8);
  s[2] = e.index;
  s[3] = kFrameBytes;
  adpcm::encode(e, pcm, kFrameSamples, s + 4);
  core->heardHead = (core->heardHead + 1) % kLastFrames;
  if (core->heardCount < kLastFrames) ++core->heardCount;
  if (from) core->heardFrom = from;
  core->heardGroup = group;
  core->heardLastMs = now;
}

void stopReplay() {
  Replay &p = core->replay;
  if (!p.on) return;
  p.on = false;
  if (p.held && !p.message) {
    std::lock_guard<std::mutex> l(core->mu);
    for (CallRec &c : core->log) {
      if (c.kind == Kind::Voice && !c.outgoing && c.played) c.heard = true;
    }
  }
  bump();
}

// Plays src (replayAudio for PLAY LAST, or msgAudio with the message's id) into the mix, over any
// voice that plays.
void startReplay(const std::vector<uint8_t> &src, uint32_t message, int group, uint32_t from) {
  if (core->out.recording) return setError("A MESSAGE IS RECORDING");
  if (src.empty()) return setError(message ? "THE MESSAGE WON'T LOAD" : "NOTHING HEARD TO PLAY");
  if (const char *why = duplexBlocked()) return setError(why);
  if (!ensureDuplex(millis())) return setError("THE SPEAKER WON'T START");
  stopReplay();
  renew(core->replay);
  Replay &p = core->replay;
  p.on = true;
  p.src = &src;
  p.message = message;
  p.group = group;
  p.from = from;
  {
    std::lock_guard<std::mutex> l(core->mu);
    core->error.clear();
  }
  bump();
}

// The replay's next frame, into replayPcm; false at its end.
bool replayFrame() {
  Replay &p = core->replay;
  if (!p.on) return false;
  if (holders.load() > 0) p.held = true;
  if ((p.pos + 1) * kStoredFrame > p.src->size()) {
    stopReplay();
    return false;
  }
  const uint8_t *stored = &(*p.src)[p.pos * kStoredFrame];
  decodeStored(stored, core->replayPcm);
  p.conceal.apply(core->replayPcm, kFrameSamples, stored[3] == 0);
  ++p.pos;
  ++p.frames;
  return true;
}

// --- Playing: the voices heard, mixed --------------------------------------------------------

OpenMicRec *findMic(int gi, uint32_t id);
OpenMicRec *findMicById(uint32_t id);
CallRec *ensureLogged(OpenMicRec &o, uint32_t now);

// What the last pull played, by board: counts, levels, the log, the call sheet.
void notePlayed(uint32_t now) {
  size_t n = 0;
  const Mixer::Played *played = core->mixer.played(n);
  for (size_t i = 0; i < n; ++i) {
    const Mixer::Played &p = played[i];
    bool firstOfSession = false;
    uint32_t queued = 0;
    {
      std::lock_guard<std::mutex> l(core->mu);
      OpenMicRec *o = findMicById(p.sender);
      if (!o) continue;
      ++o->frames;
      if (p.lost) ++o->lost;
      o->peaks[o->peakAt] = p.levelDb;
      o->peakAt = (o->peakAt + 1) % 10;
      o->level = *std::max_element(o->peaks, o->peaks + 10);
      o->playedMs = now;
      CallRec *c = ensureLogged(*o, now);
      if (c) {
        c->played = true;
        c->lost = o->lost;
        if (holders.load() > 0) c->heard = true;
      }
      if (BoardRec *b = findBoard(core->groups[o->group], o->id)) b->talkedAt = utcNow();
      // The call sheet and chirp: the first voice played here in a conversation, not each press.
      if (core->firstHeard.first(o->conv, o->id, o->st.session())) {
        firstOfSession = true;
        ++core->incoming;
        bump();
      }
    }
    if (p.first) {
      queued = audio::duplexQueuedMs();
      log_i("comms: %08lX's session %u plays %lu ms after its first frame came (%lu ms queued)",
            static_cast<unsigned long>(p.sender), p.session, static_cast<unsigned long>(p.waitedMs),
            static_cast<unsigned long>(queued));
    }
    if (firstOfSession) {
      bool chirp;
      {
        std::lock_guard<std::mutex> l(core->mu);
        chirp = core->chirpOn;
      }
      if (chirp && !core->mic.on) audio::chirp(true);
    }
  }
}

// Keeps the speaker fed while voices or a replay play: a mixed frame at a time, kMixAheadMs ahead
// of what's heard.
void pumpPlay(uint32_t now) {
  Mixer &mx = core->mixer;
  if (!mx.active() && !core->replay.on) return;
  if (!ensureDuplex(now)) {  // the speaker is someone else's: what came isn't played
    if (mx.active()) {
      mx.clear();
      drop(kDropCantPlay);
    }
    if (core->replay.on) stopReplay();
    return;
  }
  auto logEnded = [&mx] {
    Mixer::Ended e;
    if (!mx.ended(e)) return;
    log_i("comms: %08lX's session %u played out: frames %u to %u, %u came (%u never did); %u "
          "played, %u covered (%lu ms)",
          static_cast<unsigned long>(e.sender), e.session, e.from, e.top ? e.top - 1 : 0, e.came,
          e.top - e.from - e.came, e.played, e.lost,
          static_cast<unsigned long>((e.played + e.lost) * kFrameMs));
  };
  // Half duplex: while this board transmits, the mixer goes on, a frame each 20 ms by the clock
  // (its buffers drain, and the last 60 s heard keep what was talked over), but nothing of it is
  // written to the speaker (which stays silent, its amp off), and a replay or a message waits.
  if (core->mic.on && core->mic.capturing) {
    // (Signed: due in the future is the usual case between frames.)
    if (!core->halfDueMs || static_cast<int32_t>(now - core->halfDueMs) > 200) {
      core->halfDueMs = now;
    }
    for (int guard = 0; guard < 8 && static_cast<int32_t>(now - core->halfDueMs) >= 0; ++guard) {
      core->halfDueMs += kFrameMs;
      const int voices = mx.pull(core->mixPcm, now);
      logEnded();
      if (!voices) break;
      size_t n = 0;
      const Mixer::Played *played = mx.played(n);
      core->talkOverMs = now;
      core->talkOverBy = n ? played[0].sender : 0;
      int group = core->heardGroup;
      {
        std::lock_guard<std::mutex> l(core->mu);
        if (const OpenMicRec *o = findMicById(core->talkOverBy)) group = o->group;
      }
      keepHeard(core->mixPcm, core->talkOverBy, group, now);
      core->half.apply(core->mixPcm, kFrameSamples, voices, true);  // counted, not played
    }
    return;
  }
  core->halfDueMs = 0;
  for (int guard = 0; guard < 8 && audio::duplexQueuedMs() < kMixAheadMs; ++guard) {
    const int voices = mx.pull(core->mixPcm, now);
    logEnded();
    const bool replay = replayFrame();
    if (!voices && !replay) break;
    if (voices) {
      notePlayed(now);
      size_t n = 0;
      const Mixer::Played *played = mx.played(n);
      uint32_t loudest = 0;
      float top = -97;
      for (size_t i = 0; i < n; ++i) {
        if (played[i].levelDb > top) {
          top = played[i].levelDb;
          loudest = played[i].sender;
        }
      }
      int group = core->heardGroup;
      {
        std::lock_guard<std::mutex> l(core->mu);
        if (const OpenMicRec *o = findMicById(loudest)) group = o->group;
      }
      keepHeard(core->mixPcm, loudest, group, now);
    }
    if (replay) {
      for (size_t i = 0; i < kFrameSamples; ++i) {
        const int v = (voices ? core->mixPcm[i] : 0) + core->replayPcm[i];
        core->mixPcm[i] = static_cast<int16_t>(std::max(-32768, std::min(32767, v)));
      }
    }
    notePlayedLevel(peakDb(core->mixPcm, kFrameSamples));
    audio::duplexWrite(core->mixPcm, kFrameSamples);
    core->lastPlayMs = now;
  }
}

bool awakeNow();

// The duplex path goes once nothing of comms's needs it. While another board's mic that this
// board hears is on it stays (its next sound plays without the codec starting again), unless
// music waits to play on.
void pumpDuplex(uint32_t now) {
  if (core->duplexOurs && !audio::duplexOn()) core->duplexOurs = false;
  if (!core->duplexOurs || core->mic.on || core->mixer.active() || core->replay.on) return;
  if (now - core->lastPlayMs < kDuplexIdleMs || audio::duplexQueuedMs() > 0) return;
  if (!core->music.pending && awakeNow()) return;
  audio::duplexStop();
  core->duplexOurs = false;
  resetLevel();
  bump();
}

// --- The mic ---------------------------------------------------------------------------------

uint8_t batteryByte() {
  return core->battery >= 0 && core->battery <= 100 ? static_cast<uint8_t>(core->battery) : 0xFF;
}

uint32_t newConvId() {
  uint32_t id;
  do {
    id = esp_random();
  } while (!id);
  return id;
}

// Starts taking the duplex path's mic blocks (it starts the path if it isn't on).
void micCapture(uint32_t now) {
  MicRec &m = core->mic;
  if (!ensureDuplex(now)) {
    const char *why = duplexBlocked();
    m.paused = why ? why : audio::playing() ? "MUSIC IS PLAYING" : "THE MIC WON'T START";
    m.retryMs = now + kMicRetryMs;
    bump();
    return;
  }
  audio::duplexMic(true);
  m.capturing = true;
  m.paused = "";
  m.enc = adpcm::State();
  m.fill = 0;
  // The duplex path's thump just after it starts, and the cue (it plays into the speaker as the
  // mic opens, and the mic hears it): neither goes on the air. Back from a pause, no cue: a frame
  // of slack.
  const bool cold = now - core->duplexStartMs < 500;
  m.settle = cold || m.cue ? kMicSettleFrames : 1;
  m.settled = 0;
  if (m.cue) {
    m.cue = false;
    audio::micCue();
  }
  log_i("comms: the mic is capturing (%s)", cold ? "the duplex path started" : "warm");
  bump();
}

void micPaused(const char *why, uint32_t now) {
  MicRec &m = core->mic;
  if (m.capturing) audio::duplexMic(false);
  m.capturing = false;
  m.paused = why;
  m.level = -96;
  log_i("comms: the mic paused: %s", why);
  (void)now;
  bump();
}

// One frame of the mic's (bytes: under kFrameBytes for a press's last, short frame).
void sendMicFrame(const uint8_t *data, uint8_t bytes, const adpcm::State &st, uint8_t level) {
  MicRec &m = core->mic;
  Voice v;
  v.target = m.target;
  v.conv = m.conv;
  v.session = m.session;
  v.frame = m.frame++;
  v.level = level;
  v.flags = static_cast<uint8_t>((m.opened ? kVoiceOpening : 0) | (m.latched ? kVoiceLatched : 0));
  v.predictor = st.predictor;
  v.index = st.index;
  v.bytes = bytes;
  memcpy(v.data, data, bytes);
  uint8_t body[kMaxBody];
  const size_t n = putVoice(body, sizeof body, sentAt(), v);
  core->markFirstVoice = v.frame == 0;
  if (!m.vanished && n && sendOwn(core->groups[m.group], kVoice, body, n)) {
    if (!m.framesSent) {
      m.firstFrameMs = millis() - m.startMs;
      char peaks[16 * 4 + 1] = "";
      size_t at = 0;
      for (uint8_t i = 0; i < m.settled && at + 5 < sizeof peaks; ++i) {
        at += snprintf(peaks + at, sizeof peaks - at, " %d", m.settlePeaks[i]);
      }
      log_i("comms: the mic's first frame went %lu ms after it was switched on, at -%u dBFS; the "
            "%u frames dropped before it peaked at (dBFS):%s",
            static_cast<unsigned long>(m.firstFrameMs), level, m.settled, peaks);
    }
    ++m.framesSent;
  }
}

// samples of the mic in m.pcm (a whole frame, or a press's last part of one): its level, the
// history, then out at once.
void micFrame(uint32_t now, size_t samples) {
  MicRec &m = core->mic;
  if (m.toneUntilMs) {
    if (static_cast<int32_t>(now - m.toneUntilMs) < 0) {
      // 1 kHz at 16 kHz: 16 samples a period, its peak exactly on one of them. Added to what
      // the mic hears, as a voice in the room would be.
      for (size_t i = 0; i < samples; ++i, ++m.tonePhase) {
        const float x = sinf(2 * static_cast<float>(M_PI) * (m.tonePhase % 16) / 16);
        const long v = m.pcm[i] + lroundf(m.toneAmp * x);
        m.pcm[i] = static_cast<int16_t>(std::max(-32768L, std::min(32767L, v)));
      }
    } else {
      m.toneUntilMs = 0;
      log_i("comms: the mic's test tone is over");
    }
  }
  core->gate.process(m.pcm, samples);
  core->agc.process(m.pcm, samples);  // the level, the meter and the history: what goes out
  const float db = peakDb(m.pcm, samples);
  const uint8_t level = levelByte(db);
  const adpcm::State before = m.enc;
  uint8_t data[kFrameBytes];
  adpcm::encode(m.enc, m.pcm, samples, data);
  m.hist.note(now - m.startMs, db);
  if (m.peakSince == kNoLevel || level < m.peakSince) m.peakSince = level;
  m.peaks[m.peakAt] = db;
  m.peakAt = (m.peakAt + 1) % 10;
  m.level = *std::max_element(m.peaks, m.peaks + 10);
  sendMicFrame(data, static_cast<uint8_t>((samples + 1) / 2), before, level);
}

// The duplex path's mic blocks waiting, into frames: each whole one out (once the settle is over).
void takeBlocks(uint32_t now) {
  MicRec &m = core->mic;
  audio::DuplexBlock &b = core->block;
  for (int i = 0; i < 8 && audio::duplexRead(b, 0); ++i) {
    memcpy(m.pcm + m.fill, b.pcm, sizeof b.pcm);
    m.fill += audio::kDuplexBlock;
    if (m.fill < kFrameSamples) continue;
    m.fill = 0;
    if (m.settle) {
      --m.settle;
      if (m.settled < sizeof m.settlePeaks) {
        m.settlePeaks[m.settled++] =
            static_cast<int8_t>(std::max(-96.0f, peakDb(m.pcm, kFrameSamples)));
      }
      continue;
    }
    micFrame(now, kFrameSamples);
  }
}

void sendStillHere(uint32_t now) {
  MicRec &m = core->mic;
  MicStill s;
  s.flags = static_cast<uint8_t>((m.latched ? kMicLatched : 0) | (core->onUsb ? kMicUsb : 0) |
                                 (m.capturing ? 0 : kMicPaused) | (m.opened ? kMicOpening : 0));
  s.battery = batteryByte();
  s.peak = m.peakSince;
  m.peakSince = kNoLevel;
  s.session = m.session;
  s.onS = (now - m.startMs) / 1000;
  s.bar = static_cast<uint16_t>(std::min<uint32_t>((now - m.startMs) / kHistoryBarMs, 0xFFFF));
  s.barPeak = m.hist.at(s.bar);
  s.target = m.target;
  s.conv = m.conv;
  uint8_t body[kMaxBody];
  const size_t n = putMicStill(body, sizeof body, sentAt(), s);
  if (n && sendOwn(core->groups[m.group], kMic, body, n)) ++m.stillHeres;
}

void sendHistory(uint32_t to, uint32_t now) {
  MicRec &m = core->mic;
  History h;
  h.to = to;
  h.session = m.session;
  const uint16_t top =
      static_cast<uint16_t>(std::min<uint32_t>((now - m.startMs) / kHistoryBarMs, 0xFFFF));
  h.first = top + 1 >= kHistoryBars ? static_cast<uint16_t>(top + 1 - kHistoryBars) : 0;
  h.count = static_cast<uint8_t>(top - h.first + 1);
  for (uint8_t i = 0; i < h.count; ++i) h.bars[i] = m.hist.at(static_cast<uint16_t>(h.first + i));
  uint8_t body[kMaxBody];
  const size_t n = putHistory(body, sizeof body, sentAt(), h);
  if (n && sendOwn(core->groups[m.group], kExt, body, n)) ++m.histories;
}

void stopMic(uint8_t reason);
void ignoreRinging();

void startMic(int gi, uint32_t target) {
  MicRec &m = core->mic;
  auto fail = [](const char *why) {
    setError(why);
    if (!core->mic.on) micWanted = false;
  };
  if (gi < 0 || gi >= static_cast<int>(core->groups.size())) return fail("NO SUCH GROUP");
  if (core->groups[gi].via == Via::Net) return fail("NET ONLY: NO LINK YET");
  if (audio::recording()) return fail("THE RECORDER IS ON");
  if (audio::ringing()) return fail("AN ALARM IS RINGING");
  if (target == core->id) return fail("THAT'S THIS BOARD");
  if (!isListening && !radioStart()) return fail("THE RADIO WON'T START");
  if (m.on) {
    if (m.group == gi && m.target == target) return;  // as it is
    stopMic(kOffSwitched);  // to another group or board: a new session
  }
  if (core->ringing.on) ignoreRinging();  // a board whose mic is on doesn't ring
  const uint32_t now = millis();
  renew(m);
  m.on = true;
  m.group = gi;
  m.target = target;
  m.session = ++core->micSession;
  if (!m.session) m.session = ++core->micSession;
  m.startMs = now;
  m.since = utcNow();
  m.nextStillMs = now;
  m.cue = true;
  ++core->micCount;
  micWanted = true;
  {
    // The group's conversation: this board joins the one on, else its mic opens one. To a board
    // in it: to all of it.
    std::lock_guard<std::mutex> l(core->mu);
    ConvState &c = core->groups[gi].conv;
    m.opened = !c.on();
    c.join(now, m.opened ? newConvId() : 0);
    c.spoke(now, c.id(), core->id, core->id);
    m.conv = c.id();
    if (target && c.isMember(target)) m.target = 0;
    core->error.clear();
    CallRec r;
    r.at = m.since;
    r.startMs = now;
    r.group = gi;
    r.outgoing = true;
    r.from = core->id;
    r.to = m.target;
    r.call = m.session;
    r.conv = m.conv;
    logCall(r);
  }
  rememberMusic();
  presenceSoon(core->groups[gi]);  // its flag: the mic is on
  log_i("comms: the mic is on in %s to %s, conversation %08lX%s", core->groups[gi].name.c_str(),
        m.target ? "one board" : "all", static_cast<unsigned long>(m.conv),
        m.opened ? " (opens it)" : "");
  bump();
}

// The mic goes off: what it captured up to now goes first (the blocks the duplex path holds, and
// the part of a frame filled: a short last frame), so the end of the word arrives, then the off,
// three times 40 ms apart (at once, back to back, for a power-off).
void stopMic(uint8_t reason) {
  MicRec &m = core->mic;
  if (!m.on) return;
  const uint32_t now = millis();
  if (m.capturing && reason != kOffPowerOff) {
    takeBlocks(now);
    if (m.fill && !m.settle) micFrame(now, m.fill);
    m.fill = 0;
  }
  if (m.capturing) audio::duplexMic(false);
  m.capturing = false;
  m.level = -96;
  MicOff o;
  o.session = m.session;
  o.reason = reason;
  o.onS = (now - m.startMs) / 1000;
  uint8_t body[16];
  const size_t n = putMicOff(body, sizeof body, sentAt(), o);
  GroupRec &g = core->groups[m.group];
  if (!m.vanished) {
    if (reason == kOffPowerOff) {  // the radio goes in a moment: now, not from the queue
      for (int i = 0; i < 3; ++i) sendOwn(g, kMicOff, body, n);
    } else {
      for (const uint32_t after : {0u, 40u, 80u}) {
        if (!sendOwn(g, kMicOff, body, n, after) && after) sendOwn(g, kMicOff, body, n);
      }
    }
  }
  {
    std::lock_guard<std::mutex> l(core->mu);
    if (CallRec *c = findLogged(Kind::Voice, core->id, m.session, true)) {
      c->ms = now - m.startMs;
      c->live = false;
    }
    g.conv.spoke(now, m.conv, core->id, core->id);  // the last word: 60 s from here
  }
  m.on = false;
  m.latched = false;
  m.vanished = false;
  m.toneUntilMs = 0;
  micWanted = false;
  presenceSoon(g);
  log_i("comms: the mic is off (%s) after %lu ms: %lu frames sent",
        reason == kOffPowerOff ? "power-off" : "switched",
        static_cast<unsigned long>(now - m.startMs), static_cast<unsigned long>(m.framesSent));
  bump();
}

// The mic latched (held transmit): the boards that hear it know at once.
void latchMicNow() {
  MicRec &m = core->mic;
  if (!m.on || m.latched) return;
  m.latched = true;
  if (!m.vanished) {
    sendStillHere(millis());
    m.nextStillMs = millis() + kStillHereMs;
  }
  log_i("comms: the mic is latched on");
  bump();
}

void pumpMic(uint32_t now) {
  MicRec &m = core->mic;
  if (!m.on) return;
  if (m.capturing && (!core->duplexOurs || !audio::duplexOn())) {  // something took the port
    const char *why = duplexBlocked();
    if (!why && audio::playing()) why = "MUSIC IS PLAYING";
    micPaused(why ? why : "STARTING", now);
    core->duplexOurs = false;
    m.resumeMs = now + kResumeAfterMs;  // what took it shows itself meanwhile
  }
  if (!m.capturing && !poweringOff) {
    const char *why = duplexBlocked();
    if (!why && audio::playing()) {  // music started here: it has the speaker until it stops
      why = "MUSIC IS PLAYING";
      core->music.pending = false;
    }
    if (why) {
      if (strcmp(m.paused, why) != 0) {
        m.paused = why;
        bump();
      }
      m.resumeMs = now + kResumeAfterMs;  // once it's over, a moment more
    } else if (static_cast<int32_t>(now - m.resumeMs) >= 0 &&
               static_cast<int32_t>(now - m.retryMs) >= 0) {
      micCapture(now);
    }
  }
  if (m.capturing) takeBlocks(now);
  if (!m.vanished && static_cast<int32_t>(now - m.nextStillMs) >= 0) {
    sendStillHere(now);
    m.nextStillMs = now + kStillHereMs + esp_random() % 200;
  }
}

// --- Incoming packets ----------------------------------------------------------------------------

// Under mu: the board a relay's MAC belongs to, learnt from packets it sent itself.
uint32_t macToId(const uint8_t mac[6]) {
  for (const MacId &m : core->macs) {
    if (m.used && memcmp(m.mac, mac, 6) == 0) return m.id;
  }
  return 0;
}

void learnMac(const uint8_t mac[6], uint32_t id) {
  ++core->macTick;
  MacId *slot = &core->macs[0];
  for (MacId &m : core->macs) {
    if (m.used && memcmp(m.mac, mac, 6) == 0) {
      m.id = id;
      m.used = core->macTick;
      return;
    }
    if (!m.used || m.used < slot->used) slot = &m;
  }
  memcpy(slot->mac, mac, 6);
  slot->id = id;
  slot->used = core->macTick;
}

void micPresence(int gi, const Header &hd);

void onPresence(int gi, const Header &hd, Reader &r) {
  GroupRec &g = core->groups[gi];
  const uint8_t flags = r.u8();
  const uint8_t battery = r.u8();
  const uint8_t len = r.u8();
  const uint8_t *name = r.bytes(std::min<uint8_t>(len, 32));
  if (!r.ok()) {
    drop(kDropMalformed);
    return;
  }
  const std::string n = upperName(std::string(reinterpret_cast<const char *>(name), len));
  // The conversation it's in (from a board that knows conversations: an older one sends none).
  bool convKnown = false;
  uint32_t conv = 0, ageS = 0, quietS = 0;
  uint32_t members[ConvState::kMaxMembers];
  size_t count = 0;
  if (r.left() >= 4) {
    conv = r.u32();
    if (conv) {
      ageS = r.u16();
      quietS = r.u16();
      count = std::min<size_t>(r.u8(), ConvState::kMaxMembers);
      for (size_t i = 0; i < count; ++i) members[i] = r.u32();
    }
    convKnown = r.ok();
  }
  {
    std::lock_guard<std::mutex> l(core->mu);
    if (convKnown) {
      const uint32_t now = millis();
      const bool changed = conv ? g.conv.report(now, hd.sender, conv, ageS, quietS, members,
                                                count, core->id)
                                : g.conv.reportNone(now, hd.sender);
      if (changed) bump();
    }
    BoardRec &b = board(g, hd.sender);
    const uint8_t keep = flags & static_cast<uint8_t>(~kFlagHello);
    const int pct = battery <= 100 ? battery : -1;
    if (b.pollMs) {  // awake again
      b.pollMs = 0;
      bump();
    }
    if (b.name != n || b.flags != keep || b.battery != pct) {
      b.name = n;
      b.flags = keep;
      b.battery = pct;
      bump();
    }
  }
  if (!(flags & kFlagMic)) micPresence(gi, hd);
  const uint32_t now = millis();
  if ((flags & kFlagHello) && !g.answerDue && now - g.answeredMs > kHelloAnswerGapMs) {
    g.answerDue = stamp() + 100 + esp_random() % 500;
  }
}

void onPing(GroupRec &g, const Header &hd, const RxFrame &f, Reader &r) {
  const uint32_t id = r.u32();
  const uint32_t echo = r.u32();
  if (!r.ok()) return;
  uint8_t body[24];
  Writer w(body, sizeof body);
  const uint32_t hold = 5 + esp_random() % 150;  // the boards in range don't all answer at once
  w.u32(sentAt());
  w.u32(hd.sender);
  w.u32(id);
  w.u32(echo);
  w.u8(static_cast<uint8_t>(hd.hops == 0 ? f.rssi : 0));
  w.u8(hd.hops);
  w.u16(static_cast<uint16_t>(hold));
  sendOwn(g, kPong, body, w.size(), hold);
}

void onPong(const Header &hd, const RxFrame &f, Reader &r) {
  const uint32_t to = r.u32();
  const uint32_t id = r.u32();
  const uint32_t echo = r.u32();
  const int8_t rssiThere = static_cast<int8_t>(r.u8());
  const uint8_t hopsThere = r.u8();
  const uint16_t held = r.u16();
  if (!r.ok() || to != core->id || id != core->pingId) return;
  Pong p;
  p.id = hd.sender;
  p.rssiThere = rssiThere;
  p.hopsThere = hopsThere;
  p.rssiHere = hd.hops == 0 ? f.rssi : 0;
  p.hopsBack = hd.hops;
  p.rttMs = millis() - echo;
  p.heldMs = held;
  std::lock_guard<std::mutex> l(core->mu);
  p.name = nameOf(hd.sender);
  core->pongs.push_back(p);
  bump();
}

// --- Rings ---------------------------------------------------------------------------------------

// Under mu: the ring's log entry.
CallRec *ringRec(uint32_t from, uint16_t ring, bool outgoing) {
  return findLogged(Kind::Ring, from, ring, outgoing);
}

void sendRingEnd(Dialing &d, uint8_t reason) {
  uint8_t body[16];
  Writer w(body, sizeof body);
  w.u32(sentAt());
  w.u32(d.target);
  w.u16(d.ring);
  w.u8(reason);
  GroupRec &g = core->groups[d.group];
  for (const uint32_t after : {0u, 40u, 80u}) sendOwn(g, kRingEnd, body, w.size(), after);
}

// This board's ring is over (answered, given up, unanswered, a message instead).
void dialOver(DialState state, uint8_t reason) {
  Dialing &d = core->dialing;
  const uint32_t now = millis();
  if (d.state == DialState::Ringing && reason && !d.vanished) sendRingEnd(d, reason);
  d.state = state;
  d.endMs = now;
  {
    std::lock_guard<std::mutex> l(core->mu);
    if (CallRec *c = ringRec(core->id, d.ring, true)) {
      c->ms = now - d.startMs;
      c->live = false;
      c->answered = state == DialState::Answered;
    }
  }
  log_i("comms: ring %u over: %s", d.ring,
        state == DialState::Answered   ? "answered"
        : state == DialState::NoAnswer ? "no answer"
        : state == DialState::Recording ? "a message instead"
                                        : "given up");
  bump();
}

void startRing(int gi, uint32_t target) {
  Dialing &d = core->dialing;
  if (gi < 0 || gi >= static_cast<int>(core->groups.size())) return setError("NO SUCH GROUP");
  if (core->groups[gi].via == Via::Net) return setError("NET ONLY: NO LINK YET");
  if (d.state == DialState::Ringing) return setError("ALREADY RINGING");
  if (d.state == DialState::Recording) return setError("A MESSAGE IS GOING");
  if (target == core->id) return setError("THAT'S THIS BOARD");
  if (!isListening && !radioStart()) return setError("THE RADIO WON'T START");
  renew(d);
  d.state = DialState::Ringing;
  d.group = gi;
  d.target = target;
  d.ring = ++core->ringCounter;
  if (!d.ring) d.ring = ++core->ringCounter;
  d.startMs = d.nextSendMs = millis();
  ++core->dialCount;
  {
    std::lock_guard<std::mutex> l(core->mu);
    // An answer puts the boards in the conversation that's on, else in a new one.
    const ConvState &c = core->groups[gi].conv;
    d.conv = c.on() ? c.id() : newConvId();
    core->error.clear();
    CallRec r;
    r.kind = Kind::Ring;
    r.at = utcNow();
    r.startMs = d.startMs;
    r.group = gi;
    r.outgoing = true;
    r.from = core->id;
    r.to = target;
    r.call = d.ring;
    logCall(r);
  }
  log_i("comms: ringing %s in %s (ring %u, conversation %08lX)",
        target ? "one board" : "all", core->groups[gi].name.c_str(), d.ring,
        static_cast<unsigned long>(d.conv));
  bump();
}

int ringingNow(const Dialing &d, uint32_t now) {
  int n = 0;
  for (const Dialing::Ack &a : d.acks) {
    if (a.id && a.state == kRingingOn && now - a.ms < kRingingFreshMs) ++n;
  }
  return n;
}

void pumpDial(uint32_t now) {
  Dialing &d = core->dialing;
  if (d.state != DialState::Ringing) return;
  if (now - d.startMs >= kRingMs) return dialOver(DialState::NoAnswer, kEndNoAnswer);
  if (!d.vanished && static_cast<int32_t>(now - d.nextSendMs) >= 0) {
    uint8_t body[24];
    Writer w(body, sizeof body);
    w.u32(sentAt());
    w.u32(d.target);
    w.u16(d.ring);
    w.u32(d.conv);
    w.u32(now - d.startMs);
    sendOwn(core->groups[d.group], kRing, body, w.size());
    d.nextSendMs += kRingEveryMs;
    if (static_cast<int32_t>(now - d.nextSendMs) >= 0) d.nextSendMs = now + kRingEveryMs;
  }
  const int n = ringingNow(d, now);
  if (n != d.shownRinging) {
    d.shownRinging = n;
    bump();
  }
}

// A board says it rings for this board's ring (or has stopped).
void onRinging(const Header &hd, Reader &r) {
  const uint32_t dialer = r.u32();
  const uint16_t ring = r.u16();
  const uint8_t state = r.u8();
  Dialing &d = core->dialing;
  if (!r.ok() || dialer != core->id || ring != d.ring) return;
  Dialing::Ack *slot = nullptr;
  for (Dialing::Ack &a : d.acks) {
    if (a.id == hd.sender) slot = &a;
  }
  for (Dialing::Ack &a : d.acks) {
    if (!slot && !a.id) slot = &a;
  }
  if (!slot) return;
  slot->id = hd.sender;
  slot->ms = stamp();
  slot->state = state;
}

void sendRinging(GroupRec &g, uint32_t dialer, uint16_t ring, uint8_t state) {
  uint8_t body[16];
  Writer w(body, sizeof body);
  w.u32(sentAt());
  w.u32(dialer);
  w.u16(ring);
  w.u8(state);
  sendOwn(g, kRinging, body, w.size(), 20 + esp_random() % 180);
}

// The ring on this board stops.
// missedRing()'s count as last read or written (-1: not read yet), so the UI's checks don't read
// NVS each turn.
std::atomic<int> missedCount{-1};

// A ring nobody answered while silent and reachable (missedRing()).
void noteMissedRing(uint32_t from, time_t at) {
  std::string name;
  {
    std::lock_guard<std::mutex> l(core->mu);
    name = nameOf(from);
  }
  MissedRec r;
  kv::load("reach", "missed", r);
  addMissed(r, from, name.c_str(), static_cast<uint32_t>(at ? at : utcNow()));
  kv::save("reach", "missed", r);
  missedCount = r.count;
  log_i("comms: a missed ring from %08lX kept for the off face (%u)",
        static_cast<unsigned long>(from), r.count);
}

void ringingOver(RingEnd why) {
  Ringing &rg = core->ringing;
  if (!rg.on) return;
  const uint32_t now = millis();
  rg.on = false;
  rg.ended = why;
  rg.doneFrom = rg.from;
  rg.doneRing = rg.ring;
  {
    std::lock_guard<std::mutex> l(core->mu);
    if (CallRec *c = ringRec(rg.from, rg.ring, false)) {
      c->ms = now - rg.startMs;
      c->live = false;
      c->answered = why == RingEnd::Answered || why == RingEnd::AnsweredThere;
      // Missed (not heard) only if nobody did anything with it here or anywhere.
      c->missed = why == RingEnd::GaveUp || why == RingEnd::NoAnswer || why == RingEnd::Lost;
      c->heard = !c->missed;
    }
  }
  static const char *const kWhy[] = {"",       "answered here", "answered there", "ignored",
                                     "given up", "no answer",   "lost",           "a message"};
  log_i("comms: ring from %08lX stopped: %s", static_cast<unsigned long>(rg.from),
        kWhy[static_cast<int>(why)]);
  // Silent and reachable: nobody heard it, so the off faces show it (apps/missed_ring.h). To
  // show it without silent too, drop audio::silent() here.
  const bool unanswered = why == RingEnd::GaveUp || why == RingEnd::NoAnswer ||
                          why == RingEnd::Lost || why == RingEnd::Message;
  if (unanswered && audio::silent() && reachInterval() > 0) noteMissedRing(rg.from, rg.at);
  bump();
}

void sendAnswer(GroupRec &g, uint32_t dialer, uint16_t ring, uint32_t conv) {
  uint8_t body[16];
  Writer w(body, sizeof body);
  w.u32(sentAt());
  w.u32(dialer);
  w.u16(ring);
  w.u32(conv);
  sendOwn(g, kAnswer, body, w.size());
}

void onRing(int gi, const Header &hd, Reader &r) {
  const uint32_t target = r.u32();
  const uint16_t ring = r.u16();
  const uint32_t conv = r.u32();
  const uint32_t elapsed = r.u32();
  if (!r.ok()) {
    drop(kDropMalformed);
    return;
  }
  core->rings.heard(gi, hd.sender, ring, target, elapsed, millis());  // for sleepers' polls
  if (target && target != core->id) return;  // for another board (relayed all the same)
  if (core->mic.on) return;  // an open mic doesn't ring: it's in the conversation, and quiet
  Ringing &rg = core->ringing;
  GroupRec &g = core->groups[gi];
  const uint32_t now = millis();
  if (rg.on && rg.from == hd.sender && rg.ring == ring) {
    rg.lastMs = now;
    sendRinging(g, hd.sender, ring, kRingingOn);
    return;
  }
  if (rg.doneFrom == hd.sender && rg.doneRing == ring && rg.ended != RingEnd::Lost) {
    // The dialer still rings: what this board said may have been lost on the air.
    if (rg.ended == RingEnd::Answered) sendAnswer(g, hd.sender, ring, rg.conv);
    if (rg.ended == RingEnd::Ignored) sendRinging(g, hd.sender, ring, kRingingStopped);
    return;
  }
  if (rg.on) return;  // one ring at a time
  if (elapsed >= kRingMs) return;
  const Dialing &d = core->dialing;
  if (d.state == DialState::Ringing && d.group == gi) return;  // ringing them too: busy
  {
    std::lock_guard<std::mutex> l(core->mu);
    if (g.conv.mine()) return;  // in the conversation already
  }
  renew(rg);
  rg.on = true;
  rg.group = gi;
  rg.from = hd.sender;
  rg.ring = ring;
  rg.conv = conv;
  rg.toAll = target == 0;
  rg.startMs = now - elapsed;
  rg.lastMs = rg.nextChirpMs = now;
  rg.at = utcNow() ? utcNow() - static_cast<time_t>(elapsed / 1000) : 0;
  ++core->ringsSeen;
  {
    std::lock_guard<std::mutex> l(core->mu);
    CallRec c;
    c.kind = Kind::Ring;
    c.at = rg.at;
    c.startMs = rg.startMs;
    c.group = gi;
    c.from = hd.sender;
    c.to = target;
    c.call = ring;
    c.hops = hd.hops;
    logCall(c);
  }
  log_i("comms: %08lX rings %s in %s", static_cast<unsigned long>(hd.sender),
        target ? "this board" : "all", g.name.c_str());
  sendRinging(g, hd.sender, ring, kRingingOn);
  bump();
}

void pumpRinging(uint32_t now) {
  Ringing &rg = core->ringing;
  if (!rg.on) return;
  if (now - rg.lastMs >= kRingLostMs) return ringingOver(RingEnd::Lost);
  if (now - rg.startMs >= kRingMs + kRingLostMs) return ringingOver(RingEnd::NoAnswer);
  if (static_cast<int32_t>(now - rg.nextChirpMs) >= 0) {
    if (canChirp()) audio::ringChirp();  // over voice too: audio mixes it in
    rg.nextChirpMs = now + kRingChirpMs;
  }
}

void answerRing() {
  Ringing &rg = core->ringing;
  if (!rg.on) return setError("NOTHING RINGS");
  GroupRec &g = core->groups[rg.group];
  sendAnswer(g, rg.from, rg.ring, rg.conv);
  {
    std::lock_guard<std::mutex> l(core->mu);
    const uint32_t now = millis();
    g.conv.join(now, rg.conv);
    g.conv.report(now, rg.from, rg.conv, 0, 0, nullptr, 0, core->id);  // the dialer is in too
    core->error.clear();
  }
  presenceSoon(g);
  ringingOver(RingEnd::Answered);
}

void ignoreRinging() {
  Ringing &rg = core->ringing;
  if (!rg.on) return;
  sendRinging(core->groups[rg.group], rg.from, rg.ring, kRingingStopped);
  ringingOver(RingEnd::Ignored);
}

// Someone answered a ring: the dialer's, or one this board rang for, or neither (the
// conversation is on all the same).
void onAnswer(int gi, const Header &hd, Reader &r) {
  const uint32_t dialer = r.u32();
  const uint16_t ring = r.u16();
  const uint32_t conv = r.u32();
  if (!r.ok() || !conv) return;
  core->rings.over(dialer, ring);
  GroupRec &g = core->groups[gi];
  Dialing &d = core->dialing;
  const uint32_t now = millis();
  if (dialer == core->id && ring == d.ring) {
    // Late, after this board gave up or began a message: it stays out.
    if (d.state != DialState::Ringing && d.state != DialState::Answered) return;
    {
      std::lock_guard<std::mutex> l(core->mu);
      g.conv.join(now, conv);
      g.conv.report(now, hd.sender, conv, 0, 0, nullptr, 0, core->id);
    }
    presenceSoon(g);
    if (d.state == DialState::Ringing) {
      d.answeredBy = hd.sender;
      dialOver(DialState::Answered, 0);  // the answer has stopped it everywhere
    }
    bump();
    return;
  }
  Ringing &rg = core->ringing;
  if (rg.on && rg.from == dialer && rg.ring == ring) ringingOver(RingEnd::AnsweredThere);
  std::lock_guard<std::mutex> l(core->mu);
  if (g.conv.report(now, hd.sender, conv, 0, 0, &dialer, 1, core->id)) bump();
}

void onRingEnd(const Header &hd, Reader &r) {
  r.u32();  // the target
  const uint16_t ring = r.u16();
  const uint8_t reason = r.u8();
  if (r.ok()) core->rings.over(hd.sender, ring);
  Ringing &rg = core->ringing;
  if (!r.ok() || !rg.on || rg.from != hd.sender || rg.ring != ring) return;
  ringingOver(reason == kEndNoAnswer ? RingEnd::NoAnswer
              : reason == kEndMessage ? RingEnd::Message
                                      : RingEnd::GaveUp);
}

// --- Messages kept -------------------------------------------------------------------------------

// Under mu.
MsgRec *findMsg(uint32_t local) {
  for (MsgRec &m : core->msgs) {
    if (m.local == local) return &m;
  }
  return nullptr;
}

// Under mu.
MsgRec *findMsg(uint32_t sender, uint32_t msg) {
  for (MsgRec &m : core->msgs) {
    if (m.e.sender == sender && m.e.msg == msg) return &m;
  }
  return nullptr;
}

// Under mu: the index of what's on the card, to write outside the lock.
std::vector<store::Entry> cardIndex() {
  std::vector<store::Entry> out;
  for (const MsgRec &m : core->msgs) {
    if (m.onCard && !m.receiving) out.push_back(m.e);
  }
  return out;
}

void saveCardIndex() {
  std::vector<store::Entry> idx;
  {
    std::lock_guard<std::mutex> l(core->mu);
    idx = cardIndex();
  }
  if (store::available()) store::saveIndex(idx);
}

// Under mu: forgets a message (its file too), and its log entry. Returns whether one was on the
// card, so the caller writes the index again.
bool dropMsg(uint32_t local) {
  for (auto it = core->msgs.begin(); it != core->msgs.end(); ++it) {
    if (it->local != local) continue;
    const bool card = it->onCard;
    if (card) store::removeAudio(it->e);
    core->msgs.erase(it);
    for (auto c = core->log.begin(); c != core->log.end(); ++c) {
      if (c->kind == Kind::Message && c->message == local) {
        core->log.erase(c);
        break;
      }
    }
    bump();
    return card;
  }
  return false;
}

// Under mu: within kMaxMessages, and within kMessageRam in PSRAM. The oldest played message goes
// first, then the oldest. Returns whether the card's index changed.
bool trimMsgs() {
  bool card = false;
  for (;;) {
    size_t kept = 0, ram = 0;
    for (const MsgRec &m : core->msgs) {
      if (m.receiving) continue;
      ++kept;
      ram += m.audio.size();
    }
    if (kept <= static_cast<size_t>(kMaxMessages) && ram <= kMessageRam) return card;
    const MsgRec *victim = nullptr;
    for (const MsgRec &m : core->msgs) {  // oldest first
      if (m.receiving) continue;
      if (ram > kMessageRam && m.onCard) continue;  // RAM is what's over
      if (m.e.played) {
        victim = &m;
        break;
      }
      if (!victim) victim = &m;
    }
    if (!victim) return card;
    log_i("comms: message from %08lX dropped to make room",
          static_cast<unsigned long>(victim->e.sender));
    card |= dropMsg(victim->local);
  }
}

// Under mu: a message's log entry.
CallRec &logMsg(const MsgRec &m, bool outgoing) {
  CallRec c;
  c.kind = Kind::Message;
  c.at = m.e.at;
  c.startMs = m.arrivedMs;
  c.group = m.group;
  c.outgoing = outgoing;
  c.from = m.e.sender;
  c.to = m.e.to;
  c.ms = m.e.frames * kFrameMs;
  c.live = m.receiving;
  c.played = c.heard = m.e.played;
  c.complete = m.e.complete;
  c.message = m.local;
  return logCall(c);
}

// The card's messages, at the start (begin(), before the task) and after a reload.
void loadMsgs() {
  std::vector<store::Entry> idx = store::load();
  std::sort(idx.begin(), idx.end(),
            [](const store::Entry &a, const store::Entry &b) { return a.at < b.at; });
  std::lock_guard<std::mutex> l(core->mu);
  for (const store::Entry &e : idx) {
    if (findMsg(e.sender, e.msg)) continue;
    MsgRec m;
    m.local = ++core->msgCounter;
    m.e = e;
    m.group = groupIndex(e.group);
    m.onCard = true;
    core->msgs.push_back(m);
    if (m.group >= 0) logMsg(core->msgs.back(), false);
  }
  if (!idx.empty()) log_i("comms: %u messages on the card", static_cast<unsigned>(idx.size()));
}

// Played a day ago: gone. (Played while the clock wasn't set: kept until room is needed.)
void expireMsgs() {
  const time_t now = utcNow();
  if (!now) return;
  bool card = false;
  {
    std::lock_guard<std::mutex> l(core->mu);
    for (size_t i = 0; i < core->msgs.size();) {
      const MsgRec &m = core->msgs[i];
      if (m.e.played && m.e.playedAt && now - m.e.playedAt > kMessageDayS) {
        card |= dropMsg(m.local);
      } else {
        ++i;
      }
    }
  }
  if (card) saveCardIndex();
}

// --- Messages leaving ----------------------------------------------------------------------------

void sendMessageFrame(Outbound &o, uint16_t f) {
  if (static_cast<size_t>(f + 1) * kStoredFrame > o.audio.size()) return;
  const uint8_t *s = &o.audio[static_cast<size_t>(f) * kStoredFrame];
  uint8_t body[kMaxBody];
  Writer w(body, sizeof body);
  w.u32(sentAt());
  w.u32(o.msg);
  w.u32(o.to);
  w.u16(f);
  w.u16(static_cast<uint16_t>(s[0] | s[1] << 8));
  w.u8(s[2]);
  w.u8(s[3]);
  w.bytes(s + 4, std::min<uint8_t>(s[3], kFrameBytes));
  sendOwn(core->groups[o.group], kMessage, body, w.size());
}

void sendMessageEnd(Outbound &o, bool cancelled, uint32_t delayMs) {
  uint8_t body[24];
  Writer w(body, sizeof body);
  w.u32(sentAt());
  w.u32(o.msg);
  w.u32(o.to);
  w.u16(o.frames);
  w.u32(o.sum);
  w.u8(cancelled ? kMsgCancelled : 0);
  w.u16(o.ring);
  sendOwn(core->groups[o.group], kMessageEnd, body, w.size(), delayMs);
}

// One 20 ms frame recorded: kept, and sent at once.
void messageFrame(Outbound &o, size_t samples) {
  samples &= ~static_cast<size_t>(1);
  if (o.audio.size() / kStoredFrame >= MessageAssembly::kMaxFrames) return;
  const size_t at = o.audio.size();
  o.audio.resize(at + kStoredFrame, 0);
  uint8_t *s = &o.audio[at];
  s[0] = static_cast<uint8_t>(o.enc.predictor);
  s[1] = static_cast<uint8_t>(static_cast<uint16_t>(o.enc.predictor) >> 8);
  s[2] = o.enc.index;
  s[3] = static_cast<uint8_t>(samples / 2);
  core->gate.process(o.pcm, samples);
  core->agc.process(o.pcm, samples);
  adpcm::encode(o.enc, o.pcm, samples, s + 4);
  sendMessageFrame(o, static_cast<uint16_t>(at / kStoredFrame));
}

void startMessage(int group, uint32_t target) {
  Dialing &d = core->dialing;
  Outbound &o = core->out;
  const uint32_t now = millis();
  if (o.recording) return setError("A MESSAGE IS GOING");
  int gi = group;
  uint32_t to = target;
  uint16_t ring = 0;
  const bool forRing = group < 0;
  if (forRing) {
    const bool may = (d.state == DialState::Ringing && now - d.startMs >= kMessageAfterMs) ||
                     d.state == DialState::NoAnswer;
    if (!may) return setError("NO RING TO LEAVE A MESSAGE FOR");
    gi = d.group;
    to = d.target;
    ring = d.ring;
  }
  if (gi < 0 || gi >= static_cast<int>(core->groups.size())) return setError("NO SUCH GROUP");
  if (core->groups[gi].via == Via::Net) return setError("NET ONLY: NO LINK YET");
  if (audio::recording()) return setError("THE RECORDER IS ON");
  if (audio::ringing()) return setError("AN ALARM IS RINGING");
  if (audio::capturing()) return setError("THE AGENT IS LISTENING");
  if (!isListening && !radioStart()) return setError("THE RADIO WON'T START");
  // It takes the mic and the speaker: an open mic pauses, voice and a replay stop meanwhile.
  if (core->mic.capturing) micPaused("A MESSAGE IS RECORDING", now);
  stopReplay();
  core->mixer.clear();
  if (core->duplexOurs) {
    audio::duplexStop();
    core->duplexOurs = false;
  }
  rememberMusic();
  // The ring stops as the message begins: the boards it rang stop ringing and wait for it.
  if (forRing && d.state == DialState::Ringing) {
    dialOver(DialState::Recording, kEndMessage);
  } else if (!forRing) {
    if (d.state == DialState::Ringing) dialOver(DialState::None, kEndGaveUp);
    renew(d);
    d.group = gi;
    d.target = to;
    d.startMs = d.endMs = now;
    ++core->dialCount;
  }
  if (!audio::captureStart(true)) {
    d.state = DialState::None;
    return setError("THE MIC WON'T START");
  }
  renew(o);
  o.recording = true;
  o.group = gi;
  o.to = to;
  o.ring = ring;
  do {
    o.msg = esp_random();
  } while (!o.msg);
  o.startMs = millis();
  o.audio.reserve(512 * kStoredFrame);
  d.state = DialState::Recording;
  {
    std::lock_guard<std::mutex> l(core->mu);
    core->error.clear();
    MsgRec m;  // only for the log entry
    m.e.sender = core->id;
    m.e.msg = o.msg;
    m.e.to = to;
    m.e.at = utcNow();
    m.group = gi;
    m.receiving = true;
    m.arrivedMs = o.startMs;
    logMsg(m, true).message = 0;
  }
  log_i("comms: recording a message %08lX for %s in %s", static_cast<unsigned long>(o.msg),
        to ? "one board" : "all", core->groups[gi].name.c_str());
  bump();
}

void finishMessage(bool send) {
  Outbound &o = core->out;
  if (!o.recording) return;
  audio::captureStop();
  for (;;) {  // the rest of the ring, then a short last frame
    const size_t n = audio::captureRead(reinterpret_cast<uint8_t *>(o.pcm) + o.fillBytes,
                                        kFrameSamples * 2 - o.fillBytes, 0);
    if (n == 0) break;
    o.fillBytes += n;
    if (o.fillBytes == kFrameSamples * 2) {
      messageFrame(o, kFrameSamples);
      o.fillBytes = 0;
    }
  }
  if (send && o.fillBytes >= 4) messageFrame(o, o.fillBytes / 2);
  o.fillBytes = 0;
  o.recording = false;
  o.frames = static_cast<uint16_t>(o.audio.size() / kStoredFrame);
  o.sum = messageSum(o.audio.data(), o.frames);
  Dialing &d = core->dialing;
  const uint32_t now = millis();
  {
    std::lock_guard<std::mutex> l(core->mu);
    for (auto c = core->log.rbegin(); c != core->log.rend(); ++c) {
      if (c->kind == Kind::Message && c->outgoing && c->live) {
        if (send) {
          c->live = false;
          c->ms = o.frames * kFrameMs;
        } else {
          core->log.erase(std::next(c).base());
        }
        break;
      }
    }
  }
  if (!send) {
    for (const uint32_t after : {0u, 40u, 80u}) sendMessageEnd(o, true, after);
    o.audio.clear();
    o.audio.shrink_to_fit();
    d.state = DialState::None;
    log_i("comms: message %08lX dropped", static_cast<unsigned long>(o.msg));
    bump();
    return;
  }
  for (const uint32_t after : {0u, 40u, 80u}) sendMessageEnd(o, false, after);
  o.sent = true;
  o.sentMs = now;
  o.resend.assign((o.frames + 7) / 8, 0);
  d.state = DialState::Sent;
  d.endMs = now;
  log_i("comms: message %08lX sent: %u frames (%lu ms), checksum %08lX",
        static_cast<unsigned long>(o.msg), o.frames,
        static_cast<unsigned long>(o.frames * kFrameMs), static_cast<unsigned long>(o.sum));
  bump();
}

void pumpMessageOut(uint32_t now) {
  Outbound &o = core->out;
  if (o.recording) {
    for (int i = 0; i < 4; ++i) {
      const size_t n = audio::captureRead(reinterpret_cast<uint8_t *>(o.pcm) + o.fillBytes,
                                          kFrameSamples * 2 - o.fillBytes, 0);
      if (n == 0) break;
      o.fillBytes += n;
      if (o.fillBytes == kFrameSamples * 2) {
        messageFrame(o, kFrameSamples);
        o.fillBytes = 0;
      }
    }
    core->msgLevel = audio::captureLevelDb();
    if (!audio::capturing() || now - o.startMs >= kMaxMessageMs) finishMessage(true);
    return;
  }
  if (!o.sent) return;
  if (now - o.sentMs >= kMessageKeepMs) {  // long enough for anyone to ask
    o.sent = false;
    o.resending = false;
    o.audio.clear();
    o.audio.shrink_to_fit();
    o.resend.clear();
    return;
  }
  if (!o.resending || static_cast<int32_t>(now - o.nextResendMs) < 0) return;
  for (uint16_t f = 0; f < o.frames; ++f) {
    if (!((o.resend[f / 8] >> (f % 8)) & 1)) continue;
    o.resend[f / 8] &= static_cast<uint8_t>(~(1u << (f % 8)));
    sendMessageFrame(o, f);
    o.nextResendMs = now + kResendGapMs;
    return;
  }
  o.resending = false;  // all sent again: the end again, for a board that missed it
  sendMessageEnd(o, false, 0);
}

// A board asks for frames of this board's message again, or says it has them all.
void onMessageAsk(const Header &hd, Reader &r) {
  const uint32_t sender = r.u32();
  const uint32_t msg = r.u32();
  const uint8_t flags = r.u8();
  r.u16();  // the frames it has
  const uint16_t from = r.u16();
  const uint16_t span = r.u16();
  const uint8_t *bits = r.bytes((span + 7u) / 8u);
  Outbound &o = core->out;
  if (!r.ok() || sender != core->id || !o.sent || msg != o.msg) return;
  if (flags & kAskComplete) {
    std::lock_guard<std::mutex> l(core->mu);
    if (std::find(o.delivered.begin(), o.delivered.end(), hd.sender) == o.delivered.end()) {
      o.delivered.push_back(hd.sender);
      for (auto c = core->log.rbegin(); c != core->log.rend(); ++c) {
        if (c->kind == Kind::Message && c->outgoing) {
          if (std::find(c->heardBy.begin(), c->heardBy.end(), hd.sender) == c->heardBy.end()) {
            c->heardBy.push_back(hd.sender);
          }
          break;
        }
      }
      bump();
    }
    return;
  }
  size_t asked = 0;
  for (uint32_t i = 0; i < span; ++i) {
    const uint32_t f = from + i;
    if (f >= o.frames) break;
    if ((bits[i / 8] >> (i % 8)) & 1) {
      o.resend[f / 8] |= static_cast<uint8_t>(1u << (f % 8));
      ++asked;
    }
  }
  if (asked || (flags & kAskNoEnd)) {
    if (!o.resending) o.nextResendMs = millis();
    o.resending = true;
  }
  log_i("comms: %08lX asks for %u frames of the message again%s",
        static_cast<unsigned long>(hd.sender), static_cast<unsigned>(asked),
        flags & kAskNoEnd ? " (and its end)" : "");
}

// --- Messages coming -----------------------------------------------------------------------------

void sendAsk(Inbound &in, bool complete) {
  uint8_t body[kMaxBody];
  Writer w(body, sizeof body);
  w.u32(sentAt());
  w.u32(in.sender);
  w.u32(in.msg);
  uint8_t bits[kAskMapBytes];
  uint16_t from = 0;
  size_t missing = 0;
  const size_t span = complete ? 0 : in.a.missingMap(from, bits, kAskMapFrames, missing);
  w.u8(complete ? kAskComplete : in.a.ended() ? 0 : kAskNoEnd);
  w.u16(in.a.have());
  w.u16(from);
  w.u16(static_cast<uint16_t>(span));
  w.bytes(bits, (span + 7) / 8);
  sendOwn(core->groups[in.group], kMessageAsk, body, w.size(), 10 + esp_random() % 90);
}

Inbound *findInbound(uint32_t sender, uint32_t msg) {
  for (Inbound &in : core->inbound) {
    if (in.used && in.sender == sender && in.msg == msg) return &in;
  }
  return nullptr;
}

void finishInbound(Inbound &in);

// A message begins coming in (its first frame, or its end with every frame missed).
Inbound *beginInbound(int gi, uint32_t sender, uint32_t msg, uint32_t to) {
  Inbound *slot = nullptr;
  for (Inbound &in : core->inbound) {
    if (!in.used) {
      slot = &in;
      break;
    }
  }
  if (!slot) {  // two coming already: the one heard from longest ago is kept as it is
    slot = &core->inbound[0];
    for (Inbound &in : core->inbound) {
      if (in.lastMs < slot->lastMs) slot = &in;
    }
    finishInbound(*slot);
  }
  Inbound &in = *slot;
  renew(in);
  in.used = true;
  in.group = gi;
  in.sender = sender;
  in.msg = msg;
  in.to = to;
  in.firstMs = in.lastMs = millis();
  Ringing &rg = core->ringing;
  if (rg.on && rg.from == sender) ringingOver(RingEnd::Message);
  {
    std::lock_guard<std::mutex> l(core->mu);
    MsgRec m;
    m.local = in.local = ++core->msgCounter;
    m.e.sender = sender;
    m.e.msg = msg;
    m.e.group = core->groups[gi].id;
    m.e.to = to;
    m.e.at = utcNow();
    m.e.fromName = nameOf(sender);
    m.group = gi;
    m.receiving = true;
    m.arrivedMs = in.firstMs;
    core->msgs.push_back(m);
    logMsg(core->msgs.back(), false);
  }
  log_i("comms: a message %08lX from %08lX coming", static_cast<unsigned long>(msg),
        static_cast<unsigned long>(sender));
  bump();
  return &in;
}

// The message is in (whole, or as whole as it will get): kept, on the card if there is one.
void finishInbound(Inbound &in) {
  if (!in.used) return;
  MessageAssembly &a = in.a;
  const bool complete = a.sumOk();
  if (a.allIn() && !complete) {
    log_w("comms: message %08lX: every frame came, but the checksum doesn't match",
          static_cast<unsigned long>(in.msg));
  }
  const uint32_t frames = a.ended() ? a.total() : a.top();
  std::vector<uint8_t> audio;
  audio.swap(a.audio());
  audio.resize(static_cast<size_t>(frames) * kStoredFrame, 0);
  const uint32_t sum = messageSum(audio.data(), frames);
  store::Entry e;
  bool card = false;
  {
    std::lock_guard<std::mutex> l(core->mu);
    MsgRec *m = findMsg(in.local);
    if (!m) {
      in.used = false;
      return;
    }
    m->receiving = false;
    m->e.frames = frames;
    m->e.sum = sum;
    m->e.complete = complete;
    if (m->e.fromName.empty()) m->e.fromName = nameOf(in.sender);
    e = m->e;
    for (CallRec &c : core->log) {
      if (c.kind == Kind::Message && c.message == in.local) {
        c.live = false;
        c.complete = complete;
        c.ms = frames * kFrameMs;
      }
    }
  }
  const bool onCard = frames > 0 && store::available() && store::saveAudio(e, audio);
  {
    std::lock_guard<std::mutex> l(core->mu);
    if (MsgRec *m = findMsg(in.local)) {
      m->onCard = onCard;
      if (!onCard) m->audio.swap(audio);
      card = trimMsgs() || onCard;
    }
    ++core->messagesIn;
  }
  if (card) saveCardIndex();
  if (complete) sendAsk(in, true);  // the sender counts the boards that have it
  if (canChirp()) audio::messageChirp();
  log_i("comms: message %08lX from %08lX kept%s: %lu frames (%lu ms), %u asks, checksum %08lX, "
        "%s",
        static_cast<unsigned long>(in.msg), static_cast<unsigned long>(in.sender),
        onCard ? " on the card" : " in RAM", static_cast<unsigned long>(frames),
        static_cast<unsigned long>(frames * kFrameMs), static_cast<unsigned>(in.asks),
        static_cast<unsigned long>(sum), complete ? "complete" : "NOT complete");
  in.used = false;
  a.clear();
  bump();
}

void onMessage(int gi, const Header &hd, Reader &r) {
  const uint32_t msg = r.u32();
  const uint32_t to = r.u32();
  const uint16_t frame = r.u16();
  const int16_t predictor = static_cast<int16_t>(r.u16());
  const uint8_t index = r.u8();
  const uint8_t bytes = r.u8();
  const uint8_t *data = r.bytes(bytes);
  if (!r.ok() || bytes > kFrameBytes) {
    drop(kDropMalformed);
    return;
  }
  if (to && to != core->id) return;  // for another board (relayed all the same)
  const int nth = dropNth.load();
  if (nth > 0 && ++dropTick % static_cast<uint32_t>(nth) == 0) {
    drop(kDropMessageTest);
    return;
  }
  Inbound *in = findInbound(hd.sender, msg);
  if (!in) {
    {
      std::lock_guard<std::mutex> l(core->mu);
      if (findMsg(hd.sender, msg)) return;  // kept already: a frame sent again for another
    }
    in = beginInbound(gi, hd.sender, msg, to);
  }
  if (in->a.add(frame, predictor, index, data, bytes)) in->lastMs = millis();
  if (in->a.allIn()) finishInbound(*in);
}

void onMessageEnd(int gi, const Header &hd, Reader &r) {
  const uint32_t msg = r.u32();
  const uint32_t to = r.u32();
  const uint16_t frames = r.u16();
  const uint32_t sum = r.u32();
  const uint8_t flags = r.u8();
  if (!r.ok()) return;
  if (to && to != core->id) return;
  Inbound *in = findInbound(hd.sender, msg);
  if (flags & kMsgCancelled) {
    if (in) {
      in->used = false;
      in->a.clear();
      std::lock_guard<std::mutex> l(core->mu);
      dropMsg(in->local);
    }
    Ringing &rg = core->ringing;
    if (rg.on && rg.from == hd.sender) ringingOver(RingEnd::GaveUp);
    return;
  }
  if (!in) {
    bool kept;
    {
      std::lock_guard<std::mutex> l(core->mu);
      const MsgRec *m = findMsg(hd.sender, msg);
      kept = m && m->e.complete;
      if (m && !kept) return;  // kept as it was; too late now
    }
    if (kept) {  // its "have it all" may not have reached the sender
      Inbound tmp;
      tmp.group = gi;
      tmp.sender = hd.sender;
      tmp.msg = msg;
      sendAsk(tmp, true);
      return;
    }
    in = beginInbound(gi, hd.sender, msg, to);  // every frame missed: asked for from here
  }
  in->a.end(frames, sum);
  in->lastMs = millis();
  if (in->a.allIn()) finishInbound(*in);
}

// Asks for what's missing once frames have stopped coming; gives up after a while.
void pumpInbound(uint32_t now) {
  for (Inbound &in : core->inbound) {
    if (!in.used) continue;
    if (in.a.have() != in.shownHave) {  // the screens show it coming, about once a second
      in.shownHave = in.a.have();
      if (in.a.have() % 50 == 0) {
        std::lock_guard<std::mutex> l(core->mu);
        if (MsgRec *m = findMsg(in.local)) m->e.frames = in.a.top();
        bump();
      }
    }
    if (in.stale >= kStaleAsks || now - in.firstMs >= kGiveUpMs) {
      finishInbound(in);
      continue;
    }
    const uint32_t quiet = now - in.lastMs;
    const bool ask = in.a.ended() ? quiet >= kAskQuietMs : quiet >= kNoEndMs;
    if (!ask || static_cast<int32_t>(now - in.nextAskMs) < 0) continue;
    uint8_t bits[kAskMapBytes];
    uint16_t from = 0;
    size_t missing = 0;
    in.a.missingMap(from, bits, kAskMapFrames, missing);
    in.stale = in.asks > 0 && in.a.have() == in.haveAtAsk ? in.stale + 1 : 0;
    in.haveAtAsk = in.a.have();
    sendAsk(in, false);
    ++in.asks;
    // Time for the sender to send them all again, before asking once more.
    in.nextAskMs = now + 1000 + std::min<uint32_t>(missing * kResendGapMs, 4000);
    log_i("comms: asked %08lX for %lu frames again (ask %d)%s",
          static_cast<unsigned long>(in.sender), static_cast<unsigned long>(missing), in.asks,
          in.a.ended() ? "" : " and the end");
  }
}

// Plays a kept message (the task).
void playMsg(uint32_t local) {
  store::Entry e;
  bool onCard = false;
  int group = 0;
  {
    std::lock_guard<std::mutex> l(core->mu);
    MsgRec *m = findMsg(local);
    if (!m || m->receiving) {
      core->error = "NO SUCH MESSAGE";
      bump();
      return;
    }
    e = m->e;
    onCard = m->onCard;
    group = m->group;
    if (!onCard) core->msgAudio = m->audio;
  }
  if (core->out.recording) return setError("A MESSAGE IS RECORDING");
  stopReplay();  // msgAudio is about to change under it
  if (onCard && !store::loadAudio(e, core->msgAudio)) core->msgAudio.clear();
  startReplay(core->msgAudio, local, group, e.sender);
  if (!core->replay.on || core->replay.message != local) return;
  bool card = false;
  {
    std::lock_guard<std::mutex> l(core->mu);
    if (MsgRec *m = findMsg(local)) {
      if (!m->e.played) {
        m->e.played = true;
        m->e.playedAt = utcNow();
        card = m->onCard;
      }
      for (CallRec &c : core->log) {
        if (c.kind == Kind::Message && c.message == local) c.played = c.heard = true;
      }
    }
  }
  if (card) saveCardIndex();
  log_i("comms: playing the message from %08lX, %lu ms", static_cast<unsigned long>(e.sender),
        static_cast<unsigned long>(e.frames * kFrameMs));
  bump();
}

// --- Open mics heard ---------------------------------------------------------------------------

// Under mu.
OpenMicRec *findMic(int gi, uint32_t id) {
  for (OpenMicRec &o : core->mics) {
    if (o.group == gi && o.id == id) return &o;
  }
  return nullptr;
}

// Under mu.
OpenMicRec *findMicById(uint32_t id) {
  for (OpenMicRec &o : core->mics) {
    if (o.id == id) return &o;
  }
  return nullptr;
}

// Under mu: its voice is for this board (to all of the group, or to this board alone).
bool forMe(const OpenMicRec &o) { return o.target == 0 || o.target == core->id; }

// Under mu: its voice plays here: it's for this board, which is in the group's conversation.
bool heardHere(const OpenMicRec &o) {
  return forMe(o) && core->groups[o.group].conv.mine();
}

// Under mu: the session's log entry, made the first time it's heard here.
CallRec *ensureLogged(OpenMicRec &o, uint32_t now) {
  if (o.logged) return findLogged(Kind::Voice, o.id, o.st.session(), false);
  if (!o.st.on()) return nullptr;
  o.logged = true;
  CallRec c;
  const time_t utc = utcNow();
  c.at = utc ? utc - static_cast<time_t>((now - o.st.startMs()) / 1000) : 0;
  c.startMs = o.st.startMs();
  c.group = o.group;
  c.from = o.id;
  c.to = o.target;
  c.call = o.st.session();
  c.hops = o.hops;
  c.relay = o.relay;
  c.rssi = o.rssi;
  c.conv = o.conv;
  return &logCall(c);
}

// Under mu: the session heard has ended (its off, a new one in its place, cut off, or it's
// forgotten).
void endHeardSession(OpenMicRec &o, uint16_t session, uint32_t ms, bool cut = false) {
  if (o.logged) {
    if (CallRec *c = findLogged(Kind::Voice, o.id, session, false)) {
      c->ms = ms;
      c->live = false;
      c->lost = o.lost;
      c->cutOff = cut;
      if (!c->played) c->heard = true;  // nothing played: nothing to hear later
    }
  }
  o.logged = false;
  bump();
}

// Under mu: makes room by forgetting the mic heard from longest ago (not one whose alarm sounds).
OpenMicRec &micRec(int gi, uint32_t id, uint32_t now) {
  if (OpenMicRec *o = findMic(gi, id)) return *o;
  if (core->mics.size() >= kMaxOpenMics) {
    auto victim = core->mics.end();
    for (auto it = core->mics.begin(); it != core->mics.end(); ++it) {
      if (it->st.alarm()) continue;
      if (victim == core->mics.end() || now - it->st.heardMs() > now - victim->st.heardMs()) {
        victim = it;
      }
    }
    if (victim == core->mics.end()) victim = core->mics.begin();
    endHeardSession(*victim, victim->st.session(), victim->st.heardMs() - victim->st.startMs());
    core->mics.erase(victim);
  }
  OpenMicRec o;
  o.group = gi;
  o.id = id;
  core->mics.push_back(std::move(o));
  bump();
  return core->mics.back();
}

// Under mu: what a packet of a mic changed.
void micChanged(OpenMicRec &o, const HeardMic::Change &ch, uint32_t now) {
  if (ch.ended) endHeardSession(o, ch.endedSession, ch.endedMs);
  if (ch.began) {  // a new session: its bars and counts start again
    o.hist.clear();
    o.haveHistory = false;
    o.historyAsks = 0;
    o.nextAskMs = now;
    o.frames = o.lost = o.late = o.histories = 0;
    o.haveStill = false;
    log_i("comms: %08lX's mic is on (session %u)", static_cast<unsigned long>(o.id),
          o.st.session());
  }
  if (ch.back) log_i("comms: %08lX's link is back", static_cast<unsigned long>(o.id));
  if (ch.shown) bump();
}

// This board is in the group's conversation `conv` from now (an open mic's walkie-talkie, or a
// mic to it alone).
void joinConvWith(int gi, uint32_t conv) {
  GroupRec &g = core->groups[gi];
  {
    std::lock_guard<std::mutex> l(core->mu);
    if (!g.conv.join(millis(), conv)) return;
  }
  log_i("comms: in the conversation in %s (%08lX): an open mic's", g.name.c_str(),
        static_cast<unsigned long>(conv));
  presenceSoon(g);
  bump();
}

// Type 2: a board's mic is on (every 2 s while it is).
void onMicStill(int gi, const Header &hd, const RxFrame &f, Reader &r, uint32_t relay) {
  MicStill s;
  if (!readMicStill(r, s)) {
    drop(kDropMalformed);
    return;
  }
  const uint32_t now = stamp();
  GroupRec &g = core->groups[gi];
  bool join = false, ask = false;
  {
    std::lock_guard<std::mutex> l(core->mu);
    OpenMicRec &o = micRec(gi, hd.sender, now);
    micChanged(o, o.st.on(now, s.session, s.onS * 1000, s.flags & kMicLatched), now);
    o.boot = hd.boot;
    o.lastSeq = hd.seq;
    o.hops = hd.hops;
    o.relay = relay;
    o.rssi = hd.hops == 0 ? f.rssi : 0;
    if (!o.haveStill || o.last.flags != s.flags || o.last.battery != s.battery ||
        o.target != s.target) {
      bump();
    }
    o.last = s;
    o.haveStill = true;
    o.target = s.target;
    o.conv = s.conv;
    o.opened = s.flags & kMicOpening;
    o.latched = o.st.latched();
    o.hist.set(s.bar, s.barPeak);
    // A mic on in the conversation keeps it on.
    if (s.conv && g.conv.spoke(now, s.conv, hd.sender, core->id)) bump();
    if (heardHere(o)) ensureLogged(o, now);  // its session is in the log, played or not
    // The walkie-talkie: a mic that opened the conversation, or one to this board alone, puts
    // this board in it (unless it left).
    join = forMe(o) && s.conv && !g.conv.mine() && !g.conv.left() &&
           (o.opened || s.target == core->id) && !core->out.recording;
    // Came in late to a mic that has been on a while: the bars before, once.
    ask = forMe(o) && !o.haveHistory && o.historyAsks < kHistoryAsks && s.bar > 0 &&
          static_cast<int32_t>(now - o.nextAskMs) >= 0;
    if (ask) {
      ++o.historyAsks;
      o.nextAskMs = now + kHistoryAskMs;
    }
  }
  if (join) joinConvWith(gi, s.conv);
  if (ask) {
    uint8_t body[16];
    const size_t n = putHistoryAsk(body, sizeof body, sentAt(), hd.sender);
    sendOwn(g, kExt, body, n);
  }
}

enum class Whose : uint8_t {
  Ours,  // this board is in its conversation: it plays
  Join,  // it plays, and puts this board in its conversation
  No,    // not for this board now: not played
};

// Whether voice in conversation conv (to target, with the voice flags) plays here.
Whose whose(int gi, uint32_t conv, uint8_t flags, uint32_t target) {
  if (!conv) return Whose::Ours;
  std::lock_guard<std::mutex> l(core->mu);
  const ConvState &c = core->groups[gi].conv;  // a group has one: conv is it, or merges into it
  if (c.mine()) return Whose::Ours;
  if (c.left()) return Whose::No;  // left: nothing plays here until join()
  if (target == core->id) return Whose::Join;  // to this board alone
  if (flags & kVoiceOpening) return Whose::Join;  // its mic opened it: the walkie-talkie
  return Whose::No;
}

// Type 3: 20 ms of a board's open mic.
void onVoice(int gi, const Header &hd, const RxFrame &f, Reader &r, uint32_t relay) {
  Voice v;
  if (!readVoice(r, v)) {
    drop(kDropMalformed);
    return;
  }
  const uint32_t now = stamp();
  GroupRec &g = core->groups[gi];
  {
    std::lock_guard<std::mutex> l(core->mu);
    OpenMicRec &o = micRec(gi, hd.sender, now);
    micChanged(o, o.st.on(now, v.session, 0, v.flags & kVoiceLatched), now);
    o.st.voice(now);
    o.latched = o.st.latched();
    o.boot = hd.boot;
    o.lastSeq = hd.seq;
    o.target = v.target;
    o.conv = v.conv;
    o.opened = v.flags & kVoiceOpening;
    o.hops = hd.hops;
    o.relay = relay;
    o.rssi = hd.hops == 0 ? f.rssi : 0;
    if (v.conv && v.frame % 10 == 0 && g.conv.spoke(now, v.conv, hd.sender, core->id)) bump();
  }
  if (v.target && v.target != core->id) {
    drop(kDropNotForUs);
    return;
  }
  if (core->out.recording) {  // the mic and the speaker are the message's
    drop(kDropRecording);
    return;
  }
  const Whose w = whose(gi, v.conv, v.flags, v.target);
  if (w == Whose::No) {
    drop(kDropNotInConversation);
    return;
  }
  if (w == Whose::Join) joinConvWith(gi, v.conv);
  Mixer::Frame fr;
  fr.frame = v.frame;
  fr.predictor = v.predictor;
  fr.index = v.index;
  fr.bytes = v.bytes;
  memcpy(fr.data, v.data, v.bytes);
  switch (core->mixer.add(hd.sender, v.session, fr, now)) {
    case Mixer::Took::Ok: break;
    case Mixer::Took::Late: {
      drop(kDropLate);
      std::lock_guard<std::mutex> l(core->mu);
      if (OpenMicRec *o = findMic(gi, hd.sender)) ++o->late;
      break;
    }
    case Mixer::Took::Copy: drop(kDropDuplicate); break;
    case Mixer::Took::NoRoom: drop(kDropNoVoiceRoom); break;
  }
}

// Type 4: a board's mic went off.
void onMicOff(int gi, const Header &hd, Reader &r) {
  MicOff off;
  if (!readMicOff(r, off)) {
    drop(kDropMalformed);
    return;
  }
  const uint32_t now = stamp();
  bool chirp = false;
  {
    std::lock_guard<std::mutex> l(core->mu);
    OpenMicRec *o = findMic(gi, hd.sender);
    if (!o) return;
    const bool heard = heardHere(*o);
    const HeardMic::Change ch = o->st.off(now, off.session);
    if (!ch.ended) return;
    const uint32_t ms = std::max<uint32_t>(ch.endedMs, off.onS * 1000);
    endHeardSession(*o, ch.endedSession, ms);
    chirp = heard && ms >= core->longMicMs;
    log_i("comms: %08lX's mic is off (%s) after %lu s%s", static_cast<unsigned long>(hd.sender),
          off.reason == kOffPowerOff ? "power-off" : "switched", static_cast<unsigned long>(ms / 1000),
          ch.back ? ", after LOST" : "");
    core->mics.erase(core->mics.begin() + (o - core->mics.data()));
    bump();
  }
  // A mic on a long while went off where it was heard: one soft chirp, so a bumped key in the
  // room isn't silent (not on a board whose own mic is on).
  if (chirp && canChirp()) {
    audio::micOffChirp();
    log_i("comms: a long session ended: chirped");
  }
}

// A board's presence says its mic isn't on: its off was missed (or it restarted while LOST).
void micPresence(int gi, const Header &hd) {
  std::lock_guard<std::mutex> l(core->mu);
  OpenMicRec *o = findMic(gi, hd.sender);
  if (!o || (hd.boot == o->boot && hd.seq <= o->lastSeq)) return;  // older than its mic packets
  const uint32_t now = stamp();
  const HeardMic::Change ch = o->st.off(now, 0);
  if (ch.ended) endHeardSession(*o, ch.endedSession, ch.endedMs);
  log_i("comms: %08lX says its mic is off%s", static_cast<unsigned long>(hd.sender),
        ch.back ? " (back after LOST)" : "");
  core->mics.erase(core->mics.begin() + (o - core->mics.data()));
  bump();
}

void onHistoryAsk(int gi, const Header &hd, Reader &r) {
  const uint32_t board = r.u32();
  MicRec &m = core->mic;
  if (!r.ok() || board != core->id || !m.on || gi != m.group || m.vanished) return;
  sendHistory(hd.sender, millis());
  log_i("comms: sent %08lX the mic's history", static_cast<unsigned long>(hd.sender));
}

void onHistory(int gi, const Header &hd, Reader &r) {
  History h;
  if (!readHistory(r, h)) {
    drop(kDropMalformed);
    return;
  }
  std::lock_guard<std::mutex> l(core->mu);
  OpenMicRec *o = findMic(gi, hd.sender);
  if (!o || h.to != core->id || h.session != o->st.session()) return;
  for (uint8_t i = 0; i < h.count; ++i) {
    const uint16_t bar = static_cast<uint16_t>(h.first + i);
    // The bar now comes with every still-here too; keep the louder.
    const uint8_t had = o->hist.at(bar);
    o->hist.set(bar, had != kNoLevel && (h.bars[i] == kNoLevel || had < h.bars[i]) ? had
                                                                                   : h.bars[i]);
  }
  o->haveHistory = true;
  ++o->histories;
  log_i("comms: %08lX's history came: %u bars from bar %u", static_cast<unsigned long>(hd.sender),
        h.count, h.first);
  bump();
}

// --- Reachable on battery: the awake side ----------------------------------------------------

void sendReachAnswer(GroupRec &g, const Answer &a) {
  uint8_t body[48];
  const size_t n = putAnswer(body, sizeof body, sentAt(), a);
  if (!n || g.via == Via::Net || !espnowUp) return;
  // ttl 0: the sleeper listens a few milliseconds, for the boards in range; nothing is relayed.
  uint8_t f[kMaxFrame];
  Header h;
  h.type = kExt;
  h.ttl = 0;
  h.group = g.id;
  h.sender = core->id;
  h.boot = core->boot;
  h.seq = ++core->seq;
  putHeader(h, f);
  memcpy(f + kHeader, body, n);
  const size_t len = g.sealer->seal(f, n);
  if (len) radioSend(f, len);
}

// What this board knows that a poll from group gi could want (the task's state, now).
void gatherWaiting(int gi, uint32_t sleeper, uint32_t now, WaitingNow &w) {
  // A ring: this board's own, or one going by.
  const Dialing &d = core->dialing;
  RingWatch::Seen seen;
  if (d.state == DialState::Ringing && d.group == gi && (d.target == 0 || d.target == sleeper)) {
    w.ringOn = true;
    w.dialer = core->id;
    w.ringNo = d.ring;
  } else if (core->rings.ringFor(gi, sleeper, now, seen)) {
    w.ringOn = true;
    w.dialer = seen.dialer;
    w.ringNo = seen.ring;
  }
  // Voice: this board's mic, and the mics it hears. A held mic's (someone talking) wakes a
  // sleeper; a latched one's (a room) doesn't: it never says it sounds.
  const MicRec &m = core->mic;
  if (m.on && w.micCount < WaitingNow::kMics) {
    WaitingNow::MicNow &x = w.mics[w.micCount++];
    x.group = m.group;
    x.target = m.target;
    x.board = core->id;
    x.opened = m.opened;
    if (m.capturing && !m.latched) x.soundMs = now;  // it transmits
  }
  {
    std::lock_guard<std::mutex> l(core->mu);
    for (const OpenMicRec &o : core->mics) {
      if (w.micCount >= WaitingNow::kMics) break;
      if (!o.st.on()) continue;
      WaitingNow::MicNow &x = w.mics[w.micCount++];
      x.group = o.group;
      x.target = o.target;
      x.board = o.id;
      x.opened = o.opened;
      x.soundMs = o.st.latched() ? 0 : o.st.voiceMs();  // its last voice frame
    }
  }
  // A message: this board's own, while it keeps it for asks.
  const Outbound &o = core->out;
  if (o.sent && o.group == gi && now - o.sentMs < kMessageKeepMs) {
    w.messageOn = true;
    w.msgGroup = gi;
    w.msgSender = core->id;
    w.msg = o.msg;
    w.msgTo = o.to;
    w.frames = o.frames;
    std::lock_guard<std::mutex> l(core->mu);
    for (const uint32_t b : o.delivered) {
      if (w.delivered < 8) w.deliveredTo[w.delivered++] = b;
    }
  }
}

// This board woke for a message (reachSleep()'s answer): it asks for it as for one whose every
// frame it missed (pumpInbound() asks for the end, then the frames).
void fetchWokeMessage(int gi) {
  const uint32_t sender = core->fetchSender, msg = core->fetchMsg;
  if (!sender || findInbound(sender, msg)) return;
  bool kept = false, whole = false;
  uint32_t frames = 0;
  {
    std::lock_guard<std::mutex> l(core->mu);
    if (const MsgRec *m = findMsg(sender, msg)) {
      kept = true;
      whole = m->e.complete;
      frames = m->e.frames;
    }
  }
  if (!kept) {
    beginInbound(gi, sender, msg, core->fetchTo);
    return;
  }
  // Kept already: its "I have it all" was lost on the way. Said again, so the sender stops
  // answering this board's polls with it (and it doesn't wake for it at every poll).
  if (!whole) return;
  uint8_t body[24];
  Writer w(body, sizeof body);
  w.u32(sentAt());
  w.u32(sender);
  w.u32(msg);
  w.u8(kAskComplete);
  w.u16(static_cast<uint16_t>(frames));
  w.u16(0);
  w.u16(0);
  sendOwn(core->groups[gi], kMessageAsk, body, w.size(), 50);
  log_i("comms: woke for a message kept already; told %08lX so",
        static_cast<unsigned long>(sender));
}

// A sleeping board asks whether anything waits for it: it shows as asleep and reachable, and gets
// an answer if anything does (or, on USB, if it asks for one anyway: the channel check).
void onReachPoll(int gi, const Header &hd, const RxFrame &f, Reader &r) {
  Poll p;
  if (!readPoll(r, p)) {
    drop(kDropMalformed);
    return;
  }
  GroupRec &g = core->groups[gi];
  const uint32_t now = stamp();
  {
    std::lock_guard<std::mutex> l(core->mu);
    BoardRec &b = board(g, hd.sender);
    const std::string n = upperName(std::string(p.name, p.nameLen));
    const int pct = p.battery <= 100 ? p.battery : -1;
    if (!b.pollMs || b.pollS != p.intervalS || b.battery != pct || (!n.empty() && b.name != n)) {
      bump();
    }
    b.pollMs = now;
    b.pollS = p.intervalS;
    b.battery = pct;
    if (!n.empty()) b.name = n;
  }
  WaitingNow w;
  gatherWaiting(gi, hd.sender, now, w);
  Answer a;
  a.to = hd.sender;
  a.what = waitingFor(w, gi, hd.sender, p.intervalS, now);
  a.channel = static_cast<uint8_t>(radioChannel());
  a.flags = core->onUsb ? kFlagUsb : 0;
  if (a.what & kWaitRing) {
    a.dialer = w.dialer;
    a.ring = w.ringNo;
  }
  if (a.what & kWaitMessage) {
    a.msgSender = w.msgSender;
    a.msg = w.msg;
    a.msgTo = w.msgTo;
    a.frames = w.frames;
  }
  if (a.what || ((p.flags & kPollEcho) && core->onUsb)) {
    sendReachAnswer(g, a);
    if (a.what) {
      log_i("comms: %08lX polled (asleep, %d dBm): told it %s%s%s",
            static_cast<unsigned long>(hd.sender), f.rssi, a.what & kWaitRing ? "a ring " : "",
            a.what & kWaitVoice ? "voice " : "", a.what & kWaitMessage ? "a message" : "");
    }
  }
}

// Type 15: the byte after sentAt says which.
void onExt(int gi, const Header &hd, const RxFrame &f, Reader &r) {
  const uint8_t ext = r.u8();
  if (!r.ok()) {
    drop(kDropMalformed);
    return;
  }
  switch (ext) {
    case kHistoryAsk: onHistoryAsk(gi, hd, r); break;
    case kHistory: onHistory(gi, hd, r); break;
    case kExtPoll: onReachPoll(gi, hd, f, r); break;
    case kExtAnswer: break;  // for a sleeping board's loop (reachPoll()), not the task
    default: drop(kDropUnknownType); break;  // a later version's: passed on all the same
  }
}

// Each second: QUIET after a sound, LOST after kLostMs, and the alarm; LOST mics nobody's alarm
// is for are forgotten once their conversation is over here.
void tickMics(uint32_t now) {
  bool alarm = false;
  std::lock_guard<std::mutex> l(core->mu);
  for (size_t i = 0; i < core->mics.size();) {
    OpenMicRec &o = core->mics[i];
    // A board whose own mic is on sounds the alarm only for a mic that was on before its own.
    const bool alarmable =
        heardHere(o) &&
        (!core->mic.on || static_cast<int32_t>(o.st.startMs() - core->mic.startMs) < 0);
    const HeardMic::Change ch = o.st.tick(now, alarmable);
    if (ch.shown) bump();
    if (ch.cut) {  // held, then nothing: cut off (out of range mid-press?), no alarm
      log_w("comms: %08lX's mic cut off: nothing from it for %lu s, no off",
            static_cast<unsigned long>(o.id), static_cast<unsigned long>(kCutMs / 1000));
      endHeardSession(o, ch.endedSession, ch.endedMs, true);
      core->mixer.drop(o.id);
      core->mics.erase(core->mics.begin() + i);
      continue;
    }
    if (ch.lost) {
      log_w("comms: LOST: nothing from %08lX's latched mic for %lu s%s",
            static_cast<unsigned long>(o.id), static_cast<unsigned long>(kLostMs / 1000),
            o.st.alarm() ? "" : " (no alarm here)");
      if (o.logged) {
        if (CallRec *c = findLogged(Kind::Voice, o.id, o.st.session(), false)) c->linkLost = true;
      }
      if (o.st.alarm() && !core->alarm) core->nextAlarmMs = now;
      core->mixer.drop(o.id);
    }
    if (o.st.link() == HeardMic::Link::Lost && !o.st.alarm() &&
        (!core->groups[o.group].conv.mine() || now - o.st.heardMs() >= kLostKeepMs)) {
      endHeardSession(o, o.st.session(), o.st.heardMs() - o.st.startMs());
      log_i("comms: %08lX's LOST mic forgotten", static_cast<unsigned long>(o.id));
      core->mics.erase(core->mics.begin() + i);
      bump();
      continue;
    }
    alarm = alarm || o.st.alarm();
    ++i;
  }
  if (alarm != core->alarm) {
    core->alarm = alarm;
    bump();
  }
}

void pumpAlarm(uint32_t now) {
  if (!core->alarm || static_cast<int32_t>(now - core->nextAlarmMs) < 0) return;
  audio::lostAlarm();
  core->nextAlarmMs = now + kAlarmEveryMs;
}

// The board powers off (forceOff()): the mic's off goes now.
void micPowerOff() {
  if (core->mic.on) stopMic(kOffPowerOff);
  powerOffSent = true;
}

// --- Conversations -------------------------------------------------------------------------------

void joinConv(int gi) {
  if (gi < 0 || gi >= static_cast<int>(core->groups.size())) return;
  GroupRec &g = core->groups[gi];
  {
    std::lock_guard<std::mutex> l(core->mu);
    if (!g.conv.join(millis(), 0)) return;
  }
  log_i("comms: joined the conversation in %s", g.name.c_str());
  presenceSoon(g);
  bump();
}

void leaveConv(int gi) {
  if (gi < 0 || gi >= static_cast<int>(core->groups.size())) return;
  GroupRec &g = core->groups[gi];
  // Out of the conversation, out of it with the mic too.
  if (core->mic.on && core->mic.group == gi) stopMic(kOffSwitched);
  {
    std::lock_guard<std::mutex> l(core->mu);
    if (!g.conv.leave(millis())) return;
    for (const OpenMicRec &o : core->mics) {  // its voices stop here
      if (o.group == gi) core->mixer.drop(o.id);
    }
  }
  log_i("comms: left the conversation in %s", g.name.c_str());
  presenceSoon(g);
  bump();
}

// Passes a packet on, unchanged but for its ttl and hops, a few ms later so relays in range of
// each other don't all send at once.
// A voice or message frame goes on only if a board this one hears straight would otherwise miss
// it (comms_mesh.h: Neighbours::needed()); everything else floods.
void relayFrame(const RxFrame &f, const Header &hd, bool unread) {
  const bool audio = hd.type == kVoice || hd.type == kMessage;
  {
    std::lock_guard<std::mutex> l(core->mu);
    if (!core->relayOn) return;
    if (audio && !floodAll.load()) {
      const uint32_t from = hd.hops == 0 ? hd.sender : macToId(f.mac);
      if (!core->links.needed(hd.sender, from, stamp())) {
        counters.audioNotNeeded.fetch_add(1, std::memory_order_relaxed);
        return;
      }
    }
  }
  uint8_t copy[kMaxFrame];
  memcpy(copy, f.data, f.len);
  if (!forward(copy)) return;
  if (!queuePending(copy, f.len, 1 + esp_random() % 6, true, unread, audio)) {
    drop(kDropRelayFull);
  }
}

// A links packet: in the link group, straight from its board (never relayed), sealed with the
// public key. Spoofed, it can only make this board relay more or less.
void onLinkFrame(const Header &hd, const RxFrame &f) {
  uint8_t buf[kMaxFrame];
  memcpy(buf, f.data, f.len);
  size_t bodyLen = 0;
  if (!core->linkSealer || !core->linkSealer->open(buf, f.len, bodyLen)) {
    drop(kDropBadTag);
    return;
  }
  Reader r(buf + kHeader, bodyLen);
  r.u32();  // sentAt
  const uint8_t ext = r.u8();
  if (hd.type != kExt || hd.hops != 0 || !r.ok()) {
    drop(kDropMalformed);
    return;
  }
  if (ext != kExtLinks) {  // a later version's link packet
    drop(kDropUnknownType);
    return;
  }
  Links &l = core->linksRx;
  if (!readLinks(r, l)) {
    drop(kDropMalformed);
    return;
  }
  counters.linksHeard.fetch_add(1, std::memory_order_relaxed);
  std::lock_guard<std::mutex> lk(core->mu);
  core->links.links(hd.sender, l, stamp());
}

void handleFrame(const RxFrame &f) {
  Header hd;
  if (!parseHeader(f.data, f.len, hd)) {
    drop(kDropMalformed);
    return;
  }
  if (hd.sender == core->id) {
    drop(kDropOwn);
    return;
  }
  // Every packet straight from a board (hops 0), whatever its group: its MAC is learnt as that
  // board's (a relay's copies name it), and how well it's heard goes into this board's links.
  const uint32_t deaf = deafTo.load();
  {
    std::lock_guard<std::mutex> l(core->mu);
    if (hd.hops == 0) learnMac(f.mac, hd.sender);
    if (deaf && (hd.hops == 0 ? hd.sender == deaf : macToId(f.mac) == deaf)) {
      drop(kDropDeaf);  // as if out of its range
      return;
    }
    if (hd.hops == 0) core->links.heard(hd.sender, hd.boot, hd.seq, f.rssi, stamp());
  }
  if (hd.group == core->linkGroup) {
    onLinkFrame(hd, f);
    return;
  }
  const int gi = groupIndex(hd.group);
  if (gi < 0) {  // not ours: passed on unread
    if (core->seen.seen(hd.group, hd.sender, hd.boot, hd.seq)) {
      drop(kDropDuplicate);
      return;
    }
    core->seen.add(hd.group, hd.sender, hd.boot, hd.seq);
    if (hd.ttl > 0) {
      relayFrame(f, hd, true);
    } else {
      drop(kDropOtherGroup);
    }
    return;
  }
  GroupRec &g = core->groups[gi];
  if (g.via == Via::Net) {
    drop(kDropViaNet);
    return;
  }
  if (core->seen.seen(hd.group, hd.sender, hd.boot, hd.seq)) {
    drop(kDropDuplicate);
    return;
  }
  uint8_t buf[kMaxFrame];
  memcpy(buf, f.data, f.len);  // opened in place; f stays as it came, for relaying
  size_t bodyLen = 0;
  if (!g.sealer->open(buf, f.len, bodyLen)) {
    drop(kDropBadTag);
    return;
  }
  Reader r(buf + kHeader, bodyLen);
  const uint32_t at = r.u32();
  switch (core->guard.accept(hd.sender, hd.boot, hd.seq, fresh(at))) {
    case ReplayGuard::Verdict::Ok: break;
    case ReplayGuard::Verdict::Stale: drop(kDropStale); return;
    default: drop(kDropReplay); return;
  }
  core->seen.add(hd.group, hd.sender, hd.boot, hd.seq);
  counters.handled.fetch_add(1, std::memory_order_relaxed);

  // How the sender is reached.
  uint32_t relay = 0;
  {
    std::lock_guard<std::mutex> l(core->mu);
    const uint32_t now = stamp();
    BoardRec &b = board(g, hd.sender);
    b.heardMs = now;
    b.heardAt = utcNow();
    if (hd.hops == 0) {
      b.directMs = now;
      b.rssi = f.rssi;
      b.rxRate = f.rate;
      b.noise = f.noise;
    } else {
      b.relayedMs = now;
      b.hops = hd.hops;
      b.relay = relay = macToId(f.mac);
    }
    noteShown(b, now);
  }

  // A packet for this board alone isn't relayed: the board it's for is the one its first field
  // after sentAt names (voice to one board, a pong, a ring, ringing, a ring end, a message ask), a
  // message frame's or end's second, type 15's the first after its own type (extFor()).
  // Presence, a mic's still-here and off, a ping and an answer flood regardless: every board of
  // the group follows whose mic is on, and an answer stops the ring on every board it rang.
  bool forUsAlone = false;
  switch (hd.type) {
    case kVoice:
    case kPong:
    case kRing:
    case kRinging:
    case kRingEnd:
    case kMessageAsk:
      forUsAlone = bodyLen >= 8 && get32(buf + kHeader + 4) == core->id;
      break;
    case kMessage:
    case kMessageEnd: forUsAlone = bodyLen >= 12 && get32(buf + kHeader + 8) == core->id; break;
    case kExt:
      forUsAlone = extFor(buf + kHeader, bodyLen) == core->id ||
                   reachExtFor(buf + kHeader, bodyLen) == core->id;
      break;
    default: break;
  }
  switch (hd.type) {
    case kPresence: onPresence(gi, hd, r); break;
    case kMic: onMicStill(gi, hd, f, r, relay); break;
    case kVoice: onVoice(gi, hd, f, r, relay); break;
    case kMicOff: onMicOff(gi, hd, r); break;
    case kPing: onPing(g, hd, f, r); break;
    case kPong: onPong(hd, f, r); break;
    case kRing: onRing(gi, hd, r); break;
    case kRinging: onRinging(hd, r); break;
    case kAnswer: onAnswer(gi, hd, r); break;
    case kRingEnd: onRingEnd(hd, r); break;
    case kMessage: onMessage(gi, hd, r); break;
    case kMessageEnd: onMessageEnd(gi, hd, r); break;
    case kMessageAsk: onMessageAsk(hd, r); break;
    case kExt: onExt(gi, hd, f, r); break;
    default: drop(kDropUnknownType); break;  // a later version's: passed on all the same
  }
  if (hd.ttl > 0 && !forUsAlone) relayFrame(f, hd, false);
}

// Reachable on battery (below, with the public functions).
void stepWoken(uint32_t now);
void noteHomeChannel(uint32_t now);
void takeWoke();

// --- The task ----------------------------------------------------------------------------------

// Under mu: 60 bars ending with `top`, oldest first, as comms.h has them.
std::vector<int8_t> barsOf(const MicHistory &h, uint16_t top) {
  std::vector<int8_t> out(kHistoryBars, kNoBar);
  for (int i = 0; i < kHistoryBars; ++i) {
    const int bar = static_cast<int>(top) - (kHistoryBars - 1) + i;
    if (bar < 0) continue;
    const uint8_t v = h.at(static_cast<uint16_t>(bar));
    if (v != kNoLevel) out[i] = static_cast<int8_t>(-static_cast<int>(std::min<uint8_t>(v, 96)));
  }
  return out;
}

// Under mu: how a board is reached, for the mic views.
void routeTo(int gi, uint32_t id, uint32_t now, Route &route, int &hops, int &rssi) {
  route = Route::Away;
  hops = rssi = 0;
  if (gi < 0 || gi >= static_cast<int>(core->groups.size())) return;
  if (const BoardRec *b = findBoard(core->groups[gi], id)) {
    route = routeOf(*b, now, hops);
    rssi = b->directMs ? b->rssi : 0;
  }
}

// A meter moved a segment (3 dB): a screen redraws it, at most 4 times a second.
void noteLevel(float level, float &shown, uint32_t &shownMs, uint32_t now) {
  if (fabsf(level - shown) < kMeterStepDb || now - shownMs < 250) return;
  shown = level;
  shownMs = now;
  bump();
}

// The delay from a mic to a speaker that plays it (comms.h).
uint32_t delayOf() { return kCaptureMs + kAirMs + kJitterMs + kQueueMs + kSpeakerMs; }

// Under mu: the bar a mic is on now (its history's top while it's off).
uint16_t barNow(uint32_t startMs, uint32_t now) {
  return static_cast<uint16_t>(std::min<uint32_t>((now - startMs) / kHistoryBarMs, 0xFFFF));
}

// Under mu.
void publishMic(uint32_t now) {
  const MicRec &m = core->mic;
  Mic x;
  x.on = m.on;
  x.count = core->micCount;
  if (core->micCount) {
    x.group = m.group;
    x.target = m.target;
    x.targetName = nameOf(m.target);
    x.conversation = m.conv;
    x.opened = m.opened;
    x.latched = m.on && m.latched;
    x.since = m.since;
    x.ms = m.on ? now - m.startMs : 0;
    x.capturing = m.on && m.capturing;
    x.levelDb = x.capturing ? m.level : -96;
    x.paused = m.on && !m.capturing ? m.paused : "";
    x.bars = barsOf(m.hist, m.on ? barNow(m.startMs, now) : m.hist.top());
    x.delayMs = delayOf();
    x.framesSent = m.framesSent;
    x.stillHeres = m.stillHeres;
    x.histories = m.histories;
    x.firstFrameMs = m.firstFrameMs;
    x.gainDb = core->agc.gainDb();
    x.roomDb = core->gate.roomDb();
    x.gateOpen = core->gate.open();
    x.vanished = m.vanished;
  }
  core->pubMic = std::move(x);
}

void publish() {
  const uint32_t now = millis();
  Status s;
  const MicRec &m = core->mic;
  const bool replay = core->replay.on;
  s.channel = radioChannel();
  s.onNetwork = WiFi.isConnected();
  s.netUp = false;
  s.longRange = isListening && core->rangeLong;
  std::lock_guard<std::mutex> l(core->mu);
  // The voices playing now, and the loudest of them.
  const OpenMicRec *loud = nullptr;
  float top = -97;
  for (const OpenMicRec &o : core->mics) {
    if (!o.playedMs || now - o.playedMs >= kPlayingMs) continue;
    ++s.voices;
    if (o.level > top) {
      top = o.level;
      loud = &o;
    }
  }
  s.hearing = s.voices > 0;
  s.talkedOver = m.on && core->talkOverMs && now - core->talkOverMs < kPlayingMs;
  if (s.talkedOver) {
    s.talkedOverBy = core->talkOverBy;
    s.talkedOverName = nameOf(core->talkOverBy);
  }
  s.playLevelDb = s.hearing || replay ? core->level : -96;
  s.levelDb = s.playLevelDb;
  s.state = !isListening                  ? State::Off
            : m.on || core->out.recording ? State::Mic
            : s.hearing || replay         ? State::Hearing
                                          : State::Quiet;
  s.group = core->active;
  s.replay = replay;
  if (m.on) {
    s.group = m.group;
    s.peer = m.target;
    s.peerName = nameOf(m.target);
    s.toAll = m.target == 0;
    s.ms = now - m.startMs;
    s.levelDb = m.capturing ? m.level : -96;
  } else if (core->out.recording) {  // the mic is on for a message: Mic, as for an open mic
    s.group = core->out.group;
    s.peer = core->out.to;
    s.peerName = nameOf(core->out.to);
    s.toAll = core->out.to == 0;
    s.ms = now - core->out.startMs;
    s.levelDb = core->msgLevel;
  } else if (loud) {
    s.group = loud->group;
    s.peer = loud->id;
    s.peerName = nameOf(loud->id);
    s.toAll = loud->target == 0;
    s.ms = now - loud->st.startMs();
    routeTo(loud->group, loud->id, now, s.route, s.hops, s.rssi);
    if (s.route == Route::Relayed) {
      if (const BoardRec *b = findBoard(core->groups[loud->group], loud->id)) {
        s.relayName = nameOf(b->relay);
      }
    }
  } else if (replay) {
    s.group = core->replay.group;
    s.peer = core->replay.from;
    s.peerName = nameOf(core->replay.from);
    s.ms = core->replay.frames * kFrameMs;
  }
  if (replay) s.playingMessage = core->replay.message;
  s.incoming = core->incoming;
  s.ringsSeen = core->ringsSeen;
  s.conversationsSeen = core->conversationsSeen;
  s.conversationGroup = core->conversationGroup;
  s.messagesIn = core->messagesIn;
  s.alarm = core->alarm;
  s.error = core->error;
  core->pub = std::move(s);

  const Dialing &d = core->dialing;
  const Outbound &o = core->out;
  Dial dl;
  dl.state = d.state;
  dl.group = d.group;
  dl.target = d.target;
  dl.targetName = nameOf(d.target);
  dl.ms = d.state == DialState::Ringing ? now - d.startMs : d.endMs - d.startMs;
  dl.ringing = d.state == DialState::Ringing ? ringingNow(d, now) : 0;
  dl.answeredBy = d.answeredBy;
  dl.answeredByName = nameOf(d.answeredBy);
  dl.canLeaveMessage = (d.state == DialState::Ringing && now - d.startMs >= kMessageAfterMs) ||
                       d.state == DialState::NoAnswer;
  dl.messageMs = o.recording ? now - o.startMs : o.frames * kFrameMs;
  dl.delivered = static_cast<int>(o.delivered.size());
  dl.count = core->dialCount;
  core->pubDial = std::move(dl);

  const Ringing &rg = core->ringing;
  Ring rn;
  rn.on = rg.on;
  rn.group = rg.group;
  rn.from = rg.from;
  rn.fromName = nameOf(rg.from);
  rn.toAll = rg.toAll;
  rn.ms = rg.on ? now - rg.startMs : rg.lastMs - rg.startMs;
  rn.since = rg.at;
  rn.count = core->ringsSeen;
  rn.ended = rg.ended;
  core->pubRing = std::move(rn);
  publishMic(now);
  core->last = LastCall();
  if (core->heardCount) {
    core->last.have = true;
    core->last.group = core->heardGroup;
    core->last.from = core->heardFrom;
    core->last.fromName = nameOf(core->heardFrom);
    core->last.ms = static_cast<uint32_t>(core->heardCount) * kFrameMs;
    const time_t utc = utcNow();
    core->last.at = utc ? utc - static_cast<time_t>((now - core->heardLastMs + core->last.ms) / 1000)
                        : 0;
  }
}

void tick(uint32_t now) {
  noteHomeChannel(now);  // for this board's polls when it sleeps reachable
  // Boards that went away.
  {
    std::lock_guard<std::mutex> l(core->mu);
    for (GroupRec &g : core->groups) {
      for (BoardRec &b : g.boards) noteShown(b, now);
    }
  }
  tickMics(now);
  // Conversations: members that went quiet, conversations over; one this board isn't in that
  // has come on is counted once (status().conversationsSeen: the screens offer to join it).
  {
    std::lock_guard<std::mutex> l(core->mu);
    for (size_t i = 0; i < core->groups.size(); ++i) {
      GroupRec &g = core->groups[i];
      ConvState &c = g.conv;
      MicRec &m = core->mic;
      if (m.on && m.group == static_cast<int>(i)) {
        // This board's open mic keeps it on (and opens it again should it have ended).
        if (!c.on() || !c.mine()) c.join(now, c.on() ? 0 : newConvId());
        c.spoke(now, c.id(), core->id, core->id);
        m.conv = c.id();
      }
      if (c.expire(now)) bump();
      if (c.on() && !c.mine() && !c.left() && c.id() != g.convSeen) {
        g.convSeen = c.id();
        ++core->conversationsSeen;
        core->conversationGroup = static_cast<int>(i);
        log_i("comms: a conversation is on in %s (%08lX)", g.name.c_str(),
              static_cast<unsigned long>(c.id()));
        bump();
      } else if (c.on() && c.mine()) {
        g.convSeen = c.id();  // in it: no offer to join it later
      }
    }
  }
  static uint32_t lastExpire = 0;
  if (now - lastExpire >= 60000) {
    lastExpire = now;
    expireMsgs();
  }
}

// The board keeps its radio on and doesn't power off by itself: its mic is on, LOST's alarm
// sounds, or it's in a conversation where another board's mic is on.
bool awakeNow() {
  if (core->mic.on || core->alarm) return true;
  std::lock_guard<std::mutex> l(core->mu);
  for (const OpenMicRec &o : core->mics) {
    if (o.st.on() && heardHere(o)) return true;
  }
  return false;
}

// Presence: at the start a hello, then every kPresenceMs (kConvPresenceMs while this board is in
// the group's conversation); and the answer to a hello.
void presence(uint32_t now) {
  for (GroupRec &g : core->groups) {
    if (g.via == Via::Net) continue;
    if (static_cast<int32_t>(now - g.presenceDue) >= 0) {
      sendPresence(g, g.hello);
      g.hello = false;
      bool inConv;
      {
        std::lock_guard<std::mutex> l(core->mu);
        inConv = g.conv.mine();
      }
      g.presenceDue = now + (inConv ? kConvPresenceMs + esp_random() % 1000
                                    : kPresenceMs + esp_random() % 3000);
      g.answerDue = 0;
      g.answeredMs = now;
    } else if (g.answerDue && static_cast<int32_t>(now - g.answerDue) >= 0) {
      sendPresence(g, false);
      g.answerDue = 0;
      g.answeredMs = now;
    }
  }
}

void startListening() {
  if (!radioStart()) return;
  const uint32_t now = millis();
  core->linksDue = now + 300 + esp_random() % 700;
  for (GroupRec &g : core->groups) {
    g.hello = true;
    g.presenceDue = now + esp_random() % 300;
  }
  bump();
}

void stopListening() {
  stopMic(kOffSwitched);  // the radio goes (groups changed): the mic with it
  if (core->out.recording) finishMessage(true);
  stopReplay();
  core->mixer.clear();
  if (core->duplexOurs) {
    audio::duplexStop();
    core->duplexOurs = false;
  }
  if (core->dialing.state == DialState::Ringing) dialOver(DialState::None, kEndGaveUp);
  ringingOver(RingEnd::Lost);
  for (Inbound &in : core->inbound) finishInbound(in);
  {
    std::lock_guard<std::mutex> l(core->mu);
    for (OpenMicRec &o : core->mics) {
      endHeardSession(o, o.st.session(), o.st.heardMs() - o.st.startMs());
    }
    core->mics.clear();
    core->alarm = false;
    core->links.clear();
  }
  radioStop();
  bump();
}

void reload() {
  if (isListening) stopListening();
  renew(core->dialing);
  renew(core->ringing);
  renew(core->out);
  std::vector<GroupRec> loaded = loadGroups();
  {
    std::lock_guard<std::mutex> l(core->mu);
    freeGroups(core->groups);
    core->groups = std::move(loaded);
    core->log.clear();
    core->pongs.clear();
    core->last = LastCall();
    core->heardAudio.clear();
    core->heardHead = core->heardCount = 0;
    // Messages kept stay, under their group's new place (hidden while it isn't configured).
    for (MsgRec &m : core->msgs) {
      m.group = groupIndex(m.e.group);
      if (m.group >= 0) logMsg(m, false);
    }
    const uint32_t want = kv::loadU32(kNs, "active");
    core->active = 0;
    for (size_t i = 0; i < core->groups.size(); ++i) {
      if (core->groups[i].id == want) core->active = static_cast<int>(i);
    }
    core->devChannel = static_cast<int>(kv::loadU32(kNs, "devchan"));
  }
  hasGroups = !core->groups.empty();
  core->seen.clear();
  log_i("comms: %u groups", static_cast<unsigned>(core->groups.size()));
  bump();
}

void handleCommand(const Command &c) {
  switch (c.cmd) {
    case Cmd::Wake: break;
    case Cmd::MicOn: {
      uint32_t target;
      {
        std::lock_guard<std::mutex> l(core->mu);
        target = core->micWantTarget;
      }
      startMic(c.group, target);
      break;
    }
    case Cmd::MicOff:
      stopMic(kOffSwitched);
      micWanted = false;
      break;
    case Cmd::MicLatch: latchMicNow(); break;
    case Cmd::MicVanish:
      core->mic.vanished = c.arg != 0;
      log_i("comms: the mic %s", c.arg ? "vanishes: nothing goes out, its off neither"
                                       : "is back on air");
      bump();
      break;
    case Cmd::MicTone:
      if (core->mic.on) {
        core->mic.toneUntilMs = (millis() + c.arg) | 1;
        core->mic.toneAmp = std::min(32767.0f, 32767.0f * powf(10, c.group / 20.0f));
        log_i("comms: a 1 kHz tone at %d dBFS added to the mic for %lu ms", c.group,
              static_cast<unsigned long>(c.arg));
      }
      break;
    case Cmd::AckLost: {
      bool any = false;
      {
        std::lock_guard<std::mutex> l(core->mu);
        for (OpenMicRec &o : core->mics) any = o.st.ack() || any;
        core->alarm = false;
      }
      if (any) {
        log_i("comms: LOST's alarm silenced");
        bump();
      }
      break;
    }
    case Cmd::PlayLast: {
      // What's kept, oldest first, as it is now (live voice goes on adding to the ring).
      const size_t n = core->heardCount;
      core->replayAudio.resize(n * kStoredFrame);
      const size_t first = (core->heardHead + kLastFrames - n) % kLastFrames;
      for (size_t i = 0; i < n; ++i) {
        memcpy(&core->replayAudio[i * kStoredFrame],
               &core->heardAudio[((first + i) % kLastFrames) * kStoredFrame], kStoredFrame);
      }
      if (core->replay.on && !core->replay.message) stopReplay();
      startReplay(core->replayAudio, 0, core->heardGroup, core->heardFrom);
      break;
    }
    case Cmd::StopPlay: stopReplay(); break;
    case Cmd::Ping: {
      if (c.group < 0 || c.group >= static_cast<int>(core->groups.size())) break;
      core->pingId = c.arg;
      uint8_t body[12];
      Writer w(body, sizeof body);
      w.u32(sentAt());
      w.u32(c.arg);
      w.u32(millis());
      sendOwn(core->groups[c.group], kPing, body, w.size());
      break;
    }
    case Cmd::Reload: reload(); break;
    case Cmd::Ring: startRing(c.group, c.arg); break;
    case Cmd::RingStop: {
      Dialing &d = core->dialing;
      if (d.state == DialState::Ringing) {
        dialOver(DialState::None, kEndGaveUp);
      } else if (d.state == DialState::Recording) {
        finishMessage(false);
      } else if (d.state != DialState::None) {
        d.state = DialState::None;
        bump();
      }
      break;
    }
    case Cmd::Answer: answerRing(); break;
    case Cmd::Ignore: ignoreRinging(); break;
    case Cmd::Join: joinConv(c.group); break;
    case Cmd::Leave: leaveConv(c.group); break;
    case Cmd::MessageStart: startMessage(c.group, c.arg); break;
    case Cmd::MessageSend: finishMessage(true); break;
    case Cmd::MessageCancel: finishMessage(false); break;
    case Cmd::PlayMessage: playMsg(c.arg); break;
    case Cmd::PowerOff: micPowerOff(); break;
    case Cmd::Range: {
      {
        std::lock_guard<std::mutex> g(radioMu);
        if (espnowUp) applyRadio(true, c.arg == 1);
      }
      for (GroupRec &gr : core->groups) presenceSoon(gr);  // the others see the rate at once
      bump();
      break;
    }
    case Cmd::Fetch: core->fetchGroup = c.group; break;  // once listening (below)
    case Cmd::RingVanish:
      if (core->dialing.state == DialState::Ringing) {
        core->dialing.vanished = true;
        log_i("comms: ring packets stop here (no cancel), as if this board had gone");
      }
      break;
    case Cmd::Resend: {
      uint8_t f[kMaxFrame];
      const uint8_t n = c.arg ? core->firstVoiceLen : core->lastSentLen;
      memcpy(f, c.arg ? core->firstVoice : core->lastSent, n);
      if (n) radioSend(f, n);
      log_i("comms: sent again, byte for byte: %s (%u bytes)",
            c.arg ? "the first voice frame of the last session" : "the last frame", n);
      break;
    }
  }
}

void task(void *) {
  uint32_t lastUsb = 0, lastSleep = 0, lastChannel = 0, lastTick = 0, lastBump = 0,
           lastPublish = 0, lingerFrom = 0, lastAwake = 0;
  bool usbKnown = false, awakeWas = false;
  for (;;) {
    uint32_t now = millis();
    if (!usbKnown || now - lastUsb >= kUsbPollMs) {
      const pmic::Status p = pmic::status();
      const bool wasUsb = core->onUsb;
      core->onUsb = p.ok && p.vbus;
      // USB power: the reachable log starts again at the next sleep (it stays to be read till then).
      if (core->onUsb && (!wasUsb || !usbKnown)) kv::saveU32("reach", "usb", 1);
      core->battery = p.ok && p.battery ? p.percent : -1;
      lastUsb = now;
      usbKnown = true;
    }
    static uint32_t lastWoken = 0;
    if (now - lastWoken >= 1000) {  // up after a wake (reachSleep()): how long yet
      lastWoken = now;
      stepWoken(now);
    }
    if (now - lastAwake >= 250) {
      lastAwake = now;
      awakeWas = awakeNow();
      awake = awakeWas;
    }
    bool inbound = false;
    for (const Inbound &in : core->inbound) inbound = inbound || in.used;
    const bool ringOn = core->dialing.state == DialState::Ringing || core->ringing.on;
    const bool msgOn = core->out.recording || core->out.resending || inbound;
    const bool voice = micWanted || core->mic.on || core->mixer.active() || core->replay.on ||
                       core->duplexOurs;
    const bool busy = voice || ringOn || msgOn || awakeWas;  // on battery too
    const bool want =
        hasGroups && !poweringOff && !radioLent && (holders.load() > 0 || core->onUsb || busy);
    if (radioLent && isListening) stopListening();  // at once: the sleeping loop takes the radio
    if (want) lingerFrom = now;
    if (want && !isListening) {
      static uint32_t lastTry = 0;
      if (!lastTry || now - lastTry >= 5000) {
        lastTry = now ? now : 1;
        startListening();
      }
    } else if (!want && isListening && now - lingerFrom >= kLingerMs) {
      stopListening();
    }

    // Commands first; while not listening, they're all there is to wait for.
    Command cmd;
    const TickType_t idleWait = pdMS_TO_TICKS(isListening ? 0 : 250);
    while (xQueueReceive(commands, &cmd, idleWait) == pdTRUE) {
      handleCommand(cmd);
      if (!isListening) break;
    }
    if (!isListening) {
      resumeMusicWhenFree();
      publish();
      continue;
    }

    // Frames from the radio: quick turns while the mic captures or voice plays, so the mic and
    // the speaker keep up, and while a message is recorded or frames of one go again.
    const bool quick = core->mic.capturing || core->mixer.active() || core->replay.on ||
                       core->out.recording || core->out.resending;
    RxFrame &frame = core->rx;
    if (xQueueReceive(rxQueue, &frame, pdMS_TO_TICKS(quick ? 4 : 20)) == pdTRUE) {
      int n = 0;
      do {
        handleFrame(frame);
      } while (++n < 16 && xQueueReceive(rxQueue, &frame, 0) == pdTRUE);
    }
    now = millis();
    if (core->fetchGroup >= 0) {  // the message a poll's answer woke this board for
      fetchWokeMessage(core->fetchGroup);
      core->fetchGroup = -1;
    }
    sendPending(now);
    pumpMic(now);
    pumpPlay(now);
    pumpMessageOut(now);
    pumpDial(now);
    pumpRinging(now);
    pumpInbound(now);
    pumpDuplex(now);
    pumpAlarm(now);
    resumeMusicWhenFree();
    presence(now);
    if (static_cast<int32_t>(now - core->linksDue) >= 0) {
      sendLinks();
      core->linksDue = now + kLinksMs + esp_random() % 1000;
    }
    if (now - lastSleep >= kSleepCheckMs) {
      lastSleep = now;
      if (WiFi.getSleep() != WIFI_PS_NONE) {  // another feature turned it on
        WiFi.setSleep(false);
        ++core->sleepFixes;
      }
    }
    if (now - lastChannel >= kChannelCheckMs) {
      lastChannel = now;
      keepChannel();
      std::lock_guard<std::mutex> g(radioMu);
      if (espnowUp) applyRadio(false);
    }
    if (now - lastTick >= kTickMs) {
      lastTick = now;
      tick(now);
    }
    const bool live = core->mic.on || core->mixer.active() || core->replay.on || ringOn ||
                      core->out.recording;
    if (live && now - lastBump >= kCallBumpMs) {
      lastBump = now;
      bump();
    }
    if (now - lastPublish >= (live ? 50u : 200u)) {
      lastPublish = now;
      publish();
    }
    MicRec &m = core->mic;
    noteLevel(m.on && m.capturing ? m.level : -96, m.shownLevel, m.shownMs, now);
    {
      std::lock_guard<std::mutex> l(core->mu);
      for (OpenMicRec &o : core->mics) {
        const float lv = o.playedMs && now - o.playedMs < kPlayingMs ? o.level
                                                                      : levelDb(o.last.peak);
        noteLevel(lv, o.shownLevel, o.shownMs, now);
      }
    }
  }
}

// The task's stack, in internal RAM. Writing a message to the card (FatFs keeps long names on the
// stack) needs more than the calls alone did.
constexpr uint32_t kStack = 7168;

bool startTask() {
  if (taskHandle) return true;
  if (!rxQueue) {  // reachSleep() may have made it
    rxQueue = xQueueCreateWithCaps(kRxDepth, sizeof(RxFrame), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  }
  if (!rxQueue) {
    log_e("comms: no memory for the receive queue");
    return false;
  }
  if (xTaskCreate(task, "comms", kStack, nullptr, 3, &taskHandle) != pdPASS) {
    log_e("comms: no memory for the task");
    taskHandle = nullptr;
    return false;
  }
  return true;
}

void ensureCore() {
  if (core) return;
  core = new Core();
  core->id = loadId();
  core->name = makeName(core->id);
  core->relayOn = kv::loadU32(kNs, "norelay") == 0;
  core->chirpOn = kv::loadU32(kNs, "nochirp") == 0;
  core->devChannel = static_cast<int>(kv::loadU32(kNs, "devchan"));
  commands = xQueueCreate(8, sizeof(Command));
  crypto::GroupKeys lk = crypto::linkKeys();
  core->linkGroup = lk.id;
  core->linkSealer = new crypto::Sealer();
  core->linkSealer->setKey(lk.key);
  mbedtls_platform_zeroize(lk.key, sizeof lk.key);
}

}  // namespace

const char *viaName(Via v) {
  switch (v) {
    case Via::Both: return "AIR+NET";
    case Via::Air: return "AIR";
    case Via::Net: return "NET";
  }
  return "";
}

void begin() {
  if (taskHandle) return;
  ensureCore();
  {
    std::vector<GroupRec> g = loadGroups();
    std::lock_guard<std::mutex> l(core->mu);
    freeGroups(core->groups);
    core->groups = std::move(g);
    const uint32_t want = kv::loadU32(kNs, "active");
    for (size_t i = 0; i < core->groups.size(); ++i) {
      if (core->groups[i].id == want) core->active = static_cast<int>(i);
    }
  }
  hasGroups = !core->groups.empty();
  if (!hasGroups) {
    log_i("comms: no group (no mesh_group line or agent_token): off");
    return;
  }
  loadMsgs();
  core->boot = nextBoot();
  // Ring numbers start anywhere: a board that rang before a restart doesn't reuse the numbers
  // the others remember as over.
  core->ringCounter = static_cast<uint16_t>(esp_random() ^ core->boot);
  core->micSession = static_cast<uint16_t>(esp_random());
  log_i("comms: %u groups, board %s (%08lX), boot %lu", static_cast<unsigned>(core->groups.size()),
        core->name.c_str(), static_cast<unsigned long>(core->id),
        static_cast<unsigned long>(core->boot));
  takeWoke();  // woke for comms (reachSleep()): the hold, and a message to ask for (before the task)
  startTask();
}

bool configured() { return hasGroups.load(); }

void hold() {
  holders.fetch_add(1);
  post(Cmd::Wake, 0, 0, 0);  // starts the radio now rather than at the next turn; never waits
}

void release() {
  int h = holders.load();
  while (h > 0 && !holders.compare_exchange_weak(h, h - 1)) {
  }
}

uint32_t generation() { return gen.load(std::memory_order_relaxed); }

std::vector<Group> groups() {
  std::vector<Group> out;
  if (!core) return out;
  const uint32_t now = millis();
  std::lock_guard<std::mutex> l(core->mu);
  for (size_t i = 0; i < core->groups.size(); ++i) {
    const GroupRec &g = core->groups[i];
    Group x;
    x.name = g.name;
    crypto::tag(g.id, x.tag);
    x.id = g.id;
    x.via = g.via;
    x.cardVia = g.cardVia;
    x.fromToken = g.fromToken;
    x.hub = g.hub;
    char hex[3];
    for (const uint8_t b : g.rendezvous) {
      snprintf(hex, sizeof hex, "%02x", b);
      x.rendezvous += hex;
    }
    x.boards = static_cast<int>(g.boards.size());
    for (const BoardRec &b : g.boards) {
      int hops;
      const Route r = routeOf(b, now, hops);
      if (r == Route::Air || r == Route::Relayed) ++x.onAir;
    }
    for (const CallRec &c : core->log) {
      if (c.group == static_cast<int>(i) && !c.outgoing && !c.heard && !c.live) ++x.unheard;
    }
    for (const OpenMicRec &o : core->mics) {
      if (o.group == static_cast<int>(i)) ++x.micsOn;
    }
    x.active = static_cast<int>(i) == core->active;
    out.push_back(std::move(x));
  }
  return out;
}

int activeGroup() {
  if (!core) return 0;
  std::lock_guard<std::mutex> l(core->mu);
  return core->active;
}

void setActiveGroup(int group) {
  if (!core) return;
  uint32_t id = 0;
  {
    std::lock_guard<std::mutex> l(core->mu);
    if (group < 0 || group >= static_cast<int>(core->groups.size()) || group == core->active) {
      return;
    }
    core->active = group;
    id = core->groups[group].id;
  }
  kv::saveU32(kNs, "active", id);
  bump();
}

void setVia(int group, Via via) {
  if (!core) return;
  uint32_t record = 0;
  {
    std::lock_guard<std::mutex> l(core->mu);
    if (group < 0 || group >= static_cast<int>(core->groups.size())) return;
    GroupRec &g = core->groups[group];
    if (g.via == via) return;
    g.via = via;
    record = via == g.cardVia ? 0 : viaRecord(g.line, via);
  }
  char key[8];
  snprintf(key, sizeof key, "via%d", group);
  kv::saveU32(kNs, key, record);
  bump();
}

void cycleVia(int group) {
  if (!core) return;
  Via v;
  {
    std::lock_guard<std::mutex> l(core->mu);
    if (group < 0 || group >= static_cast<int>(core->groups.size())) return;
    v = core->groups[group].via;
  }
  setVia(group, v == Via::Both ? Via::Air : v == Via::Air ? Via::Net : Via::Both);
}

std::vector<Board> boards(int group) {
  std::vector<Board> out;
  if (!core) return out;
  const uint32_t now = millis();
  std::lock_guard<std::mutex> l(core->mu);
  if (group < 0 || group >= static_cast<int>(core->groups.size())) return out;
  for (const BoardRec &b : core->groups[group].boards) {
    Board x;
    x.id = b.id;
    x.name = b.name;
    x.route = routeOf(b, now, x.hops);
    if (x.route == Route::Relayed) {
      x.relay = b.relay;
      x.relayName = nameOf(b.relay);
    }
    x.rssi = b.directMs ? b.rssi : 0;
    x.rxRate = b.directMs ? rateName(b.rxRate) : "";
    x.noise = b.directMs ? b.noise : 0;
    x.heardAt = b.heardAt;
    x.heardAgoS = b.heardMs ? (now - b.heardMs) / 1000 : 0;
    x.talkedAt = b.talkedAt;
    x.flags = b.flags;
    x.battery = b.battery;
    // Asleep while its polls come: three missed and it's no longer said to be.
    x.asleep = b.pollMs && now - b.pollMs < static_cast<uint32_t>(b.pollS) * 3000 + 5000;
    x.pollS = x.asleep ? b.pollS : 0;
    out.push_back(std::move(x));
  }
  std::stable_sort(out.begin(), out.end(), [](const Board &a, const Board &b) {
    auto rank = [](const Board &x) {
      switch (x.route) {
        case Route::Air: return 0;
        case Route::Relayed: return 1;
        case Route::Net: return 2;
        case Route::Away: return 3;
      }
      return 3;
    };
    if (rank(a) != rank(b)) return rank(a) < rank(b);
    if (a.route == Route::Air) return a.rssi > b.rssi;
    if (a.route == Route::Relayed && a.hops != b.hops) return a.hops < b.hops;
    return a.heardAgoS < b.heardAgoS;
  });
  return out;
}

Status status() {
  if (!core) return Status();
  std::lock_guard<std::mutex> l(core->mu);
  return core->pub;
}

std::vector<Call> calls(int group, int max) {
  std::vector<Call> out;
  if (!core) return out;
  const uint32_t now = millis();
  std::lock_guard<std::mutex> l(core->mu);
  for (auto it = core->log.rbegin(); it != core->log.rend() && static_cast<int>(out.size()) < max;
       ++it) {
    const CallRec &c = *it;
    if (group >= 0 && c.group != group) continue;
    Call x;
    x.kind = c.kind;
    x.at = c.at;
    const time_t utc = utcNow();
    x.agoS = c.startMs ? (now - c.startMs) / 1000
             : c.at && utc > c.at ? static_cast<uint32_t>(utc - c.at)
                                  : 0;
    x.group = c.group;
    x.outgoing = c.outgoing;
    x.from = c.from;
    x.fromName = nameOf(c.from);
    x.to = c.to;
    x.toName = nameOf(c.to);
    x.ms = c.live ? now - c.startMs : c.ms;
    x.live = c.live;
    x.route = c.hops ? Route::Relayed : Route::Air;
    x.hops = c.hops;
    x.relayName = nameOf(c.relay);
    x.rssi = c.rssi;
    x.played = c.played;
    x.heard = c.heard;
    x.linkLost = c.linkLost;
    x.cutOff = c.cutOff;
    x.heardBy = static_cast<int>(c.heardBy.size());
    x.lost = c.lost;
    x.conversation = c.conv;
    x.answered = c.answered;
    x.missed = c.missed;
    x.complete = c.complete;
    x.message = c.message;
    if (c.kind == Kind::Message && !c.live) x.ms = c.ms;
    if (c.kind == Kind::Message && c.live && !c.outgoing) {  // coming: as much as has come
      if (const MsgRec *m = findMsg(c.message)) x.ms = m->e.frames * kFrameMs;
    }
    out.push_back(std::move(x));
  }
  return out;
}

void markHeard(int group) {
  if (!core) return;
  std::lock_guard<std::mutex> l(core->mu);
  bool changed = false;
  for (CallRec &c : core->log) {
    if ((group < 0 || c.group == group) && !c.outgoing && !c.heard && !c.live &&
        c.kind != Kind::Message) {  // a message is heard once it has played
      c.heard = true;
      changed = true;
    }
  }
  if (changed) bump();
}

LastCall lastCall() {
  if (!core) return LastCall();
  std::lock_guard<std::mutex> l(core->mu);
  LastCall c = core->last;
  if (c.have) c.fromName = nameOf(c.from);
  return c;
}

bool playLast() {
  if (!core) return false;
  {
    std::lock_guard<std::mutex> l(core->mu);
    if (!core->last.have) {
      core->error = core->pub.error = "NOTHING HEARD TO PLAY";
      bump();
      return false;
    }
  }
  post(Cmd::PlayLast);
  return true;
}

void stopPlaying() { post(Cmd::StopPlay); }

Conversation conversation(int group) {
  Conversation out;
  if (!core) return out;
  const uint32_t now = millis();
  std::lock_guard<std::mutex> l(core->mu);
  if (group < 0 || group >= static_cast<int>(core->groups.size())) return out;
  const ConvState &c = core->groups[group].conv;
  if (!c.on()) return out;
  out.on = true;
  out.id = c.id();
  out.in = c.mine();
  out.left = c.left();
  for (size_t i = 0; i < c.count(); ++i) {
    out.members.push_back(c.member(i).id);
    out.names.push_back(nameOf(c.member(i).id));
  }
  if (core->pubMic.on && core->pubMic.group == group) ++out.micsOn;
  for (const OpenMicRec &o : core->mics) {
    if (o.group == group && o.st.on()) ++out.micsOn;
  }
  out.ms = now - c.startMs();
  out.quietMs = out.micsOn ? 0 : now - c.lastWordMs();
  const time_t utc = utcNow();
  out.since = utc ? utc - static_cast<time_t>(out.ms / 1000) : 0;
  return out;
}

bool join(int group) {
  if (!core) return false;
  {
    std::lock_guard<std::mutex> l(core->mu);
    if (group < 0 || group >= static_cast<int>(core->groups.size()) ||
        !core->groups[group].conv.on()) {
      core->error = core->pub.error = "NO CONVERSATION ON";
      bump();
      return false;
    }
  }
  post(Cmd::Join, group);
  return true;
}

void leave(int group) { post(Cmd::Leave, group); }

Dial dial() {
  if (!core) return Dial();
  std::lock_guard<std::mutex> l(core->mu);
  return core->pubDial;
}

bool ringStart(int group, uint32_t target) {
  if (!core) return false;
  std::lock_guard<std::mutex> l(core->mu);
  const char *why = !hasGroups ? "NO GROUP SET"
                    : group < 0 || group >= static_cast<int>(core->groups.size())
                        ? "NO SUCH GROUP"
                    : core->groups[group].via == Via::Net ? "NET ONLY: NO LINK YET"
                    : core->pubDial.state == DialState::Ringing ? "ALREADY RINGING"
                    : core->pubDial.state == DialState::Recording ? "A MESSAGE IS GOING"
                                                                  : nullptr;
  if (why) {
    core->error = core->pub.error = why;
    bump();
    return false;
  }
  post(Cmd::Ring, group, target, 0);
  return true;
}

void ringStop() { post(Cmd::RingStop); }

Ring incomingRing() {
  if (!core) return Ring();
  std::lock_guard<std::mutex> l(core->mu);
  return core->pubRing;
}

bool answer() {
  if (!core) return false;
  {
    std::lock_guard<std::mutex> l(core->mu);
    if (!core->pubRing.on) return false;
  }
  post(Cmd::Answer);
  return true;
}

void ignoreRing() { post(Cmd::Ignore); }

bool messageStart(int group, uint32_t target) {
  if (!core) return false;
  {
    std::lock_guard<std::mutex> l(core->mu);
    const char *why = nullptr;
    if (!hasGroups) {
      why = "NO GROUP SET";
    } else if (group < 0 && !core->pubDial.canLeaveMessage) {
      why = "NO RING TO LEAVE A MESSAGE FOR";
    } else if (group >= static_cast<int>(core->groups.size())) {
      why = "NO SUCH GROUP";
    } else if (core->pubDial.state == DialState::Recording) {
      why = "A MESSAGE IS GOING";
    }
    if (why) {
      core->error = core->pub.error = why;
      bump();
      return false;
    }
  }
  if (audio::recording()) {
    std::lock_guard<std::mutex> l(core->mu);
    core->error = core->pub.error = "THE RECORDER IS ON";
    bump();
    return false;
  }
  post(Cmd::MessageStart, group, target);
  return true;
}

void messageSend() { post(Cmd::MessageSend); }

void messageCancel() { post(Cmd::MessageCancel); }

std::vector<Message> messages(int group) {
  std::vector<Message> out;
  if (!core) return out;
  const uint32_t now = millis();
  const time_t utc = utcNow();
  std::lock_guard<std::mutex> l(core->mu);
  for (auto it = core->msgs.rbegin(); it != core->msgs.rend(); ++it) {
    const MsgRec &m = *it;
    if (m.group < 0 || (group >= 0 && m.group != group)) continue;
    Message x;
    x.id = m.local;
    x.group = m.group;
    x.from = m.e.sender;
    x.fromName = nameOf(m.e.sender);
    if (x.fromName.empty()) x.fromName = m.e.fromName;
    x.to = m.e.to;
    x.at = m.e.at;
    x.agoS = m.arrivedMs ? (now - m.arrivedMs) / 1000
             : m.e.at && utc > m.e.at ? static_cast<uint32_t>(utc - m.e.at)
                                      : 0;
    x.ms = m.e.frames * kFrameMs;
    x.receiving = m.receiving;
    x.complete = m.e.complete;
    x.played = m.e.played;
    x.onCard = m.onCard;
    x.frames = m.e.frames;
    x.sum = m.e.sum;
    out.push_back(std::move(x));
  }
  return out;
}

bool playMessage(uint32_t id) {
  if (!core) return false;
  {
    std::lock_guard<std::mutex> l(core->mu);
    const MsgRec *m = findMsg(id);
    if (!m || m->receiving) {
      core->error = core->pub.error = "NO SUCH MESSAGE";
      bump();
      return false;
    }
  }
  post(Cmd::PlayMessage, 0, id);
  return true;
}

void ringVanish() { post(Cmd::RingVanish); }

void dropMessageFrames(int nth) { dropNth = std::max(0, nth); }

// --- The mic -----------------------------------------------------------------------------------

namespace {

bool refuse(const char *why) {
  std::lock_guard<std::mutex> l(core->mu);
  core->error = core->pub.error = why;
  bump();
  return false;
}

}  // namespace

bool micOn(int group, uint32_t target) {
  if (!core) return false;
  if (!hasGroups) return refuse("NO GROUP SET");
  {
    std::lock_guard<std::mutex> l(core->mu);
    const char *why = group < 0 || group >= static_cast<int>(core->groups.size())
                          ? "NO SUCH GROUP"
                      : core->groups[group].via == Via::Net ? "NET ONLY: NO LINK YET"
                      : target == core->id                  ? "THAT'S THIS BOARD"
                                                            : nullptr;
    if (why) {
      core->error = core->pub.error = why;
      bump();
      return false;
    }
    core->micWantTarget = target;
  }
  if (audio::recording()) return refuse("THE RECORDER IS ON");
  if (audio::ringing()) return refuse("AN ALARM IS RINGING");
  micWanted = true;
  post(Cmd::MicOn, group, target);
  return true;
}

void micOff() {
  micWanted = false;
  post(Cmd::MicOff);
}

bool micEnabled() { return micWanted.load(); }

void latchMic() { post(Cmd::MicLatch); }

Mic mic() {
  if (!core) return Mic();
  std::lock_guard<std::mutex> l(core->mu);
  return core->pubMic;
}

std::vector<OpenMic> openMics(int group) {
  std::vector<OpenMic> out;
  if (!core) return out;
  const uint32_t now = millis();
  const time_t utc = utcNow();
  std::lock_guard<std::mutex> l(core->mu);
  for (const OpenMicRec &o : core->mics) {
    if (group >= 0 && o.group != group) continue;
    OpenMic x;
    x.group = o.group;
    x.id = o.id;
    x.name = nameOf(o.id);
    x.target = o.target;
    x.toMe = o.target == core->id;
    x.forMe = forMe(o);
    x.heard = heardHere(o);
    x.playing = o.playedMs && now - o.playedMs < kPlayingMs;
    x.opened = o.opened;
    x.latched = o.st.latched();
    x.conversation = o.conv;
    x.link = o.st.link() == HeardMic::Link::Lost ? Link::Lost : Link::On;
    x.linkMs = now - o.st.sinceMs();
    x.onS = (now - o.st.startMs()) / 1000;
    x.since = utc ? utc - static_cast<time_t>(x.onS) : 0;
    x.levelDb = x.playing ? o.level : levelDb(o.last.peak);
    x.paused = o.last.flags & kMicPaused;
    x.alarm = o.st.alarm();
    x.battery = o.haveStill && o.last.battery <= 100 ? o.last.battery : -1;
    x.usb = o.last.flags & kMicUsb;
    x.heardAgoMs = now - o.st.heardMs();
    routeTo(o.group, o.id, now, x.route, x.hops, x.rssi);
    x.delayMs = delayOf();
    x.bars = barsOf(o.hist, x.link == Link::Lost ? o.hist.top() : barNow(o.st.startMs(), now));
    const auto &ev = o.st.events();
    for (auto it = ev.rbegin(); it != ev.rend(); ++it) {
      MicEvent e;
      e.agoS = (now - it->startMs) / 1000;
      e.at = utc ? utc - static_cast<time_t>(e.agoS) : 0;
      e.ms = (it->open ? now : it->endMs) - it->startMs;
      e.live = it->open;
      x.events.push_back(e);
    }
    x.frames = o.frames;
    x.lost = o.lost;
    x.late = o.late;
    x.histories = o.histories;
    out.push_back(std::move(x));
  }
  std::stable_sort(out.begin(), out.end(), [](const OpenMic &a, const OpenMic &b) {
    if (a.group != b.group) return a.group < b.group;
    return a.onS > b.onS;
  });
  return out;
}

void ackLost() { post(Cmd::AckLost); }

bool keepAwake() { return awake.load(); }

void micVanish(bool on) { post(Cmd::MicVanish, 0, on ? 1 : 0); }

void micTone(uint32_t ms, int db) { post(Cmd::MicTone, std::min(0, db), ms); }

void setLongMicMs(uint32_t ms) {
  if (!core) return;
  core->longMicMs = ms ? ms : kLongMicMs;
}

std::vector<LinkStat> linkStats() {
  std::vector<LinkStat> out;
  if (!core) return out;
  const uint32_t now = millis();
  std::lock_guard<std::mutex> l(core->mu);
  for (size_t i = 0; i < core->links.size(); ++i) {
    const Neighbours::Neighbour &n = core->links.at(i);
    LinkStat x;
    x.id = n.id;
    x.got = n.got;
    x.missed = n.missed;
    for (int k = 0; k < 4; ++k) x.runs[k] = n.runs[k];
    x.rssi = n.rssi;
    x.neighbour = n.heardMs && now - n.heardMs < Neighbours::kHeardMs;
    out.push_back(x);
  }
  return out;
}

namespace {
// The air survey's sums, written by the Wi-Fi task's promiscuous callback.
struct AirSums {
  uint32_t frames[AirSurvey::kKinds];
  uint64_t us[AirSurvey::kKinds];
  int loudestData;
};
AirSums *airSums = nullptr;

// How long a frame was on the air, in µs, from its length (FCS included) and how it came:
// 802.11b with its long (192 µs) or short (96 µs) preamble, OFDM (20 µs, 4 µs symbols), HT
// (36 µs of preambles, 4 µs symbols; 40 MHz about twice the rate), long range as 1 Mbps at half
// the rate (its preamble isn't documented). Approximate: enough to tell a busy channel.
uint32_t airUs(const wifi_pkt_rx_ctrl_t &rx) {
  const uint32_t len = rx.sig_len;
  const uint32_t bits = 22 + 8 * len;  // service, the bits, tail (OFDM)
  if (rx.sig_mode == 0) {
    static const float kDsss[8] = {1, 2, 5.5f, 11, 1, 2, 5.5f, 11};
    static const float kOfdm[8] = {48, 24, 12, 6, 54, 36, 18, 9};
    const unsigned r = rx.rate;
    if (r < 8) return (r < 4 ? 192 : 96) + static_cast<uint32_t>(8 * len / kDsss[r]);
    if (r < 16) {
      const float perSymbol = 4 * kOfdm[r - 8];
      return 20 + 4 * static_cast<uint32_t>(ceilf(bits / perSymbol));
    }
    if (r == 26) return 192 + 16 * len;  // long range, 500 kbps
    return 192 + 8 * len;
  }
  static const float kHt[8] = {6.5f, 13, 19.5f, 26, 39, 52, 58.5f, 65};
  const unsigned mcs = rx.mcs;
  float mbps = kHt[mcs % 8] * (1 + mcs / 8);
  if (rx.cwb) mbps *= 2.08f;
  return 36 + 4 * (mcs / 8) + 4 * static_cast<uint32_t>(ceilf(bits / (4 * mbps)));
}

void onAirFrame(void *buf, wifi_promiscuous_pkt_type_t type) {
  AirSums *a = airSums;
  if (!a || !buf) return;
  const auto *p = static_cast<const wifi_promiscuous_pkt_t *>(buf);
  const wifi_pkt_rx_ctrl_t &rx = p->rx_ctrl;
  const uint8_t *f = p->payload;
  int kind = AirSurvey::kOther;
  if (type == WIFI_PKT_DATA) {
    kind = AirSurvey::kData;
    if (rx.rssi > a->loudestData) a->loudestData = rx.rssi;
  } else if (type == WIFI_PKT_CTRL) {
    kind = AirSurvey::kCtrl;
  } else if (type == WIFI_PKT_MGMT) {
    if (f[0] == 0x80) {
      kind = AirSurvey::kBeacon;
    } else if (f[0] == 0xD0 && rx.sig_len >= 28 && f[24] == 0x7F && f[25] == 0x18 &&
               f[26] == 0xFE && f[27] == 0x34) {  // a vendor action frame: Espressif's, ESP-NOW
      kind = AirSurvey::kEspNow;
    } else {
      kind = AirSurvey::kMgmt;
    }
  }
  ++a->frames[kind];
  a->us[kind] += airUs(rx);
}
}  // namespace

AirSurvey airSurvey(uint32_t ms) {
  AirSurvey r;
  ms = std::max<uint32_t>(1000, std::min<uint32_t>(30000, ms));
  wifi_mode_t mode = WIFI_MODE_NULL;
  if (esp_wifi_get_mode(&mode) != ESP_OK || mode == WIFI_MODE_NULL) {
    r.error = "WI-FI IS OFF";
    return r;
  }
  r.channel = radioChannel();
  if (!r.channel) {
    uint8_t primary = 0;
    wifi_second_chan_t second;
    if (esp_wifi_get_channel(&primary, &second) == ESP_OK) r.channel = primary;
  }
  std::unique_ptr<AirSums> sums(new AirSums());
  memset(sums.get(), 0, sizeof(AirSums));
  sums->loudestData = -128;
  wifi_promiscuous_filter_t all = {WIFI_PROMIS_FILTER_MASK_ALL};
  wifi_promiscuous_filter_t ctrl = {WIFI_PROMIS_CTRL_FILTER_MASK_ALL};
  airSums = sums.get();
  esp_err_t e = esp_wifi_set_promiscuous_rx_cb(onAirFrame);
  if (e == ESP_OK) e = esp_wifi_set_promiscuous_filter(&all);
  if (e == ESP_OK) e = esp_wifi_set_promiscuous_ctrl_filter(&ctrl);
  if (e == ESP_OK) e = esp_wifi_set_promiscuous(true);
  if (e != ESP_OK) {
    airSums = nullptr;
    r.error = esp_err_to_name(e);
    return r;
  }
  const uint32_t t0 = millis();
  delay(ms);
  esp_wifi_set_promiscuous(false);
  r.ms = millis() - t0;
  delay(20);  // a callback still running
  airSums = nullptr;
  for (int k = 0; k < AirSurvey::kKinds; ++k) {
    r.frames[k] = sums->frames[k];
    r.share[k] = static_cast<float>(sums->us[k]) / (r.ms * 1000.0f);
    r.total += r.share[k];
  }
  r.loudestData = sums->loudestData == -128 ? 0 : sums->loudestData;
  r.ok = true;
  return r;
}

bool relayForOthers() {
  if (!core) return true;
  std::lock_guard<std::mutex> l(core->mu);
  return core->relayOn;
}

void setRelayForOthers(bool on) {
  if (!core) return;
  {
    std::lock_guard<std::mutex> l(core->mu);
    if (core->relayOn == on) return;
    core->relayOn = on;
  }
  kv::saveU32(kNs, "norelay", on ? 0 : 1);
  bump();
}

bool callChirp() {
  if (!core) return true;
  std::lock_guard<std::mutex> l(core->mu);
  return core->chirpOn;
}

void setCallChirp(bool on) {
  if (!core) return;
  {
    std::lock_guard<std::mutex> l(core->mu);
    if (core->chirpOn == on) return;
    core->chirpOn = on;
  }
  kv::saveU32(kNs, "nochirp", on ? 0 : 1);
  bump();
}

bool netUp() { return false; }

uint32_t boardId() {
  ensureCore();
  return core->id;
}

std::string boardName() {
  ensureCore();
  return core->name;
}

bool listening() { return isListening.load(); }

uint32_t ping(int group) {
  if (!core || !isListening) return 0;
  {
    std::lock_guard<std::mutex> l(core->mu);
    if (group < 0 || group >= static_cast<int>(core->groups.size()) ||
        core->groups[group].via == Via::Net) {
      return 0;
    }
    core->pongs.clear();
  }
  uint32_t id;
  do {
    id = esp_random();
  } while (!id);
  post(Cmd::Ping, group, id);
  return id;
}

std::vector<Pong> pongs() {
  if (!core) return {};
  std::lock_guard<std::mutex> l(core->mu);
  return core->pongs;
}

void setTestGroup(const std::string &secret, const std::string &name) {
  ensureCore();
  Preferences prefs;
  if (prefs.begin(kNs, false)) {
    if (secret.empty()) {
      prefs.remove("devgrp");
    } else {
      prefs.putString("devgrp", (secret + "\t" + upperName(name)).c_str());
    }
    prefs.end();
  }
  if (taskHandle) {
    post(Cmd::Reload);
  } else {
    begin();
  }
}

void resend(bool firstVoice) { post(Cmd::Resend, 0, firstVoice ? 1 : 0); }

void setTestChannel(int channel) {
  ensureCore();
  if (channel < 0 || channel > 13) return;
  kv::saveU32(kNs, "devchan", static_cast<uint32_t>(channel));
  std::lock_guard<std::mutex> l(core->mu);
  core->devChannel = channel;
}


Range range() { return wantLong() ? Range::Long : Range::Normal; }

void setDeaf(uint32_t id) {
  deafTo = id;
  log_i("comms: %s", id ? "deaf to a board (!commsdeaf)" : "deaf to nobody");
}
uint32_t deaf() { return deafTo.load(); }
void setFlood(bool on) { floodAll = on; }
bool flood() { return floodAll.load(); }

void setTestRange(bool set, Range r) {
  ensureCore();
  const int v = !set ? 0 : r == Range::Long ? 2 : 1;
  kv::saveU32(kNs, "devrange", static_cast<uint32_t>(v));
  devRangeMode = v;
  post(Cmd::Range);
}

bool testRange() { return devRange() != 0; }

void longRangeRx(bool on) {
  wlan::setLongRangeProtocol(on);
  post(Cmd::Range, 0, 1);  // even while joined: the connection drops and joins again
}

void forceOff() {
  poweringOff = true;
  if (micWanted.load() && taskHandle) {
    // The mic's off goes first, so the others know it was a power-off rather than a lost link.
    powerOffSent = false;
    post(Cmd::PowerOff, 0, 0, 100);
    const uint32_t t0 = millis();
    while (!powerOffSent.load() && millis() - t0 < 500) delay(10);
    delay(30);  // out of the radio
  }
  std::lock_guard<std::mutex> g(radioMu);
  if (!espnowUp) return;
  esp_now_unregister_recv_cb();
  esp_now_unregister_send_cb();
  esp_now_deinit();
  espnowUp = false;
  isListening = false;
}

// --- Reachable on battery: the sleeping board ---------------------------------------------------

namespace {

constexpr const char *kNsReach = "reach";  // NVS: chan (the channel boards were last heard on),
                                           // usb (1: USB power seen since the log began), log,
                                           // dev (!commsreach: seconds, 0xFFFF off; none: the card)
constexpr uint32_t kReachMagic = 0x52434831;   // "RCH1"
// After a poll's send, for an answer: one comes 2.5 to 7 ms after the send is done (!reachbench,
// 2026-11-23); an echo poll, which decides the channel, waits longer.
constexpr uint32_t kReachListenMs = 12;
constexpr uint32_t kReachEchoListenMs = 20;
constexpr uint32_t kReachSendMs = 30;          // a send that hasn't finished by then didn't go
constexpr uint32_t kReachStatusMs = 60000;     // the battery looked at this often (USB: each wake)
constexpr uint32_t kReachFlushMs = 10 * 60000; // the log to NVS this often (and at each restart)
constexpr uint32_t kReachWokeFreshS = 120;     // the answer that woke the board, for begin()

// What a sleeping board keeps across the restarts it makes for its off face's wakes, in RTC
// memory (a software restart keeps it; a power loss doesn't, and NVS has the log then).
struct ReachRtc {
  uint32_t magic;
  uint32_t board;
  uint16_t intervalS;
  SleepPlan plan;
  ReachLog log;
  Answer woke;       // the answer that woke it
  uint32_t wokeAt;   // UTC; 0 none
  int32_t wokeGroup;
  uint32_t sum;      // FNV-1a of the bytes before it
};
RTC_NOINIT_ATTR uint8_t reachRaw[sizeof(ReachRtc)] __attribute__((aligned(4)));
ReachRtc reachRtc;         // the working copy
bool reachLoaded = false;
std::mutex reachMu;        // reachRtc.log, between the task, the loop and the dev tools
std::atomic<uint8_t> wokenVerdict{0};  // Woken, as the task last stepped it
std::atomic<bool> wokenKeyed{false};
std::atomic<int> devReach{-2};  // !commsreach as read from NVS (-2: not read yet; -1: none)

uint32_t reachSum(const ReachRtc &r) {
  const uint8_t *p = reinterpret_cast<const uint8_t *>(&r);
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < offsetof(ReachRtc, sum); ++i) h = (h ^ p[i]) * 16777619u;
  return h;
}

void reachKeep() {  // under reachMu
  reachRtc.magic = kReachMagic;
  reachRtc.sum = reachSum(reachRtc);
  memcpy(reachRaw, &reachRtc, sizeof reachRtc);
}

void reachLoad() {  // under reachMu
  if (reachLoaded) return;
  reachLoaded = true;
  ReachRtc r;
  memcpy(&r, reachRaw, sizeof r);
  if (r.magic == kReachMagic && r.sum == reachSum(r)) {
    reachRtc = r;
    return;
  }
  reachRtc = ReachRtc();
  reachRtc.magic = 0;
  reachRtc.board = 0;
  reachRtc.intervalS = 0;
  reachRtc.wokeAt = 0;
  reachRtc.wokeGroup = -1;
  kv::load(kNsReach, "log", reachRtc.log);  // as last flushed
}

void reachFlush() {  // under reachMu
  kv::save(kNsReach, "log", reachRtc.log);
}

int8_t batteryNow(bool &usb, bool &low) {
  const pmic::Status p = pmic::status();
  usb = p.ok && p.vbus;
  low = p.ok && !p.vbus && p.percent >= 0 && p.percent < 15;  // power::kLowBatteryPercent
  return p.ok && p.battery && p.percent >= 0 ? static_cast<int8_t>(p.percent) : -1;
}

// The channel the boards were last heard on directly: kept in NVS as the task hears them.
uint8_t homeChannel() {
  const uint32_t c = kv::loadU32(kNsReach, "chan");
  return c >= 1 && c <= 13 ? static_cast<uint8_t>(c) : static_cast<uint8_t>(meshChannel());
}

// Each second, listening: the channel the boards are on (any heard straight from them lately).
void noteHomeChannel(uint32_t now) {
  const int ch = radioChannel();
  if (ch < 1 || ch > 13 || core->homeChannel == ch) return;
  bool heard = false;
  {
    std::lock_guard<std::mutex> l(core->mu);
    for (const GroupRec &g : core->groups) {
      for (const BoardRec &b : g.boards) heard = heard || within(b.directMs, now, 10000);
    }
  }
  if (!heard) return;
  core->homeChannel = static_cast<uint8_t>(ch);
  kv::saveU32(kNsReach, "chan", static_cast<uint32_t>(ch));
}

// --- The sleeper's radio: Wi-Fi stopped while it sleeps, started for each poll (step 1 of the
// issue's phase 7 measured esp_wifi_start() plus the channel at about 1.8 ms) ---------------------

esp_now_peer_info_t reachPeer() {
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, kBroadcast, 6);
  peer.channel = 0;
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;
  return peer;
}

bool reachRadioOpen(uint8_t channel) {
  wlan::forceOff();  // a sleeping board isn't on a network
  wlan::holdRadio();
  WiFi.setSleep(false);
  std::lock_guard<std::mutex> g(radioMu);
  esp_err_t e = esp_now_init();
  if (e == ESP_OK) e = esp_now_register_recv_cb(onRecv);
  if (e == ESP_OK) e = esp_now_register_send_cb(onSent);
  const esp_now_peer_info_t peer = reachPeer();
  if (e == ESP_OK) e = esp_now_add_peer(&peer);
  // The protocols (long range heard too) and the rate (mesh_range), as radioStart() sets them;
  // both stay across the esp_wifi_stop() and esp_wifi_start() of each poll.
  if (e == ESP_OK) applyRadio(true);
  if (e == ESP_OK) e = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
  if (e != ESP_OK) {
    log_e("comms: the sleeping board's radio won't start (%s)", esp_err_to_name(e));
    esp_now_deinit();
    wlan::releaseRadio();
    return false;
  }
  espnowUp = true;
  return true;
}

void reachRadioUp(uint8_t channel) {
  esp_wifi_start();
  esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
}

void reachRadioDown() { esp_wifi_stop(); }

void reachRadioClose() {
  {
    std::lock_guard<std::mutex> g(radioMu);
    if (espnowUp) {
      esp_now_unregister_recv_cb();
      esp_now_unregister_send_cb();
      esp_now_deinit();
      espnowUp = false;
    }
  }
  esp_wifi_start();  // as wlan left it; releaseRadio() switches it off
  wlan::releaseRadio();
  if (rxQueue) xQueueReset(rxQueue);
}

// A frame from the radio: an answer to this board? Its group too.
bool openAnswer(const RxFrame &f, int &gi, Answer &a) {
  Header hd;
  if (!parseHeader(f.data, f.len, hd) || hd.type != kExt || hd.sender == core->id) return false;
  gi = groupIndex(hd.group);
  if (gi < 0) return false;
  uint8_t buf[kMaxFrame];
  memcpy(buf, f.data, f.len);
  size_t bodyLen = 0;
  if (!core->groups[gi].sealer->open(buf, f.len, bodyLen)) return false;
  Reader r(buf + kHeader, bodyLen);
  const uint32_t at = r.u32();
  if (r.u8() != kExtAnswer || !readAnswer(r, a) || a.to != core->id) return false;
  return core->guard.accept(hd.sender, hd.boot, hd.seq, fresh(at)) == ReplayGuard::Verdict::Ok;
}

struct PollOutcome {
  bool sent = false;
  bool answered = false;  // an answer to this board came (an echo's included)
  int answers = 0;
  Answer best;            // what's waiting, from all the answers together
  int group = -1;
};

// One poll in each group that goes over the air, on the radio's channel now, then up to
// kReachListenMs for answers: to the first with something waiting, the whole window for an echo.
PollOutcome reachPollNow(uint8_t flags, uint16_t intervalS, int8_t battery) {
  PollOutcome out;
  Poll p;
  p.flags = flags;
  p.battery = battery >= 0 ? static_cast<uint8_t>(battery) : 0xFF;
  p.intervalS = intervalS;
  p.nameLen = static_cast<uint8_t>(std::min<size_t>(core->name.size(), sizeof p.name));
  memcpy(p.name, core->name.data(), p.nameLen);
  uint8_t body[48];
  const size_t n = putPoll(body, sizeof body, sentAt(), p);
  if (rxQueue) xQueueReset(rxQueue);
  sentAtUs = 0;
  for (GroupRec &g : core->groups) {
    if (g.via == Via::Net || !n) continue;
    uint8_t f[kMaxFrame];
    Header h;
    h.type = kExt;
    h.ttl = 0;
    h.group = g.id;
    h.sender = core->id;
    h.boot = core->boot;
    h.seq = ++core->seq;
    putHeader(h, f);
    memcpy(f + kHeader, body, n);
    const size_t len = g.sealer->seal(f, n);
    if (len && esp_now_send(kBroadcast, f, len) == ESP_OK) out.sent = true;
  }
  if (!out.sent) return out;
  const uint32_t t0 = millis();
  while (!sentAtUs.load() && millis() - t0 < kReachSendMs) vTaskDelay(1);
  const uint32_t from = millis();
  for (;;) {
    RxFrame &f = core->rx;
    while (rxQueue && xQueueReceive(rxQueue, &f, 0) == pdTRUE) {
      int gi = -1;
      Answer a;
      if (!openAnswer(f, gi, a)) continue;
      out.answered = true;
      ++out.answers;
      if (a.what & kWaitRing && !(out.best.what & kWaitRing)) {
        out.best.dialer = a.dialer;
        out.best.ring = a.ring;
      }
      if (a.what & kWaitMessage && !(out.best.what & kWaitMessage)) {
        out.best.msgSender = a.msgSender;
        out.best.msg = a.msg;
        out.best.msgTo = a.msgTo;
        out.best.frames = a.frames;
      }
      if (a.what && out.group < 0) out.group = gi;
      out.best.what |= a.what;
      out.best.to = a.to;
    }
    if (out.best.what && !(flags & kPollEcho)) break;
    if (millis() - from >= ((flags & kPollEcho) ? kReachEchoListenMs : kReachListenMs)) break;
    vTaskDelay(1);
  }
  return out;
}

// Asleep for ms: light sleep (timer, and PWR: BOOT and the dial don't wake it), or pretending
// (the console alive: 'o' on it counts as PWR). True if PWR ended it.
bool reachNap(uint32_t ms, bool pretend) {
  const gpio_num_t pwr = static_cast<gpio_num_t>(board::PWR_KEY_SENSE);
  if (pretend) {
    const uint32_t t0 = millis();
    while (millis() - t0 < ms) {
      if (gpio_get_level(pwr)) return true;
      while (Serial.available() > 0) {
        const int k = Serial.read();
        if (k == 'o') return true;
        // The dev tools' screenshot, while pretending (no dev tools task runs here): the face
        // as the boot drew it (tools/screenshot.py's format; nothing before screen::begin()).
        if (k == 's' && screen::ready()) screen::dump(Serial);
      }
      delay(std::min<uint32_t>(20, ms - (millis() - t0)));
    }
    return false;
  }
  gpio_sleep_sel_dis(pwr);
  gpio_wakeup_enable(pwr, GPIO_INTR_HIGH_LEVEL);
  esp_sleep_enable_gpio_wakeup();
  esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(ms) * 1000);
  esp_light_sleep_start();
  const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  gpio_wakeup_disable(pwr);
  return cause == ESP_SLEEP_WAKEUP_GPIO && gpio_get_level(pwr);
}

// Milliseconds until UTC time t (0 if it's now or past).
uint32_t msUntil(time_t t) {
  timeval tv;
  gettimeofday(&tv, nullptr);
  const int64_t ms = (static_cast<int64_t>(t) - tv.tv_sec) * 1000 - tv.tv_usec / 1000;
  return ms > 0 ? static_cast<uint32_t>(std::min<int64_t>(ms, 0x7FFFFFFF)) : 0;
}

// Each turn of the task while the board is up after a wake: steps the hold (WokenHold).
void stepWoken(uint32_t now) {
  if (!core->woken.active()) return;
  WokenHold::Seen s;
  s.ringing = core->ringing.on;
  // Voice: PLAY LAST, a message, or a held mic heard here (its last frame within a second); a
  // latched one (a room) is a mic still on, not a sound, so it can't keep the board up all night.
  s.sound = core->replay.on;
  {
    std::lock_guard<std::mutex> l(core->mu);
    for (const OpenMicRec &o : core->mics) {
      if (!o.st.on() || !heardHere(o)) continue;
      s.micOn = true;
      if (!o.st.latched() && o.st.voiceMs() && now - o.st.voiceMs() < 1000) s.sound = true;
    }
  }
  for (const Inbound &in : core->inbound) s.transfer = s.transfer || in.used;
  s.keyed = wokenKeyed.load();
  const uint32_t since = core->woken.since();
  switch (core->woken.step(now, s)) {
    case WokenHold::Verdict::Stay: wokenVerdict = static_cast<uint8_t>(Woken::Stay); return;
    case WokenHold::Verdict::Sleep: wokenVerdict = static_cast<uint8_t>(Woken::Sleep); break;
    case WokenHold::Verdict::On: wokenVerdict = static_cast<uint8_t>(Woken::On); break;
  }
  log_i("comms: up %lu s after the wake: %s", static_cast<unsigned long>((now - since) / 1000),
        wokenVerdict == static_cast<uint8_t>(Woken::On) ? "a key, the board stays on"
                                                        : "back to sleep");
  std::lock_guard<std::mutex> l(reachMu);
  reachLoad();
  reachRtc.log.awakeMs += now - since;
  reachKeep();
  reachFlush();
}

// begin(): the board woke for comms (reachSleep(), this boot): the hold, and a message to ask for.
void takeWoke() {
  std::lock_guard<std::mutex> l(reachMu);
  reachLoad();
  const uint32_t at = reachRtc.wokeAt;
  const time_t now = utcNow();
  if (!at || (now && (now < at || now - at > kReachWokeFreshS))) return;
  const Answer a = reachRtc.woke;
  const int gi = reachRtc.wokeGroup;
  reachRtc.wokeAt = 0;
  reachKeep();
  core->woken.begin(millis(), a.what);
  wokenVerdict = static_cast<uint8_t>(Woken::Stay);
  wokenKeyed = false;
  log_i("comms: woke for %s%s%s", a.what & kWaitRing ? "a ring " : "",
        a.what & kWaitVoice ? "voice " : "", a.what & kWaitMessage ? "a message" : "");
  if ((a.what & kWaitMessage) && gi >= 0 && gi < static_cast<int>(core->groups.size())) {
    core->fetchSender = a.msgSender;
    core->fetchMsg = a.msg;
    core->fetchTo = a.msgTo;
    post(Cmd::Fetch, gi, 0, 0);
  }
}

}  // namespace

int reachInterval() {
  int dev = devReach.load();
  if (dev == -2) {
    Preferences prefs;
    dev = -1;
    if (prefs.begin(kNsReach, true)) {
      if (prefs.isKey("dev")) dev = static_cast<int>(prefs.getUInt("dev", 0));
      prefs.end();
    }
    devReach = dev;
  }
  int s = dev >= 0 ? (dev == 0xFFFF ? 0 : dev) : settings::meshReachable();
  if (s <= 0) return 0;
  s = std::max(2, std::min(s, 30));
  return reachPossible() ? s : 0;
}

bool reachPossible() {
  // A group: a mesh_group line, the agent token's, or the dev one.
  return !settings::meshGroups().empty() || !settings::agentToken().empty() ||
         !devGroupRecord().empty();
}

void setReachOverride(int seconds) {
  Preferences prefs;
  if (!prefs.begin(kNsReach, false)) return;
  if (seconds < 0) {
    prefs.remove("dev");
    devReach = -1;
  } else {
    const uint32_t v = seconds == 0 ? 0xFFFF : static_cast<uint32_t>(std::max(2, std::min(seconds, 30)));
    prefs.putUInt("dev", v);
    devReach = static_cast<int>(v);
  }
  prefs.end();
}

bool reachOverridden() {
  reachInterval();
  return devReach.load() >= 0;
}

MissedRing missedRing() {
  MissedRec r;
  MissedRing m;
  if (missedCount.load() == 0) return m;
  const bool have = kv::load(kNsReach, "missed", r) && r.count > 0;
  missedCount = have ? r.count : 0;
  if (!have) return m;
  m.from = r.from;
  m.name = r.name;
  m.at = static_cast<time_t>(r.at);
  m.count = r.count;
  return m;
}

void clearMissedRings() {
  if (missedCount.load() == 0) return;
  MissedRec r;
  missedCount = 0;
  if (!kv::load(kNsReach, "missed", r) || r.count == 0) return;
  kv::save(kNsReach, "missed", MissedRec());
  log_i("comms: the missed rings are cleared");
}

ReachEnd reachSleep(const ReachArgs &a) {
  ensureCore();
  if (core->groups.empty()) {
    std::vector<GroupRec> g = loadGroups();
    std::lock_guard<std::mutex> l(core->mu);
    core->groups = std::move(g);
  }
  if (core->groups.empty()) return ReachEnd::Failed;
  if (!core->boot) core->boot = nextBoot();
  if (!rxQueue) {
    rxQueue = xQueueCreateWithCaps(kRxDepth, sizeof(RxFrame), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!rxQueue) return ReachEnd::Failed;
  }
  const uint16_t interval = static_cast<uint16_t>(std::max(2, std::min(a.intervalS, 30)));
  bool usb = false, low = false;
  int8_t battery = batteryNow(usb, low);
  {
    std::lock_guard<std::mutex> l(reachMu);
    reachLoad();
    if (reachRtc.board != core->id || reachRtc.intervalS != interval) {
      const uint8_t misses = reachRtc.board == 0 ? reachRtc.plan.misses : 0;  // !reachchan's
      Sleeper::begin(reachRtc.plan, homeChannel(), interval);
      reachRtc.plan.misses = misses;
      reachRtc.board = core->id;
      reachRtc.intervalS = interval;
    }
    ReachLog &lg = reachRtc.log;
    if (!lg.since || kv::loadU32(kNsReach, "usb")) {  // the first sleep since USB: a new log
      lg = ReachLog();
      lg.since = static_cast<uint32_t>(utcNow());
      lg.startPct = battery;
      kv::saveU32(kNsReach, "usb", 0);
    }
    lg.lastPct = battery;
    lg.lastAt = static_cast<uint32_t>(utcNow());
    lg.channel = reachRtc.plan.channel;
    reachKeep();
  }
  // PWR still down from the press that sent the board off: wait for it to go up.
  const gpio_num_t pwr = static_cast<gpio_num_t>(board::PWR_KEY_SENSE);
  pinMode(board::PWR_KEY_SENSE, INPUT);
  for (uint32_t t0 = millis(); gpio_get_level(pwr) && millis() - t0 < 5000;) delay(20);
  if (!reachRadioOpen(reachRtc.plan.channel)) return ReachEnd::Failed;
  reachRadioDown();
  log_i("comms: asleep, reachable: a poll every %u s on channel %u", interval,
        reachRtc.plan.channel);

  uint32_t nextPoll = millis();  // the first at once: the boards learn it's asleep
  uint32_t statusAt = millis();
  uint32_t flushAt = millis();
  ReachEnd end = ReachEnd::Failed;
  for (;;) {
    const time_t utc = utcNow();
    if (a.wakeAt && utc && utc >= a.wakeAt) {
      end = ReachEnd::Due;
      break;
    }
    if (a.testEnd && utc && utc >= a.testEnd) {
      end = ReachEnd::TestOver;
      break;
    }
    uint32_t now = millis();
    // USB plugged in: looked at every wake (one register), so the port is back within a poll's
    // interval. Once a minute only, it wasn't: a board plugged in stayed off the bus for up to a
    // minute, the USB pad off through each light sleep (found 2026-11-24: no port after 45 s).
    if (!a.ignoreUsb && pmic::usbPower()) {
      end = ReachEnd::Usb;
      break;
    }
    if (now - statusAt >= kReachStatusMs) {
      statusAt = now;
      battery = batteryNow(usb, low);
      if (usb && !a.ignoreUsb) {
        end = ReachEnd::Usb;
        break;
      }
      if (low) {
        end = ReachEnd::LowBattery;
        break;
      }
      std::lock_guard<std::mutex> l(reachMu);
      reachRtc.log.lastPct = battery;
      reachRtc.log.lastAt = static_cast<uint32_t>(utc);
    }
    if (now - flushAt >= kReachFlushMs) {
      flushAt = now;
      std::lock_guard<std::mutex> l(reachMu);
      reachFlush();
    }
    if (static_cast<int32_t>(now - nextPoll) >= 0) {
      nextPoll += interval * 1000u;
      if (static_cast<int32_t>(nextPoll - now) <= 0) nextPoll = now + interval * 1000u;
      SleepPlan &plan = reachRtc.plan;
      const Sleeper::Next n = Sleeper::next(plan, interval);
      const uint32_t up = millis();
      PollOutcome o;
      if (n.sweep) {
        uint8_t found = 0;
        esp_wifi_start();
        for (uint8_t ch = 1; ch <= 13 && !found; ++ch) {
          esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
          o = reachPollNow(n.flags, interval, battery);
          if (o.answered) found = ch;
        }
        reachRadioDown();
        Sleeper::sweepResult(plan, interval, found);
        if (found) kv::saveU32(kNsReach, "chan", found);
        log_i("comms: swept the channels: %s %u", found ? "boards on" : "nobody; staying on",
              found ? found : plan.channel);
      } else {
        reachRadioUp(plan.channel);
        o = reachPollNow(n.flags, interval, battery);
        reachRadioDown();
        Sleeper::result(plan, interval, n, o.answered);
      }
      const uint32_t radioMs = millis() - up;
      if (a.pretend) {  // the console is alive: each poll, for the tests
        log_i("comms: poll %lu on channel %u%s%s: %d answers, waiting %02X, radio %lu ms",
              static_cast<unsigned long>(plan.polls), plan.channel,
              n.flags & kPollEcho ? " (echo)" : "", n.sweep ? " (sweep)" : "", o.answers,
              o.best.what, static_cast<unsigned long>(radioMs));
      }
      {
        std::lock_guard<std::mutex> l(reachMu);
        ReachLog &lg = reachRtc.log;
        lg.radioMs += radioMs;
        lg.channel = plan.channel;
        if (n.sweep) {
          ++lg.sweeps;
          if (o.answered) ++lg.sweepFound;
        } else {
          ++lg.polls;
          if (o.answered) ++lg.answered;
          if (n.flags & kPollEcho) {
            ++lg.echoes;
            if (o.answered) ++lg.echoAnswered;
          }
        }
        if (o.best.what) {
          ++lg.woken;
          reachRtc.woke = o.best;
          reachRtc.wokeAt = static_cast<uint32_t>(utcNow());
          reachRtc.wokeGroup = o.group;
        }
        reachKeep();
      }
      if (o.best.what) {
        log_i("comms: a poll's answer woke the board (%s%s%s)",
              o.best.what & kWaitRing ? "ring " : "", o.best.what & kWaitVoice ? "voice " : "",
              o.best.what & kWaitMessage ? "message" : "");
        end = ReachEnd::Woken;
        break;
      }
    }
    // Asleep until the next poll or the wake, whichever is first.
    now = millis();
    uint32_t ms = static_cast<int32_t>(nextPoll - now) > 0 ? nextPoll - now : 0;
    if (a.wakeAt) ms = std::min(ms, msUntil(a.wakeAt));
    if (a.testEnd) ms = std::min(ms, msUntil(a.testEnd));
    if (ms == 0) continue;
    const bool pressed = reachNap(ms, a.pretend);
    {
      std::lock_guard<std::mutex> l(reachMu);
      reachRtc.log.asleepMs += millis() - now;
    }
    if (pressed) {
      end = ReachEnd::PowerKey;
      break;
    }
  }
  reachRadioClose();
  std::lock_guard<std::mutex> l(reachMu);
  if (end == ReachEnd::Due) ++reachRtc.log.restarts;
  if (end == ReachEnd::PowerKey) ++reachRtc.log.pwr;
  reachKeep();
  reachFlush();
  return end;
}

Woken reachWokenStep(bool keyed) {
  if (keyed) wokenKeyed = true;
  return static_cast<Woken>(wokenVerdict.load());
}

ReachLog reachLog() {
  std::lock_guard<std::mutex> l(reachMu);
  reachLoad();
  return reachRtc.log;
}

void reachLogReset() {
  std::lock_guard<std::mutex> l(reachMu);
  reachLoad();
  reachRtc.log = ReachLog();
  reachKeep();
  reachFlush();
}

void setReachChannel(int channel, int misses) {
  if (channel >= 1 && channel <= 13) kv::saveU32(kNsReach, "chan", static_cast<uint32_t>(channel));
  std::lock_guard<std::mutex> l(reachMu);
  reachLoad();
  reachRtc.board = 0;  // the next sleep plans afresh, on that channel
  reachRtc.plan.misses = static_cast<uint8_t>(std::max(0, std::min(misses, 255)));
  reachKeep();
}

void reachLogFlush() {
  std::lock_guard<std::mutex> l(reachMu);
  reachLoad();
  reachKeep();
  reachFlush();
}

void dumpReach(Print &out) {
  const ReachLog lg = reachLog();
  const int interval = reachInterval();
  out.printf("reach: %s%s; polls on channel %u (boards last heard on %u)\n",
             interval ? ("every " + std::to_string(interval) + " s").c_str() : "off",
             reachOverridden() ? " (System's REACHABLE or !commsreach; !commsreach - removes it)"
                               : " (mesh_reachable)",
             lg.channel, homeChannel());
  const MissedRing mr = missedRing();
  out.printf("  silent %s; missed rings shown on the off faces: %d", audio::silent() ? "on" : "off",
             mr.count);
  if (mr.count) {
    out.printf(" (the last from %s %08lX at %ld)", mr.name.c_str(),
               static_cast<unsigned long>(mr.from), static_cast<long>(mr.at));
  }
  out.println();
  if (!lg.since) {
    out.println("  log: empty (no sleep since it began)");
    return;
  }
  const time_t since = lg.since;
  tm t;
  localtime_r(&since, &t);
  const uint32_t hours10 = lg.lastAt > lg.since ? (lg.lastAt - lg.since) / 360 : 0;
  out.printf("  log since %04d-%02d-%02d %02d:%02d (%lu.%lu h): battery %d %% then, %d %% at the "
             "last look\n",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min,
             static_cast<unsigned long>(hours10 / 10), static_cast<unsigned long>(hours10 % 10),
             lg.startPct, lg.lastPct);
  out.printf("  polls %lu (answered %lu), echo polls %lu (answered %lu), sweeps %lu (found %lu)\n",
             static_cast<unsigned long>(lg.polls), static_cast<unsigned long>(lg.answered),
             static_cast<unsigned long>(lg.echoes), static_cast<unsigned long>(lg.echoAnswered),
             static_cast<unsigned long>(lg.sweeps), static_cast<unsigned long>(lg.sweepFound));
  out.printf("  woken %lu, restarts for the off face %lu, PWR %lu\n",
             static_cast<unsigned long>(lg.woken), static_cast<unsigned long>(lg.restarts),
             static_cast<unsigned long>(lg.pwr));
  const uint32_t polls = lg.polls + lg.sweeps * 13;
  out.printf("  radio up %lu ms (%lu ms a poll), asleep %lu s, up after a wake %lu s\n",
             static_cast<unsigned long>(lg.radioMs),
             static_cast<unsigned long>(polls ? lg.radioMs / polls : 0),
             static_cast<unsigned long>(lg.asleepMs / 1000),
             static_cast<unsigned long>(lg.awakeMs / 1000));
}

// --- !reachbench: the radio around light sleep, measured ---------------------------------------

namespace {

struct BenchCycle {
  int16_t sleepErr = 0;
  uint32_t sleptUs = 0;  // in esp_light_sleep_start() (or the delay)
  uint32_t upUs = 0;     // wake to the radio up
  uint32_t sentUs = 0;   // wake to the poll's send done
  uint32_t answerUs = 0; // wake to the first answer (0: none)
  uint32_t awakeUs = 0;  // wake to the radio down again
  int answers = 0;
  bool sendOk = false;
};

// One sealed poll with the echo flag (boards on USB answer at once) into f; its length.
size_t benchPoll(GroupRec &g, uint8_t *f) {
  Poll p;
  p.flags = kPollEcho;
  p.battery = 0xFF;
  p.intervalS = 0;
  uint8_t body[48];
  const size_t n = putPoll(body, sizeof body, sentAt(), p);
  Header h;
  h.type = kExt;
  h.ttl = 0;
  h.group = g.id;
  h.sender = core->id;
  h.boot = core->boot;
  h.seq = ++core->seq;
  putHeader(h, f);
  memcpy(f + kHeader, body, n);
  return g.sealer->seal(f, n);
}

// Frames waiting in rxQueue: answers to this board.
int benchAnswers(GroupRec &g) {
  int n = 0;
  RxFrame &f = core->rx;
  while (xQueueReceive(rxQueue, &f, 0) == pdTRUE) {
    Header hd;
    if (!parseHeader(f.data, f.len, hd) || hd.group != g.id || hd.type != kExt) continue;
    uint8_t buf[kMaxFrame];
    memcpy(buf, f.data, f.len);
    size_t bodyLen = 0;
    if (!g.sealer->open(buf, f.len, bodyLen)) continue;
    Reader r(buf + kHeader, bodyLen);
    r.u32();
    if (r.u8() != kExtAnswer) continue;
    Answer a;
    if (readAnswer(r, a) && a.to == core->id) ++n;
  }
  return n;
}

}  // namespace

namespace {

// One mode's cycles; its report appended to rep.
void benchMode(char mode, int cycles, uint32_t sleepMs, uint32_t listenMs, int ch, GroupRec &g,
               std::string &rep) {
  const esp_now_peer_info_t peer = reachPeer();
  if (mode == 'c') {
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    esp_now_set_wake_window(0);
  }
  std::vector<BenchCycle> res(cycles);
  uint8_t f[kMaxFrame];
  for (int i = 0; i < cycles; ++i) {
    BenchCycle &c = res[i];
    // Down.
    if (mode == 'a' || mode == 'p') esp_wifi_stop();
    if (mode == 'A') {
      esp_now_deinit();
      esp_wifi_stop();
    }
    if (mode == 'c') esp_wifi_force_wakeup_release();
    // Asleep.
    const int64_t s0 = esp_timer_get_time();
    if (mode == 'p') {
      delay(sleepMs);
    } else {
      esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(sleepMs) * 1000);
      c.sleepErr = static_cast<int16_t>(esp_light_sleep_start());
      esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    }
    const int64_t w = esp_timer_get_time();
    c.sleptUs = static_cast<uint32_t>(w - s0);
    // Up.
    if (mode == 'a' || mode == 'p' || mode == 'A') {
      esp_wifi_start();
      if (mode == 'A') {
        esp_now_init();
        esp_now_register_recv_cb(onRecv);
        esp_now_register_send_cb(onSent);
        esp_now_add_peer(&peer);
      }
      esp_wifi_set_channel(static_cast<uint8_t>(ch), WIFI_SECOND_CHAN_NONE);
    }
    if (mode == 'c') esp_wifi_force_wakeup_acquire();
    c.upUs = static_cast<uint32_t>(esp_timer_get_time() - w);
    xQueueReset(rxQueue);
    sentAtUs = 0;
    const size_t n = benchPoll(g, f);
    c.sendOk = n && esp_now_send(kBroadcast, f, n) == ESP_OK;
    while (c.sendOk && !sentAtUs.load() && esp_timer_get_time() - w < 50000) vTaskDelay(1);
    if (sentAtUs.load()) c.sentUs = sentAtUs.load() - static_cast<uint32_t>(w);
    // Listen: until the first answer, or listenMs after the send.
    const int64_t until = esp_timer_get_time() + static_cast<int64_t>(listenMs) * 1000;
    while (esp_timer_get_time() < until) {
      const int a = benchAnswers(g);
      if (a && !c.answers) c.answerUs = rxAtUs.load() - static_cast<uint32_t>(w);
      c.answers += a;
      if (c.answers) break;
      vTaskDelay(1);
    }
    c.awakeUs = static_cast<uint32_t>(esp_timer_get_time() - w);
  }
  if (mode == 'c') {
    esp_wifi_force_wakeup_release();
    esp_wifi_set_ps(WIFI_PS_NONE);
  }
  esp_wifi_start();
  if (mode == 'A') {  // ESP-NOW as the next mode expects it
    esp_now_init();
    esp_now_register_recv_cb(onRecv);
    esp_now_register_send_cb(onSent);
    esp_now_add_peer(&peer);
  }
  char line[200];
  uint64_t sumUp = 0, sumSent = 0, sumAns = 0, sumAwake = 0, sumOver = 0;
  int answered = 0, sent = 0, errs = 0;
  for (int i = 0; i < cycles; ++i) {
    const BenchCycle &c = res[i];
    if (i < 4) {
      snprintf(line, sizeof line, "%c%d err %d slept %lu up %lu sent %lu answer %lu awake %lu (us)\n",
               mode, i, c.sleepErr, static_cast<unsigned long>(c.sleptUs),
               static_cast<unsigned long>(c.upUs), static_cast<unsigned long>(c.sentUs),
               static_cast<unsigned long>(c.answerUs), static_cast<unsigned long>(c.awakeUs));
      rep += line;
    }
    if (c.sleepErr) ++errs;
    sumUp += c.upUs;
    sumAwake += c.awakeUs;
    sumOver += c.sleptUs > sleepMs * 1000 ? c.sleptUs - sleepMs * 1000 : 0;
    if (c.sentUs) {
      ++sent;
      sumSent += c.sentUs;
    }
    if (c.answers) {
      ++answered;
      sumAns += c.answerUs;
    }
  }
  snprintf(line, sizeof line,
           "mode %c: %d cycles, %d sleep errors; avg up %lu us, send done %lu us (%d), answer %lu "
           "us (%d answered), awake %lu us, sleep overrun %lu us\n",
           mode, cycles, errs, static_cast<unsigned long>(sumUp / cycles),
           static_cast<unsigned long>(sent ? sumSent / sent : 0), sent,
           static_cast<unsigned long>(answered ? sumAns / answered : 0), answered,
           static_cast<unsigned long>(sumAwake / cycles),
           static_cast<unsigned long>(sumOver / cycles));
  rep += line;
}

}  // namespace

void reachBench(char mode, int cycles, uint32_t sleepMs, uint32_t listenMs, Print &out) {
  if (!core || core->groups.empty() || !taskHandle) {
    out.println("reachbench: no group");
    return;
  }
  cycles = std::max(1, std::min(cycles, 40));
  const int chNow = radioChannel();
  radioLent = true;
  for (int i = 0; i < 300 && isListening; ++i) delay(10);
  if (isListening) {
    out.println("reachbench: comms won't let go of the radio");
    radioLent = false;
    return;
  }
  GroupRec &g = core->groups[core->active];
  const int ch = chNow ? chNow : meshChannel();
  if (!reachRadioOpen(static_cast<uint8_t>(ch))) {
    out.println("reachbench: ESP-NOW won't start");
    radioLent = false;
    return;
  }
  const char *modes = mode == 'x' ? "aAbc" : nullptr;
  out.printf("reachbench: mode %c, %d cycles a mode, sleep %lu ms, listen %lu ms, channel %d%s\n",
             mode, cycles, static_cast<unsigned long>(sleepMs),
             static_cast<unsigned long>(listenMs), ch,
             mode == 'p' ? "" : "; the console goes for the light sleeps");
  out.flush();
  delay(200);
  std::string rep;
  auto keep = [&rep](const char *state) {  // after each mode: a reset partway still tells
    Preferences p;
    if (p.begin(kNsReach, false)) {
      p.putString("bench", (rep + state).c_str());
      p.end();
    }
  };
  keep("(started)\n");
  if (modes) {
    for (const char *m = modes; *m; ++m) {
      benchMode(*m, cycles, sleepMs, listenMs, ch, g, rep);
      keep("(going on)\n");
    }
  } else {
    benchMode(mode, cycles, sleepMs, listenMs, ch, g, rep);
  }
  reachRadioClose();
  radioLent = false;
  // The report, kept in NVS too (the console doesn't come back by itself after light sleep).
  Preferences prefs;
  if (prefs.begin(kNsReach, false)) {
    prefs.putString("bench", rep.c_str());
    prefs.end();
  }
  out.print(rep.c_str());
  if (mode != 'p') {
    out.println("reachbench: a power cycle to bring the USB console back; !reachbench shows the "
                "report");
    delay(200);
    power::consoleRecover();
  }
}

void reachBenchShow(Print &out) {
  out.printf("reachbench: this boot's reset reason %d\n", static_cast<int>(esp_reset_reason()));
  Preferences prefs;
  if (prefs.begin(kNsReach, true)) {
    if (prefs.isKey("bench")) out.print(prefs.getString("bench", "").c_str());
    prefs.end();
  }
}

namespace {

const char *routeName(Route r) {
  static const char *const kRoutes[] = {"AWAY", "AIR", "RELAYED", "NET"};
  return kRoutes[static_cast<int>(r)];
}

// The last bars, " -54 - -20": into buf.
void barsText(const std::vector<int8_t> &bars, size_t last, char *buf, size_t cap) {
  size_t n = 0;
  buf[0] = 0;
  for (size_t i = bars.size() > last ? bars.size() - last : 0; i < bars.size() && n < cap; ++i) {
    const int w = bars[i] == kNoBar ? snprintf(buf + n, cap - n, " -")
                                    : snprintf(buf + n, cap - n, " %d", bars[i]);
    if (w < 0) break;
    n += static_cast<size_t>(w);
  }
}

// Power as a word: "USB", "87%" or "no battery".
const char *powerText(bool usb, int battery, char *buf, size_t cap) {
  if (usb) return "USB";
  if (battery < 0) return "no battery";
  snprintf(buf, cap, "%d%%", battery);
  return buf;
}

// In its own frame, its views on the heap: the dev tools' task has 4 KB of stack, and dump()
// uses most of it.
// The boards heard straight and their links (comms_mesh.h). On the heap: dump() runs in the dev
// tools' task, 6 KB of stack.
__attribute__((noinline)) void dumpLinks(Print &out) {
  struct Row {
    Neighbours::Neighbour n;
    std::string name;
    std::vector<std::string> names;  // of the boards its links list
  };
  std::unique_ptr<std::vector<Row>> rows(new std::vector<Row>());
  const uint32_t now = millis();
  {
    std::lock_guard<std::mutex> l(core->mu);
    for (size_t i = 0; i < core->links.size(); ++i) {
      rows->emplace_back();
      Row &r = rows->back();
      r.n = core->links.at(i);
      r.name = nameOf(r.n.id);
      for (uint8_t k = 0; k < r.n.count; ++k) r.names.push_back(nameOf(r.n.heard[k].id));
    }
  }
  out.printf("  links: sent %lu, taken %lu; voice and message frames relayed %lu, not relayed "
             "(nobody needed them) %lu%s",
             static_cast<unsigned long>(counters.linksSent.load()),
             static_cast<unsigned long>(counters.linksHeard.load()),
             static_cast<unsigned long>(counters.relayedAudio.load()),
             static_cast<unsigned long>(counters.audioNotNeeded.load()),
             floodAll.load() ? "; FLOODING every one (!commsflood)" : "");
  if (deafTo.load()) out.printf("; DEAF to %08lX", static_cast<unsigned long>(deafTo.load()));
  out.println();
  char id[12];
  for (const Row &r : *rows) {
    const Neighbours::Neighbour &n = r.n;
    const uint32_t ago = now - n.heardMs;
    out.printf("    %08lX %-16s straight %lu s ago%s, %d dBm, %u%% of its last 64 packets; "
               "this boot %lu came, %lu didn't (gaps 1: %lu, 2-3: %lu, 4-15: %lu, 16+: %lu)",
               static_cast<unsigned long>(n.id),
               r.name.empty() ? "(another group's)" : r.name.c_str(),
               static_cast<unsigned long>(ago / 1000),
               ago >= Neighbours::kHeardMs ? " (not a neighbour now)" : "", n.rssi,
               static_cast<unsigned>(n.pct()), static_cast<unsigned long>(n.got),
               static_cast<unsigned long>(n.missed), static_cast<unsigned long>(n.runs[0]),
               static_cast<unsigned long>(n.runs[1]), static_cast<unsigned long>(n.runs[2]),
               static_cast<unsigned long>(n.runs[3]));
    if (!n.listMs) {
      out.println("; no links from it (relayed for)");
      continue;
    }
    out.printf("; its links %lu s ago%s%s%s:", static_cast<unsigned long>((now - n.listMs) / 1000),
               now - n.listMs >= Neighbours::kListMs ? " (too old: relayed for)" : "",
               n.flags & kLinksRelays ? " RELAY" : "", n.flags & kLinksLong ? " LONG" : "");
    if (!n.count) out.print(" hears nobody");
    for (uint8_t k = 0; k < n.count; ++k) {
      const char *who;
      if (n.heard[k].id == core->id) {
        who = "THIS BOARD";
      } else if (!r.names[k].empty()) {
        who = r.names[k].c_str();
      } else {
        snprintf(id, sizeof id, "%08lX", static_cast<unsigned long>(n.heard[k].id));
        who = id;
      }
      out.printf(" %s %u%%,", who, static_cast<unsigned>(n.heard[k].pct));
    }
    out.println();
  }
}

__attribute__((noinline)) void dumpMic(Print &out) {
  struct Views {
    Mic m;
    std::vector<OpenMic> mics;
    audio::DuplexStats d;
    char bars[128];
    char power[16];
  };
  std::unique_ptr<Views> v(new Views());
  v->m = mic();
  v->mics = openMics(-1);
  v->d = audio::duplexStats();
  const Mic &m = v->m;
  if (m.on) {
    out.printf("  mic: ON (%s) in group %d to %s, session on %lu s, conversation %08lX%s, ",
               m.latched ? "LATCHED" : "held", m.group,
               m.target ? (m.targetName.empty() ? "?" : m.targetName.c_str()) : "ALL",
               static_cast<unsigned long>(m.ms / 1000), static_cast<unsigned long>(m.conversation),
               m.opened ? " (opened it)" : "");
    if (m.capturing) {
      out.printf("TRANSMITTING, level %d dBFS with the gain at %d dB; the noise gate %s, the "
                 "room at %d dBFS RMS",
                 static_cast<int>(lroundf(m.levelDb)), static_cast<int>(lroundf(m.gainDb)),
                 m.gateOpen ? "open" : "closed", static_cast<int>(lroundf(m.roomDb)));
    } else {
      out.printf("PAUSED (%s)", m.paused.c_str());
    }
    out.println(m.vanished ? " (vanished: nothing goes out)" : "");
  } else {
    out.println("  mic: off");
  }
  if (m.count) {
    barsText(m.bars, 10, v->bars, sizeof v->bars);
    out.printf("    last session: %lu frames sent, the first %lu ms after it went on, %lu "
               "still-heres, %lu histories; delay to a speaker about %lu ms; talked over %lu frames; "
               "bars (last 10):%s\n",
               static_cast<unsigned long>(m.framesSent), static_cast<unsigned long>(m.firstFrameMs),
               static_cast<unsigned long>(m.stillHeres), static_cast<unsigned long>(m.histories),
               static_cast<unsigned long>(m.delayMs),
               static_cast<unsigned long>(core->half.talkedOver), v->bars);
  }
  out.printf("  duplex: %s, amp %s, %lu blocks, %lu underruns, %lu mic blocks dropped, speaker %d "
             "dBFS, queued %d ms; the audio task's stack never used: %lu bytes\n",
             v->d.on ? "ON" : "off", v->d.ampOn ? "on" : "off",
             static_cast<unsigned long>(v->d.blocks), static_cast<unsigned long>(v->d.underruns),
             static_cast<unsigned long>(v->d.micDropped), static_cast<int>(lroundf(v->d.speakerDb)),
             static_cast<int>(lroundf(v->d.queuedMs)), static_cast<unsigned long>(v->d.stackFree));
  const Replay &rp = core->replay;
  if (rp.on && rp.src) {
    out.printf("  replay: %s from %08lX, frame %lu of %lu%s\n",
               rp.message ? "a message" : "PLAY LAST", static_cast<unsigned long>(rp.from),
               static_cast<unsigned long>(rp.pos),
               static_cast<unsigned long>(rp.src->size() / kStoredFrame),
               core->mic.on && core->mic.capturing ? ", waiting: the mic transmits" : "");
  }
  static const char *const kLinks[] = {"ON", "LOST"};
  for (const OpenMic &o : v->mics) {
    out.printf("  mic on: %08lX %s in group %d to %s, %s%s for %lu s%s, on %lu s, level %d dBFS, "
               "%s%s%s",
               static_cast<unsigned long>(o.id), o.name.empty() ? "?" : o.name.c_str(), o.group,
               o.target ? (o.toMe ? "THIS BOARD" : "another board") : "ALL",
               kLinks[static_cast<int>(o.link)], o.latched ? " LATCHED" : " (held)",
               static_cast<unsigned long>(o.linkMs / 1000),
               o.alarm ? " (ALARM)" : o.link == Link::Lost ? " (alarm silenced or not for here)" : "",
               static_cast<unsigned long>(o.onS), static_cast<int>(lroundf(o.levelDb)),
               o.heard ? "HEARD here" : "not heard here", o.playing ? ", PLAYING" : "",
               o.paused ? ", its mic paused" : "");
    out.printf(", %s, heard %lu ms ago, %s", powerText(o.usb, o.battery, v->power, sizeof v->power),
               static_cast<unsigned long>(o.heardAgoMs), routeName(o.route));
    if (o.route == Route::Air) out.printf(" %d dBm", o.rssi);
    out.printf(", delay %lu ms\n", static_cast<unsigned long>(o.delayMs));
    barsText(o.bars, 10, v->bars, sizeof v->bars);
    out.printf("    frames %lu, lost %lu, late %lu, histories %lu; bars (last 10):%s\n",
               static_cast<unsigned long>(o.frames), static_cast<unsigned long>(o.lost),
               static_cast<unsigned long>(o.late), static_cast<unsigned long>(o.histories), v->bars);
    int shown = 0;
    for (const MicEvent &e : o.events) {
      if (++shown > 6) break;
      out.printf("    LOST %lu s ago, %lu ms%s\n", static_cast<unsigned long>(e.agoS),
                 static_cast<unsigned long>(e.ms), e.live ? " (going)" : "");
    }
  }
}

}  // namespace

void dump(Print &out) {
  if (!core) {
    out.println("comms: not started (no group)");
    return;
  }
  const Status s = status();
  static const char *const kStates[] = {"OFF", "QUIET", "MIC ON", "HEARING"};
  uint8_t primary = 0;
  wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
  const bool chOk = esp_wifi_get_channel(&primary, &second) == ESP_OK;
  out.printf("comms: board %s (%08lX), boot %lu, state %s, %s, holds %d, USB %s\n",
             core->name.c_str(), static_cast<unsigned long>(core->id),
             static_cast<unsigned long>(core->boot), kStates[static_cast<int>(s.state)],
             isListening ? "listening" : "not listening", holders.load(),
             core->onUsb ? "yes" : "no");
  out.printf("  radio: channel %d%s, %s, mesh_channel %d%s, modem sleep %s, ESP-NOW %s\n",
             chOk ? primary : 0, chOk ? "" : " (Wi-Fi off)",
             WiFi.isConnected() ? ("on network " + std::string(WiFi.SSID().c_str()) +
                                   " (its channel)").c_str()
                                : "not on a network",
             meshChannel(), core->devChannel ? " (dev override)" : "",
             WiFi.getSleep() == WIFI_PS_NONE ? "off" : "ON (deaf to other boards)",
             espnowUp ? "up" : "down");
  {
    uint8_t proto = 0;
    const bool pOk = esp_wifi_get_protocol(WIFI_IF_STA, &proto) == ESP_OK;
    out.printf("  rate: sending at %s%s (mesh_range %s%s); protocols %s%s%s%s%s; modem sleep put "
               "off again %lu times\n",
               core->rangeLong ? "long range, 500 kbps" : "1 Mbps",
               core->rangeFailed ? " (long range won't take: no LR)"
               : wantLong() != core->rangeLong ? " (changing)" : "",
               wantLong() ? "long" : "normal", testRange() ? ", dev override" : "",
               pOk ? "" : "?", proto & WIFI_PROTOCOL_11B ? "B" : "",
               proto & WIFI_PROTOCOL_11G ? "G" : "", proto & WIFI_PROTOCOL_11N ? "N" : "",
               proto & WIFI_PROTOCOL_LR ? "+LR" : " (no LR: deaf to long range)",
               static_cast<unsigned long>(core->sleepFixes));
    out.print("  received by rate:");
    bool anyRate = false;
    for (int i = 0; i < 256; ++i) {
      if (!core->rxRates[i]) continue;
      out.printf(" %s %lu,", rateName(static_cast<uint8_t>(i)),
                 static_cast<unsigned long>(core->rxRates[i]));
      anyRate = true;
    }
    out.print(anyRate ? "" : " none");
    out.print("; sent by rate:");
    anyRate = false;
    for (int i = 0; i < 64; ++i) {
      if (!core->txRates[i]) continue;
      if (i == WIFI_PHY_RATE_LORA_500K) {
        out.print(" LR 500K");
      } else if (i == WIFI_PHY_RATE_LORA_250K) {
        out.print(" LR 250K");
      } else {
        out.print(" ");
        out.print(rateName(static_cast<uint8_t>(i & 0x1F)));
      }
      out.printf(" %lu,", static_cast<unsigned long>(core->txRates[i]));
      anyRate = true;
    }
    out.println(anyRate ? "" : " none");
    const uint32_t timed = txTimed.load();
    out.printf("  a full frame (200 bytes and up) from send to sent, alone in the radio: %lu us on "
               "average over %lu; Wi-Fi %d dBm\n",
               static_cast<unsigned long>(timed ? txUsSum.load() / timed : 0),
               static_cast<unsigned long>(timed), WiFi.isConnected() ? WiFi.RSSI() : 0);
  }
  out.printf("  relay for others %s, call chirp %s, net %s\n", relayForOthers() ? "on" : "off",
             callChirp() ? "on" : "off", netUp() ? "up" : "down (not built)");
  dumpLinks(out);
  out.printf("  counters for the screens: calls played %lu, rings %lu, conversations seen %lu "
             "(the last in group %d), messages in %lu\n",
             static_cast<unsigned long>(s.incoming), static_cast<unsigned long>(s.ringsSeen),
             static_cast<unsigned long>(s.conversationsSeen), s.conversationGroup,
             static_cast<unsigned long>(s.messagesIn));
  if (s.hearing || s.replay) {
    out.printf("  hearing %d voices%s, the loudest %s, level %d dBFS%s\n", s.voices,
               s.replay ? (s.playingMessage ? " and a message" : " and PLAY LAST") : "",
               s.peerName.empty() ? "?" : s.peerName.c_str(),
               static_cast<int>(lroundf(s.playLevelDb)), s.alarm ? "; LOST'S ALARM" : "");
  } else if (s.alarm) {
    out.println("  LOST's alarm sounds");
  }
  if (!s.error.empty()) out.printf("  last error: %s\n", s.error.c_str());
  const std::vector<Group> gs = groups();
  for (size_t i = 0; i < gs.size(); ++i) {
    const Group &g = gs[i];
    out.printf("  group %u %s %s (%08lX) via %s (card %s)%s: %d boards, %d on air, %d unheard%s\n",
               static_cast<unsigned>(i), g.name.c_str(), g.tag, static_cast<unsigned long>(g.id),
               viaName(g.via), viaName(g.cardVia), g.fromToken ? " from agent_token" : "",
               g.boards, g.onAir, g.unheard, g.active ? " [active]" : "");
    for (const Board &b : boards(static_cast<int>(i))) {
      static const char *const kRoutes[] = {"AWAY", "AIR", "RELAYED", "NET"};
      out.printf("    %08lX %-16s %s", static_cast<unsigned long>(b.id),
                 b.name.empty() ? "(no name yet)" : b.name.c_str(),
                 kRoutes[static_cast<int>(b.route)]);
      if (b.route == Route::Air) {
        out.printf(" %d dBm (noise %d) at %s", b.rssi, b.noise, b.rxRate.c_str());
      }
      if (b.route == Route::Relayed) {
        out.printf(" %d hop%s via %s", b.hops, b.hops == 1 ? "" : "s",
                   b.relayName.empty() ? "?" : b.relayName.c_str());
      }
      out.printf(", heard %lu s ago, flags%s%s%s%s, battery %d",
                 static_cast<unsigned long>(b.heardAgoS), b.flags & kFlagUsb ? " USB" : "",
                 b.flags & kFlagRelay ? " RELAY" : "", b.flags & kFlagMic ? " MIC" : "",
                 b.flags & kFlagLong ? " LONG" : "", b.battery);
      if (b.asleep) out.printf(", ASLEEP (reachable: a poll every %d s)", b.pollS);
      out.println();
    }
    const Conversation cv = conversation(static_cast<int>(i));
    if (!cv.on) {
      out.println("    conversation: none on");
    } else {
      std::string who = cv.in ? "YOU" : "";
      for (size_t k = 0; k < cv.members.size(); ++k) {
        char id[12];
        snprintf(id, sizeof id, "%08lX", static_cast<unsigned long>(cv.members[k]));
        who += (who.empty() ? "" : " + ") + (cv.names[k].empty() ? std::string(id) : cv.names[k]);
      }
      out.printf("    conversation %08lX on %lu s, quiet %lu s, %d mics on: %s; this board %s\n",
                 static_cast<unsigned long>(cv.id), static_cast<unsigned long>(cv.ms / 1000),
                 static_cast<unsigned long>(cv.quietMs / 1000), cv.micsOn, who.c_str(),
                 cv.in ? "IN it" : cv.left ? "LEFT it" : "not in it");
    }
  }
  {
    static const char *const kDial[] = {"none", "RINGING", "ANSWERED", "NO ANSWER", "RECORDING",
                                        "SENT"};
    static const char *const kEnd[] = {"",          "answered here", "answered there",
                                       "ignored",   "given up",      "no answer",
                                       "lost (no ring packet for 5 s)", "a message instead"};
    const Dial d = dial();
    out.printf("  dial: %s", kDial[static_cast<int>(d.state)]);
    if (d.state != DialState::None) {
      out.printf(" group %d to %s, %lu ms, %d ringing now%s", d.group,
                 d.target ? (d.targetName.empty() ? "?" : d.targetName.c_str()) : "ALL",
                 static_cast<unsigned long>(d.ms), d.ringing,
                 d.canLeaveMessage ? ", a message may be left" : "");
      if (d.answeredBy) {
        out.printf(", answered by %s",
                   d.answeredByName.empty() ? "?" : d.answeredByName.c_str());
      }
      if (d.state == DialState::Recording || d.state == DialState::Sent) {
        out.printf(", message %lu ms, %d have it whole", static_cast<unsigned long>(d.messageMs),
                   d.delivered);
      }
      if (core->dialing.vanished) out.print(" (ring packets stopped: !commsringvanish)");
    }
    out.println();
    const Ring r = incomingRing();
    if (r.on) {
      out.printf("  ringing here: %s rings %s in group %d, %lu ms\n",
                 r.fromName.empty() ? "?" : r.fromName.c_str(), r.toAll ? "ALL" : "THIS BOARD",
                 r.group, static_cast<unsigned long>(r.ms));
    } else if (r.ended != RingEnd::None) {
      out.printf("  ringing here: no; the last ring (from %s, %lu ms) ended: %s\n",
                 r.fromName.empty() ? "?" : r.fromName.c_str(), static_cast<unsigned long>(r.ms),
                 kEnd[static_cast<int>(r.ended)]);
    } else {
      out.println("  ringing here: no");
    }
  }
  const std::vector<Message> ms = messages(-1);
  out.printf("  messages kept: %u (of %d at most)%s\n", static_cast<unsigned>(ms.size()),
             kMaxMessages, store::available() ? ", the card is in" : ", no card: in RAM");
  for (const Message &m : ms) {
    out.printf("    #%lu from %s to %s, group %d, %lu s ago, %lu ms (%lu frames), %s%s%s, "
               "checksum %08lX\n",
               static_cast<unsigned long>(m.id), m.fromName.empty() ? "?" : m.fromName.c_str(),
               m.to ? "THIS BOARD" : "ALL", m.group, static_cast<unsigned long>(m.agoS),
               static_cast<unsigned long>(m.ms), static_cast<unsigned long>(m.frames),
               m.receiving ? "COMING" : m.complete ? "complete" : "INCOMPLETE",
               m.played ? ", played" : ", not played", m.onCard ? ", on the card" : ", in RAM",
               static_cast<unsigned long>(m.sum));
  }
  {
    const Outbound &o = core->out;
    if (o.recording || o.sent) {
      out.printf("  message leaving: %08lX, %u frames, checksum %08lX, %s, %u boards have it "
                 "whole\n",
                 static_cast<unsigned long>(o.msg),
                 static_cast<unsigned>(o.recording ? o.audio.size() / kStoredFrame : o.frames),
                 static_cast<unsigned long>(o.sum),
                 o.recording ? "recording" : o.resending ? "sending frames again" : "kept for asks",
                 static_cast<unsigned>(o.delivered.size()));
    }
    for (const Inbound &in : core->inbound) {
      if (!in.used) continue;
      out.printf("  message coming: %08lX from %08lX, %u frames in%s, %d asks\n",
                 static_cast<unsigned long>(in.msg), static_cast<unsigned long>(in.sender),
                 in.a.have(),
                 in.a.ended() ? (" of " + std::to_string(in.a.total())).c_str() : " (no end yet)",
                 in.asks);
    }
    if (dropNth.load() > 0) {
      out.printf("  dropping every %d message frames that come (!commsdropmsg)\n", dropNth.load());
    }
  }
  dumpMic(out);
  const std::vector<Call> cs = calls(-1, 8);
  if (!cs.empty()) out.println("  calls (newest first):");
  for (const Call &c : cs) {
    static const char *const kKinds[] = {"MIC", "RING", "MESSAGE"};
    out.printf("    %lu s ago, group %d, %s %s -> %s, %lu ms%s", static_cast<unsigned long>(c.agoS),
               c.group, kKinds[static_cast<int>(c.kind)],
               c.outgoing ? "YOU" : (c.fromName.empty() ? "?" : c.fromName.c_str()),
               c.to ? (c.toName.empty() ? "?" : c.toName.c_str()) : "ALL",
               static_cast<unsigned long>(c.ms), c.live ? " (live)" : "");
    if (c.kind == Kind::Ring) {
      out.printf(", %s%s\n", c.live ? "ringing" : c.answered ? "answered" : "not answered",
                 c.outgoing || c.live || c.heard ? "" : ", MISSED");
      continue;
    }
    if (c.kind == Kind::Message) {
      if (c.outgoing) {
        out.printf(", %d have it whole\n", c.heardBy);
      } else {
        out.printf(", %s, %s\n", c.complete ? "complete" : "incomplete",
                   c.played ? "played" : "not played");
      }
      continue;
    }
    if (c.conversation) out.printf(", conversation %08lX", static_cast<unsigned long>(c.conversation));
    if (c.outgoing) {
      out.println();
    } else {
      out.printf(", %s%s%s, %s, %s, %lu frames lost%s%s\n", c.hops ? "relayed " : "air ",
                 c.hops ? std::to_string(c.hops).c_str() : std::to_string(c.rssi).c_str(),
                 c.hops ? " hops" : " dBm", c.played ? "played" : "not played",
                 c.heard ? "heard" : "unheard", static_cast<unsigned long>(c.lost),
                 c.linkLost ? ", LOST on the way" : "", c.cutOff ? ", cut off" : "");
    }
  }
  const LastCall lc = lastCall();
  if (lc.have) {
    out.printf("  heard and kept for PLAY LAST: %lu ms, the last from %s\n",
               static_cast<unsigned long>(lc.ms), lc.fromName.empty() ? "?" : lc.fromName.c_str());
  }
  out.printf("  sent %lu (refused %lu, tx failed %lu), received %lu, handled %lu, relayed %lu "
             "(%lu of them unread, for groups not this board's)\n",
             static_cast<unsigned long>(counters.sent.load()),
             static_cast<unsigned long>(counters.sendFailed.load()),
             static_cast<unsigned long>(counters.txFailed.load()),
             static_cast<unsigned long>(counters.received.load()),
             static_cast<unsigned long>(counters.handled.load()),
             static_cast<unsigned long>(counters.relayed.load()),
             static_cast<unsigned long>(counters.relayedUnread.load()));
  out.print("  dropped:");
  bool any = false;
  for (int i = 0; i < kDropCount; ++i) {
    const uint32_t n = counters.dropped[i].load();
    if (!n) continue;
    out.printf(" %s %lu,", kDropNames[i], static_cast<unsigned long>(n));
    any = true;
  }
  out.println(any ? "" : " none");
  if (taskHandle) {
    out.printf("  task stack never used: %u bytes; frames waiting: %u\n",
               static_cast<unsigned>(uxTaskGetStackHighWaterMark(taskHandle)),
               static_cast<unsigned>(rxQueue ? uxQueueMessagesWaiting(rxQueue) : 0));
  }
}

}  // namespace comms

#endif  // INKDECK_COMMS
