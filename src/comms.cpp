#include "comms.h"

#if INKDECK_COMMS  // the whole file (features.h)

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_mac.h>
#include <esp_now.h>
#include <esp_random.h>
#include <esp_wifi.h>
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
#include <mutex>
#include <new>

#include "adpcm.h"
#include "audio.h"
#include "comms_calls.h"
#include "comms_crypto.h"
#include "comms_store.h"
#include "comms_wire.h"
#include "kv_store.h"
#include "pmic.h"
#include "rtc.h"
#include "text.h"
#include "wlan.h"

namespace comms {
namespace {

using namespace wire;

// NVS ("comms"): id, the board's; boot, the last boot number; active, the active group's id;
// norelay and nochirp, 1 when those are off; via0..via3, a group's via changed on the board
// (the card line's hash with the via in its low 2 bits); devgrp and devchan, the dev overrides.
constexpr const char *kNs = "comms";

constexpr uint32_t kPresenceMs = 30000;   // each group hears where the board is this often
constexpr uint32_t kAwayMs = 95000;       // three presences missed: away
constexpr uint32_t kLingerMs = 10000;     // listening on after the last hold or unplugging
constexpr uint32_t kUsbPollMs = 5000;
constexpr uint32_t kCallTimeoutMs = 800;  // no voice this long: the call is over
constexpr uint32_t kHelloAnswerGapMs = 5000;
constexpr uint32_t kTickMs = 1000;
constexpr uint32_t kSleepCheckMs = 250;
constexpr uint32_t kChannelCheckMs = 1000;
constexpr uint32_t kCallBumpMs = 250;
constexpr size_t kRxDepth = 96;     // frames from the radio waiting for the task (PSRAM)
constexpr size_t kPending = 16;     // relays and delayed packets waiting to go
constexpr size_t kSlots = 8;        // the jitter buffer: 160 ms of frames
constexpr int kJitterWait = 3;      // frames ahead before a missing one is taken as lost
constexpr int kResyncFrames = 50;   // a jump this far (1 s) starts again there, no silence
constexpr size_t kLastFrames = 3000;  // PLAY LAST keeps 60 s
constexpr uint8_t kFlagHello = 0x80;  // in a presence: answer with yours
// Conversations, rings and messages (docs/comms-protocol.md).
constexpr uint32_t kConvPresenceMs = 4000;  // in a conversation, presence this often (+ 0-1 s)
constexpr uint8_t kVoiceOpening = 0x01;     // voice flags: this call opened its conversation
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
const uint8_t kBroadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static_assert(kHeader + 4 + 4 + 2 + 2 + 2 + 1 + 1 + kFrameBytes + 4 + 1 + kTag <= kMaxFrame,
              "a voice packet, its conversation too, must fit ESP-NOW v1's 250 bytes");
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
  kDropTalking,
  kDropLate,
  kDropRelayFull,
  kDropNotInConversation,
  kDropMessageTest,
  kDropUnknownType,
  kDropCount
};
const char *const kDropNames[kDropCount] = {
    "malformed", "queue full", "own (relayed back)", "duplicate", "other group (not relayed)",
    "bad tag",   "replayed",   "stale",              "via net",   "not for us",
    "while talking", "late frame", "relay queue full", "not in the conversation",
    "message frame (dropped to test)", "a later type (relayed only)"};

struct Counters {
  std::atomic<uint32_t> sent{0}, sendFailed{0}, txFailed{0}, received{0}, handled{0}, relayed{0};
  std::atomic<uint32_t> relayedUnread{0};  // of relayed: groups this board isn't in
  std::atomic<uint32_t> dropped[kDropCount];
};

struct RxFrame {
  uint8_t mac[6];
  int8_t rssi;
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
  uint32_t relayedMs = 0;  // its last packet through relays
  int hops = 0;
  uint32_t relay = 0;
  time_t talkedAt = 0;
  Route shown = Route::Away;  // as the last generation had it
  int shownRssi = 0;
  int shownHops = 0;
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
  uint16_t call = 0;
  uint32_t ms = 0;
  bool live = true;
  int hops = 0;
  uint32_t relay = 0;
  int rssi = 0;
  bool played = false, heard = false;
  uint32_t lost = 0;  // frames that never came, played as silence
  std::vector<uint32_t> heardBy;
  uint32_t conv = 0;      // Voice: its conversation
  bool answered = false;  // Ring
  bool missed = false;    // Ring, incoming: it rang out here, nobody answering or ignoring it
  bool complete = true;   // Message
  uint32_t message = 0;   // Message: its id in messages()
};

struct Slot {
  bool full = false;
  uint16_t frame = 0;
  int16_t predictor = 0;
  uint8_t index = 0;
  uint8_t bytes = 0;
  uint8_t data[kFrameBytes];
};

// Covers a lost 20 ms frame: the one before it again at half the level, then silence. A click-free
// gap for the odd frame the air loses (about 4% at -76 dBm between two boards, 2026-11-22).
struct Concealer {
  int16_t prev[kFrameSamples] = {};
  size_t samples = 0;
  int run = 0;  // lost frames in a row
  // pcm holds what decoded (or silence, for a lost frame); fixes up a lost one.
  void apply(int16_t *pcm, size_t n, bool lost) {
    if (!lost) {
      memcpy(prev, pcm, n * sizeof(int16_t));
      samples = n;
      run = 0;
      return;
    }
    if (run++ == 0 && samples) {
      for (size_t i = 0; i < n; ++i) pcm[i] = static_cast<int16_t>(prev[i % samples] / 2);
    }
  }
};

struct Hearing {
  bool active = false;
  int group = 0;
  uint32_t from = 0, target = 0;
  uint16_t call = 0;
  uint32_t startMs = 0, lastMs = 0;
  bool started = false;  // next is set
  uint16_t next = 0;     // the next frame to play
  bool playing = false;  // the stream is this call's now
  bool played = false;   // it ever was
  bool held = false;     // a hold() came while it played
  uint32_t frames = 0;   // played, lost ones as silence included
  uint32_t lost = 0;
  bool ending = false;
  uint16_t endFrames = 0;
  int hops = 0;
  uint32_t relay = 0;
  int rssi = 0;
  uint32_t conv = 0;       // its conversation (0: an older board's call, none)
  bool joinOnPlay = false; // this board joins the conversation once the call plays
  Slot slots[kSlots];
  Concealer conceal;
};

struct Talk {
  bool on = false;
  int group = 0;
  uint32_t target = 0;
  uint32_t conv = 0;
  bool opening = false;  // this call opens its conversation
  uint16_t call = 0;
  uint16_t frame = 0;
  uint32_t startMs = 0;
  adpcm::State enc;
  size_t fillBytes = 0;
  int16_t pcm[kFrameSamples];
};

struct Replay {
  bool on = false;
  const std::vector<uint8_t> *src = nullptr;  // lastAudio (PLAY LAST) or msgAudio (a message)
  uint32_t message = 0;  // the message playing, 0 for PLAY LAST
  int group = 0;
  uint32_t from = 0;
  size_t pos = 0;  // into src, in stored frames
  int16_t pcm[kFrameSamples];
  size_t pcmOff = 0, pcmLen = 0;  // bytes of pcm still to write
  uint32_t frames = 0;
  bool held = false;
  Concealer conceal;
};

// Another call while one plays: logged, not played.
struct Other {
  bool used = false;
  int group = 0;
  uint32_t from = 0;
  uint16_t call = 0;
  uint32_t lastMs = 0, frames = 0;
};

struct Pending {
  bool used = false;
  bool relay = false;
  bool unread = false;  // a relay for a group this board isn't in
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

enum class Cmd : uint8_t {
  Wake, Talk, TalkStop, PlayLast, StopPlay, Ping, Reload, Resend,
  Ring, RingStop, Answer, Ignore, Join, Leave, MessageStart, MessageSend, MessageCancel,
  PlayMessage, RingVanish,
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
  std::vector<uint8_t> lastAudio;  // the last call, kStoredFrame a frame
  std::vector<Pong> pongs;
  Status pub;
  Dial pubDial;
  Ring pubRing;
  std::vector<MsgRec> msgs;  // oldest first
  uint32_t msgCounter = 0;
  bool relayOn = true, chirpOn = true;
  int devChannel = 0;

  // The task's own.
  uint32_t id = 0, boot = 0, seq = 0;
  std::string name;
  uint16_t callCounter = 0;
  uint16_t ringCounter = 0;
  uint32_t pingId = 0;
  Dialing dialing;
  uint32_t dialCount = 0;
  Ringing ringing;
  uint32_t ringsSeen = 0;
  uint32_t conversationsSeen = 0;
  int conversationGroup = 0;
  uint32_t messagesIn = 0;
  Outbound out;
  Inbound inbound[kInbound];
  std::vector<uint8_t> msgAudio;  // the message playing
  SeenCache seen;
  ReplayGuard guard;
  Pending pending[kPending];
  Hearing hear;
  Talk talk;
  Replay replay;
  Other others[2];
  MacId macs[32];
  uint32_t macTick = 0;
  std::vector<uint8_t> recAudio;  // the call being heard, until it ends
  uint32_t finishedFrom = 0;      // the call that ended last: its stray packets are ignored
  uint16_t finishedCall = 0;
  Music music;
  bool streamOurs = false;
  // PLAY LAST is queued to the speaker far faster than it plays: once it's all queued, it still
  // counts as hearing until the stream has played out (status() and the screens).
  bool replayTail = false;
  float level = -96;
  float peaks[10] = {};  // the last 200 ms of played frames, for the meter
  int peakAt = 0;
  bool onUsb = false;
  std::string error;
  uint32_t incoming = 0;
  RxFrame rx;  // the frame being handled: too big for the task's stack
  // For !commsresend: the last frame this board sent, and the first voice frame of its last call.
  uint8_t lastSent[kMaxFrame];
  uint8_t lastSentLen = 0;
  uint8_t firstVoice[kMaxFrame];
  uint8_t firstVoiceLen = 0;
  bool markFirstVoice = false;
};

Core *core = nullptr;
Counters counters;
QueueHandle_t rxQueue = nullptr;
QueueHandle_t commands = nullptr;
TaskHandle_t taskHandle = nullptr;
std::atomic<int> holders{0};
std::atomic<uint32_t> gen{1};
std::atomic<bool> isListening{false};
std::atomic<bool> talkWanted{false};  // a Talk command is on its way or talking
std::atomic<bool> hasGroups{false};
std::atomic<bool> poweringOff{false};  // forceOff(): the radio stays off
std::atomic<int> dropNth{0};  // !commsdropmsg: every nth message frame that comes is dropped
uint32_t dropTick = 0;
std::mutex radioMu;  // the ESP-NOW calls, against forceOff()
bool espnowUp = false;

void bump() { gen.fetch_add(1, std::memory_order_relaxed); }

// x as freshly made, built where it is: `x = T()` would build a T on the task's stack first,
// and Hearing is 2 KB.
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

// Under mu.
CallRec *findCall(int group, uint32_t from, uint16_t call, bool outgoing) {
  for (auto it = core->log.rbegin(); it != core->log.rend(); ++it) {
    if (it->kind == Kind::Voice && it->group == group && it->from == from && it->call == call &&
        it->outgoing == outgoing) {
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
  if (len < static_cast<int>(kHeader + kTag) || len > static_cast<int>(kMaxFrame) ||
      data[0] != kMagic) {
    drop(kDropMalformed);
    return;
  }
  memcpy(f.mac, info->src_addr, 6);
  f.rssi = info->rx_ctrl ? static_cast<int8_t>(info->rx_ctrl->rssi) : 0;
  f.len = static_cast<uint8_t>(len);
  memcpy(f.data, data, len);
  if (!rxQueue || xQueueSend(rxQueue, &f, 0) != pdTRUE) drop(kDropQueueFull);
}

void onSent(const esp_now_send_info_t *, esp_now_send_status_t status) {
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
  if (esp_now_send(kBroadcast, frame, len) != ESP_OK) {
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
                  bool unread = false) {
  for (Pending &p : core->pending) {
    if (p.used) continue;
    p.used = true;
    p.relay = relay;
    p.unread = unread;
    p.due = millis() + delayMs;
    p.len = static_cast<uint8_t>(len);
    memcpy(p.data, frame, len);
    return true;
  }
  return false;
}

void sendPending(uint32_t now) {
  for (Pending &p : core->pending) {
    if (!p.used || static_cast<int32_t>(now - p.due) < 0) continue;
    p.used = false;
    if (radioSend(p.data, p.len) && p.relay) {
      counters.relayed.fetch_add(1);
      if (p.unread) counters.relayedUnread.fetch_add(1);
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
                            (hello ? kFlagHello : 0)));
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

// --- Audio -------------------------------------------------------------------------------------

// A call may play now: nothing that it mustn't interrupt is going on.
bool canPlay() {
  if (audio::recording() || audio::ringing()) return false;  // the Recorder, an alarm
  if (audio::capturing()) return false;                      // the agent listens
  if (audio::streaming() && !core->streamOurs) return false;  // the agent speaks
  return true;
}

// A ring's chirp, or a message's, may sound now: as canPlay(), and nothing of comms's plays or
// records either. (Music doesn't stop it: audio plays a ring over music.)
bool canChirp() {
  if (!canPlay() || audio::streaming()) return false;
  return !core->talk.on && !core->hear.active && !core->replay.on && !core->out.recording;
}

// Music that a call stops plays on afterwards.
void rememberMusic() {
  if (core->music.pending || !audio::playing()) return;
  const audio::Status s = audio::status();
  core->music.list = audio::queue(s.track, 64);
  core->music.ms = s.elapsedMs;
  core->music.pending = !core->music.list.empty();
}

void resumeMusicWhenFree() {
  Music &m = core->music;
  if (!m.pending || core->talk.on || core->hear.active || core->replay.on) return;
  if (audio::streaming() || audio::capturing()) return;
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

// --- Hearing -----------------------------------------------------------------------------------

void storeFrame(std::vector<uint8_t> &to, const Slot *s) {
  if (to.size() / kStoredFrame >= kLastFrames) return;
  const size_t at = to.size();
  to.resize(at + kStoredFrame, 0);
  if (!s) return;  // lost: stored as 0 bytes, which plays as 20 ms of silence
  to[at] = static_cast<uint8_t>(s->predictor);
  to[at + 1] = static_cast<uint8_t>(static_cast<uint16_t>(s->predictor) >> 8);
  to[at + 2] = s->index;
  to[at + 3] = s->bytes;
  memcpy(&to[at + 4], s->data, s->bytes);
}

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
  return bytes * 2u;
}

// Plays (or for a call that can't play, only keeps) the next frame; s is null for a lost one.
void playFrame(const Slot *s) {
  Hearing &h = core->hear;
  uint8_t stored[kStoredFrame] = {};
  if (s) {
    stored[0] = static_cast<uint8_t>(s->predictor);
    stored[1] = static_cast<uint8_t>(static_cast<uint16_t>(s->predictor) >> 8);
    stored[2] = s->index;
    stored[3] = s->bytes;
    memcpy(stored + 4, s->data, s->bytes);
  }
  int16_t pcm[kFrameSamples];
  const size_t samples = decodeStored(stored, pcm);
  h.conceal.apply(pcm, samples, !s);
  notePlayedLevel(peakDb(pcm, samples));
  if (h.playing) {
    const size_t want = samples * 2;
    const size_t n = audio::streamWrite(reinterpret_cast<const uint8_t *>(pcm), want, 20);
    if (n < want && !audio::streaming()) {  // the agent took the speaker
      h.playing = false;
      core->streamOurs = false;
    }
  }
  storeFrame(core->recAudio, s);
  ++h.frames;
  if (!s) ++h.lost;
}

// Plays what's ready in order. A missing frame is given up (as 20 ms of silence) once a frame
// kJitterWait further on has come, or at once when flushing.
void drain(bool flush) {
  Hearing &h = core->hear;
  if (!h.started) return;
  for (;;) {
    Slot &s = h.slots[h.next % kSlots];
    if (s.full && s.frame == h.next) {
      playFrame(&s);
      s.full = false;
      ++h.next;
      continue;
    }
    int ahead = -1;
    for (const Slot &x : h.slots) {
      if (x.full) ahead = std::max(ahead, static_cast<int>(static_cast<int16_t>(x.frame - h.next)));
    }
    if (ahead < 0) break;
    if (ahead >= kJitterWait || flush) {
      playFrame(nullptr);
      ++h.next;
      continue;
    }
    break;
  }
}

void beginHearing(int gi, const Header &hd, uint32_t target, uint16_t call, int rssi,
                  uint32_t relay, uint32_t conv, bool joinOnPlay) {
  Hearing &h = core->hear;
  renew(h);
  h.active = true;
  h.group = gi;
  h.from = hd.sender;
  h.target = target;
  h.call = call;
  h.conv = conv;
  h.joinOnPlay = joinOnPlay;
  h.startMs = h.lastMs = millis();
  h.hops = hd.hops;
  h.relay = relay;
  h.rssi = hd.hops == 0 ? rssi : 0;
  core->replayTail = false;
  core->recAudio.clear();
  resetLevel();
  const bool can = canPlay();
  {
    std::lock_guard<std::mutex> l(core->mu);
    CallRec c;
    c.at = utcNow();
    c.startMs = h.startMs;
    c.group = gi;
    c.from = hd.sender;
    c.to = target;
    c.call = call;
    c.hops = h.hops;
    c.relay = relay;
    c.rssi = h.rssi;
    c.conv = conv;
    logCall(c);
    if (BoardRec *b = findBoard(core->groups[gi], hd.sender)) b->talkedAt = c.at;
  }
  if (can) {
    if (core->replay.on) {  // a live call goes before PLAY LAST
      audio::streamAbort();
      core->replay.on = false;
    }
    rememberMusic();
    bool chirp;
    {
      std::lock_guard<std::mutex> l(core->mu);
      chirp = core->chirpOn;
    }
    if (chirp) audio::chirp(true);
    if (audio::streamStart(kVoiceRate)) {
      h.playing = h.played = true;
      core->streamOurs = true;
      ++core->incoming;
      if (joinOnPlay && conv) {  // it played here, so this board is in its conversation
        std::lock_guard<std::mutex> l(core->mu);
        core->groups[gi].conv.join(millis(), conv);
        presenceSoon(core->groups[gi]);
      }
    }
  }
  log_i("comms: call from %08lX in %s%s", static_cast<unsigned long>(hd.sender),
        core->groups[gi].name.c_str(), h.playing ? "" : " (busy: not played)");
  bump();
}

void endHearing(bool abort) {
  Hearing &h = core->hear;
  if (!h.active) return;
  if (!abort) drain(true);
  if (h.playing) {
    if (abort) {
      audio::streamAbort();
      core->streamOurs = false;
    } else {
      audio::streamEnd();
    }
  }
  const uint32_t ms = h.frames * kFrameMs;
  {
    std::lock_guard<std::mutex> l(core->mu);
    if (CallRec *c = findCall(h.group, h.from, h.call, false)) {
      c->ms = ms;
      c->live = false;
      c->lost = h.lost;
      c->played = h.played;
      c->heard = c->heard || (h.played && h.held);
    }
    if (h.frames > 0) {
      core->lastAudio.swap(core->recAudio);
      core->last.have = true;
      core->last.group = h.group;
      core->last.from = h.from;
      core->last.fromName = nameOf(h.from);
      core->last.ms = static_cast<uint32_t>(core->lastAudio.size() / kStoredFrame) * kFrameMs;
      core->last.at = utcNow() ? utcNow() - ms / 1000 : 0;
    }
  }
  core->recAudio.clear();
  core->recAudio.shrink_to_fit();
  if (h.played) {  // the caller counts who heard it
    uint8_t body[12];
    Writer w(body, sizeof body);
    w.u32(sentAt());
    w.u32(h.from);
    w.u16(h.call);
    sendOwn(core->groups[h.group], kHeard, body, w.size(), 50 + esp_random() % 200);
  }
  log_i("comms: call from %08lX over, %lu ms%s", static_cast<unsigned long>(h.from),
        static_cast<unsigned long>(ms), h.played ? "" : " (not played)");
  core->finishedFrom = h.from;
  core->finishedCall = h.call;
  h.active = false;
  resetLevel();
  bump();
}

// A frame (or the start or end) of a call that isn't the one playing: logged only.
void otherCall(int gi, const Header &hd, uint32_t target, uint16_t call, bool end) {
  Other *o = nullptr;
  for (Other &x : core->others) {
    if (x.used && x.from == hd.sender && x.call == call) o = &x;
  }
  const uint32_t now = millis();
  if (!o) {
    if (end) return;
    for (Other &x : core->others) {
      if (!x.used) o = &x;
    }
    if (!o) return;  // three at once: the third isn't even logged
    *o = Other{true, gi, hd.sender, call, now, 0};
    std::lock_guard<std::mutex> l(core->mu);
    CallRec c;
    c.at = utcNow();
    c.startMs = now;
    c.group = gi;
    c.from = hd.sender;
    c.to = target;
    c.call = call;
    c.hops = hd.hops;
    logCall(c);
  }
  o->lastMs = now;
  if (!end) ++o->frames;
  if (end) {
    std::lock_guard<std::mutex> l(core->mu);
    if (CallRec *c = findCall(o->group, o->from, o->call, false)) {
      c->ms = o->frames * kFrameMs;
      c->live = false;
    }
    o->used = false;
    bump();
  }
}

enum class Whose : uint8_t {
  Ours,  // this board is in its conversation (or it's an older board's, with none): it plays
  Join,  // it plays, and puts this board in its conversation
  No,    // another conversation's: not played, not logged
};

// Whether a call in conversation conv (to target, with the voice flags) plays here.
Whose whose(int gi, uint32_t conv, uint8_t flags, uint32_t target) {
  if (!conv) return Whose::Ours;  // a board from before conversations: as it always was
  std::lock_guard<std::mutex> l(core->mu);
  const ConvState &c = core->groups[gi].conv;  // a group has one: conv is it, or merges into it
  if (c.mine()) return Whose::Ours;
  if (c.left()) return Whose::No;  // left: nothing plays here until join()
  if (target == core->id) return Whose::Join;  // to this board alone
  if (flags & kVoiceOpening) return Whose::Join;  // its first call: the walkie-talkie
  return Whose::No;
}

void onVoice(int gi, const Header &hd, const RxFrame &f, Reader &r, uint32_t relay) {
  const uint32_t target = r.u32();
  const uint16_t call = r.u16();
  Slot in;
  uint16_t endFrames = 0;
  if (hd.type == kVoice) {
    in.frame = r.u16();
    in.predictor = static_cast<int16_t>(r.u16());
    in.index = r.u8();
    in.bytes = r.u8();
    const uint8_t *data = r.bytes(in.bytes);
    if (!r.ok() || in.bytes > kFrameBytes || in.index > 88) {
      drop(kDropMalformed);
      return;
    }
    memcpy(in.data, data, in.bytes);
    in.full = true;
  } else if (hd.type == kVoiceStart) {
    r.u8();  // the codec
  } else if (hd.type == kVoiceEnd) {
    endFrames = r.u16();
  }
  if (!r.ok()) {
    drop(kDropMalformed);
    return;
  }
  // The conversation, after the fields (a board from before conversations sends none).
  uint32_t conv = 0;
  uint8_t vflags = 0;
  if (r.left() >= 5) {
    conv = r.u32();
    vflags = r.u8();
  }
  if (conv && (hd.type != kVoice || in.frame % 10 == 0)) {  // a word in it, now and then
    std::lock_guard<std::mutex> l(core->mu);
    if (core->groups[gi].conv.spoke(millis(), conv, hd.sender, core->id)) bump();
  }
  if (target && target != core->id) {
    drop(kDropNotForUs);
    return;
  }
  if (core->talk.on || core->out.recording) {  // half-duplex (a message records like a talk)
    drop(kDropTalking);
    return;
  }
  if (core->finishedFrom == hd.sender && core->finishedCall == call) return;  // its tail
  Hearing &h = core->hear;
  const bool thisCall = h.active && h.from == hd.sender && h.call == call;
  const Whose w = thisCall ? Whose::Ours : whose(gi, conv, vflags, target);
  if (w == Whose::No) {
    drop(kDropNotInConversation);
    return;
  }
  if (h.active && !thisCall) {
    otherCall(gi, hd, target, call, hd.type == kVoiceEnd);
    return;
  }
  if (!h.active) {
    if (hd.type == kVoiceEnd) return;  // all of it was missed
    beginHearing(gi, hd, target, call, f.rssi, relay, conv, w == Whose::Join);
  }
  h.lastMs = millis();
  if (hd.type == kVoice) {
    if (!h.started) {
      h.started = true;
      h.next = in.frame <= kJitterWait ? 0 : in.frame;
    }
    int ahead = static_cast<int16_t>(in.frame - h.next);
    if (ahead < 0) {
      drop(kDropLate);
      return;
    }
    if (ahead > kResyncFrames) {  // a long gap: carry on from here
      for (Slot &s : h.slots) s.full = false;
      h.next = in.frame;
      ahead = 0;
    }
    while (ahead >= static_cast<int>(kSlots)) {  // no room: what's missing is lost
      drain(true);
      ahead = static_cast<int16_t>(in.frame - h.next);
      if (ahead >= static_cast<int>(kSlots)) {
        playFrame(nullptr);
        ++h.next;
        ahead = static_cast<int16_t>(in.frame - h.next);
      }
    }
    h.slots[in.frame % kSlots] = in;
    drain(false);
  } else if (hd.type == kVoiceEnd) {
    h.ending = true;
    h.endFrames = endFrames;
    endHearing(false);
  }
}

// --- Replay (PLAY LAST) --------------------------------------------------------------------------

void stopReplay(bool abort) {
  Replay &p = core->replay;
  if (!p.on) return;
  if (abort) {
    audio::streamAbort();
    core->streamOurs = false;
  } else {
    audio::streamEnd();
  }
  p.on = false;
  core->replayTail = !abort;
  if (p.held && !p.message) {
    std::lock_guard<std::mutex> l(core->mu);
    for (auto it = core->log.rbegin(); it != core->log.rend(); ++it) {
      if (!it->outgoing && it->from == core->last.from && it->group == core->last.group) {
        it->heard = true;
        break;
      }
    }
  }
  resetLevel();
  bump();
}

// Plays src (lastAudio for PLAY LAST, or msgAudio with the message's id) through the stream.
void startReplay(const std::vector<uint8_t> &src, uint32_t message, int group, uint32_t from) {
  if (core->talk.on || core->hear.active || core->replay.on || core->out.recording) {
    std::lock_guard<std::mutex> l(core->mu);
    core->error = "BUSY";
    bump();
    return;
  }
  if (src.empty() || !canPlay()) {
    std::lock_guard<std::mutex> l(core->mu);
    core->error = !src.empty() ? "AUDIO BUSY" : message ? "THE MESSAGE WON'T LOAD" : "NO CALL TO PLAY";
    bump();
    return;
  }
  rememberMusic();
  if (!audio::streamStart(kVoiceRate)) {
    std::lock_guard<std::mutex> l(core->mu);
    core->error = "THE SPEAKER WON'T START";
    bump();
    return;
  }
  core->streamOurs = true;
  renew(core->replay);
  core->replay.on = true;
  core->replay.src = &src;
  core->replay.message = message;
  core->replay.group = group;
  core->replay.from = from;
  resetLevel();
  {
    std::lock_guard<std::mutex> l(core->mu);
    core->error.clear();
  }
  bump();
}

void pumpReplay() {
  Replay &p = core->replay;
  if (!p.on) return;
  if (holders.load() > 0) p.held = true;
  for (int i = 0; i < 8; ++i) {
    if (p.pcmLen == 0) {
      if ((p.pos + 1) * kStoredFrame > p.src->size()) {
        stopReplay(false);
        return;
      }
      const uint8_t *stored = &(*p.src)[p.pos * kStoredFrame];
      const size_t samples = decodeStored(stored, p.pcm);
      p.conceal.apply(p.pcm, samples, stored[3] == 0);
      ++p.pos;
      ++p.frames;
      p.pcmOff = 0;
      p.pcmLen = samples * 2;
      notePlayedLevel(peakDb(p.pcm, samples));
    }
    const size_t n = audio::streamWrite(reinterpret_cast<const uint8_t *>(p.pcm) + p.pcmOff,
                                        p.pcmLen, 0);
    if (n == 0) {
      if (!audio::streaming()) {  // the speaker was taken
        core->streamOurs = false;
        p.on = false;
        bump();
      }
      return;  // the ring is full: on with it next time round
    }
    p.pcmOff += n;
    p.pcmLen -= n;
  }
}

// --- Talking -----------------------------------------------------------------------------------

void sendVoiceFrame(size_t samples) {
  Talk &t = core->talk;
  uint8_t body[kMaxBody];
  Writer w(body, sizeof body);
  w.u32(sentAt());
  w.u32(t.target);
  w.u16(t.call);
  w.u16(t.frame++);
  w.u16(static_cast<uint16_t>(t.enc.predictor));
  w.u8(t.enc.index);
  samples &= ~static_cast<size_t>(1);
  uint8_t data[kFrameBytes];
  adpcm::encode(t.enc, t.pcm, samples, data);
  w.u8(static_cast<uint8_t>(samples / 2));
  w.bytes(data, samples / 2);
  w.u32(t.conv);
  w.u8(t.opening ? kVoiceOpening : 0);
  core->markFirstVoice = t.frame == 1;  // the one just numbered 0
  sendOwn(core->groups[t.group], kVoice, body, w.size());
  if (t.frame % 25 == 1) {  // a word in the conversation, twice a second
    std::lock_guard<std::mutex> l(core->mu);
    core->groups[t.group].conv.spoke(millis(), t.conv, core->id, core->id);
  }
}

uint32_t newConvId() {
  uint32_t id;
  do {
    id = esp_random();
  } while (!id);
  return id;
}

void startTalk(int gi, uint32_t target) {
  Talk &t = core->talk;
  auto fail = [](const char *why) {
    std::lock_guard<std::mutex> l(core->mu);
    core->error = why;
    talkWanted = false;
    bump();
  };
  if (t.on) return;
  if (gi < 0 || gi >= static_cast<int>(core->groups.size())) return fail("NO SUCH GROUP");
  if (core->groups[gi].via == Via::Net) return fail("NET ONLY: NO LINK YET");
  if (core->out.recording) return fail("A MESSAGE IS RECORDING");
  if (audio::recording()) return fail("THE RECORDER IS ON");
  if (audio::ringing()) return fail("AN ALARM IS RINGING");
  if (audio::capturing()) return fail("THE AGENT IS LISTENING");
  if (!isListening && !radioStart()) return fail("THE RADIO WON'T START");
  if (core->hear.active) endHearing(true);
  if (core->replay.on) stopReplay(true);
  core->replayTail = false;
  rememberMusic();
  if (!audio::captureStart(true)) return fail("THE MIC WON'T START");
  renew(t);
  t.on = true;
  t.group = gi;
  t.target = target;
  t.call = ++core->callCounter;
  t.startMs = millis();
  {
    // The group's conversation: this board joins the one on and talks in it, to all of it (or
    // to a board of it: all of it hears). With none on, this call opens one.
    std::lock_guard<std::mutex> l(core->mu);
    ConvState &c = core->groups[gi].conv;
    t.opening = !c.on();
    c.join(t.startMs, t.opening ? newConvId() : 0);
    c.spoke(t.startMs, c.id(), core->id, core->id);
    t.conv = c.id();
    if (target && c.isMember(target)) t.target = 0;
  }
  presenceSoon(core->groups[gi]);
  uint8_t body[24];
  Writer w(body, sizeof body);
  w.u32(sentAt());
  w.u32(t.target);
  w.u16(t.call);
  w.u8(1);  // the codec: IMA ADPCM, 16 kHz mono, 20 ms frames
  w.u32(t.conv);
  w.u8(t.opening ? kVoiceOpening : 0);
  sendOwn(core->groups[gi], kVoiceStart, body, w.size());
  {
    std::lock_guard<std::mutex> l(core->mu);
    core->error.clear();
    CallRec c;
    c.at = utcNow();
    c.startMs = t.startMs;
    c.group = gi;
    c.outgoing = true;
    c.from = core->id;
    c.to = t.target;
    c.call = t.call;
    c.conv = t.conv;
    logCall(c);
  }
  log_i("comms: talking in %s to %s, conversation %08lX%s", core->groups[gi].name.c_str(),
        t.target ? "one board" : "all", static_cast<unsigned long>(t.conv),
        t.opening ? " (opens it)" : "");
  bump();
}

void stopTalk() {
  Talk &t = core->talk;
  if (!t.on) return;
  audio::captureStop();
  // The rest of the ring, then a short last frame.
  for (;;) {
    const size_t n = audio::captureRead(reinterpret_cast<uint8_t *>(t.pcm) + t.fillBytes,
                                        kFrameSamples * 2 - t.fillBytes, 0);
    if (n == 0) break;
    t.fillBytes += n;
    if (t.fillBytes == kFrameSamples * 2) {
      sendVoiceFrame(kFrameSamples);
      t.fillBytes = 0;
    }
  }
  if (t.fillBytes >= 4) sendVoiceFrame(t.fillBytes / 2);
  t.fillBytes = 0;
  uint8_t body[24];
  Writer w(body, sizeof body);
  w.u32(sentAt());
  w.u32(t.target);
  w.u16(t.call);
  w.u16(t.frame);
  w.u32(t.conv);
  w.u8(t.opening ? kVoiceOpening : 0);
  GroupRec &g = core->groups[t.group];
  for (const uint32_t after : {0u, 40u, 80u}) sendOwn(g, kVoiceEnd, body, w.size(), after);
  {
    std::lock_guard<std::mutex> l(core->mu);
    g.conv.spoke(millis(), t.conv, core->id, core->id);
    if (CallRec *c = findCall(t.group, core->id, t.call, true)) {
      c->ms = t.frame * kFrameMs;
      c->live = false;
    }
  }
  log_i("comms: talked %lu ms (%u frames)", static_cast<unsigned long>(millis() - t.startMs),
        static_cast<unsigned>(t.frame));
  t.on = false;
  talkWanted = false;
  resetLevel();
  bump();
}

void pumpTalk() {
  Talk &t = core->talk;
  if (!t.on) return;
  for (int i = 0; i < 4; ++i) {
    const size_t n = audio::captureRead(reinterpret_cast<uint8_t *>(t.pcm) + t.fillBytes,
                                        kFrameSamples * 2 - t.fillBytes, 0);
    if (n == 0) break;
    t.fillBytes += n;
    if (t.fillBytes == kFrameSamples * 2) {
      sendVoiceFrame(kFrameSamples);
      t.fillBytes = 0;
    }
  }
  core->level = audio::captureLevelDb();
  if (!audio::capturing() || millis() - t.startMs >= kMaxTalkMs) stopTalk();
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

void onPresence(GroupRec &g, const Header &hd, Reader &r) {
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
    if (b.name != n || b.flags != keep || b.battery != pct) {
      b.name = n;
      b.flags = keep;
      b.battery = pct;
      bump();
    }
  }
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

void onHeard(int gi, const Header &hd, Reader &r) {
  const uint32_t caller = r.u32();
  const uint16_t call = r.u16();
  if (!r.ok() || caller != core->id) return;
  std::lock_guard<std::mutex> l(core->mu);
  if (CallRec *c = findCall(gi, core->id, call, true)) {
    if (std::find(c->heardBy.begin(), c->heardBy.end(), hd.sender) == c->heardBy.end() &&
        c->heardBy.size() < 16) {
      c->heardBy.push_back(hd.sender);
      bump();
    }
  }
}

// --- Rings ---------------------------------------------------------------------------------------

void setError(const char *why) {
  std::lock_guard<std::mutex> l(core->mu);
  core->error = why;
  bump();
}

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
  if (target && target != core->id) return;  // for another board (relayed all the same)
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
    if (canChirp()) audio::ringChirp();
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
  adpcm::encode(o.enc, o.pcm, samples, s + 4);
  sendMessageFrame(o, static_cast<uint16_t>(at / kStoredFrame));
}

void startMessage(int group, uint32_t target) {
  Dialing &d = core->dialing;
  Outbound &o = core->out;
  const uint32_t now = millis();
  if (o.recording) return setError("A MESSAGE IS GOING");
  if (core->talk.on) return setError("TALKING");
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
  if (core->hear.active) endHearing(true);
  if (core->replay.on) stopReplay(true);
  core->replayTail = false;
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
    core->level = audio::captureLevelDb();
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
  if (core->talk.on || core->hear.active || core->replay.on || core->out.recording) {
    return setError("BUSY");
  }
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
  {
    std::lock_guard<std::mutex> l(core->mu);
    if (!g.conv.leave(millis())) return;
  }
  Hearing &h = core->hear;
  if (h.active && h.group == gi && h.conv && h.playing) {  // its voice stops here
    audio::streamAbort();
    h.playing = false;
    core->streamOurs = false;
  }
  log_i("comms: left the conversation in %s", g.name.c_str());
  presenceSoon(g);
  bump();
}

// Passes a packet on, unchanged but for its ttl and hops, a few ms later so relays in range of
// each other don't all send at once.
void relayFrame(const RxFrame &f, bool unread) {
  bool on;
  {
    std::lock_guard<std::mutex> l(core->mu);
    on = core->relayOn;
  }
  if (!on) return;
  uint8_t copy[kMaxFrame];
  memcpy(copy, f.data, f.len);
  if (!forward(copy)) return;
  if (!queuePending(copy, f.len, 1 + esp_random() % 6, true, unread)) drop(kDropRelayFull);
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
  const int gi = groupIndex(hd.group);
  if (gi < 0) {  // not ours: passed on unread
    if (core->seen.seen(hd.group, hd.sender, hd.boot, hd.seq)) {
      drop(kDropDuplicate);
      return;
    }
    core->seen.add(hd.group, hd.sender, hd.boot, hd.seq);
    if (hd.ttl > 0) {
      relayFrame(f, true);
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
      learnMac(f.mac, hd.sender);
    } else {
      b.relayedMs = now;
      b.hops = hd.hops;
      b.relay = relay = macToId(f.mac);
    }
    noteShown(b, now);
  }

  // A packet for this board alone isn't relayed: the board it's for is the one its first field
  // after sentAt names (a message frame's or end's, the second). An answer floods regardless,
  // so every board the ring rang stops.
  const size_t aloneAt = hd.type == kMessage || hd.type == kMessageEnd ? 8 : 4;
  Reader peek(buf + kHeader + aloneAt, bodyLen >= aloneAt ? bodyLen - aloneAt : 0);
  const bool forUsAlone = hd.type != kPresence && hd.type != kPing && hd.type != kAnswer &&
                          knownType(hd.type) && peek.u32() == core->id;
  switch (hd.type) {
    case kPresence: onPresence(g, hd, r); break;
    case kVoiceStart:
    case kVoice:
    case kVoiceEnd: onVoice(gi, hd, f, r, relay); break;
    case kPing: onPing(g, hd, f, r); break;
    case kPong: onPong(hd, f, r); break;
    case kHeard: onHeard(gi, hd, r); break;
    case kRing: onRing(gi, hd, r); break;
    case kRinging: onRinging(hd, r); break;
    case kAnswer: onAnswer(gi, hd, r); break;
    case kRingEnd: onRingEnd(hd, r); break;
    case kMessage: onMessage(gi, hd, r); break;
    case kMessageEnd: onMessageEnd(gi, hd, r); break;
    case kMessageAsk: onMessageAsk(hd, r); break;
    default: drop(kDropUnknownType); break;  // a later version's: passed on all the same
  }
  if (hd.ttl > 0 && !forUsAlone) relayFrame(f, false);
}

// --- The task ----------------------------------------------------------------------------------

void publish() {
  Status s;
  const Hearing &h = core->hear;
  const Talk &t = core->talk;
  const bool replay =
      core->replay.on || (core->replayTail && core->streamOurs && audio::streaming());
  s.state = !isListening ? State::Off
            : t.on       ? State::Talking
            : (h.active && h.playing) || replay ? State::Hearing
                                                 : State::Quiet;
  s.channel = radioChannel();
  s.onNetwork = WiFi.isConnected();
  s.netUp = false;
  s.levelDb = core->level;
  std::lock_guard<std::mutex> l(core->mu);
  s.group = core->active;
  if (t.on) {
    s.group = t.group;
    s.peer = t.target;
    s.peerName = nameOf(t.target);
    s.toAll = t.target == 0;
    s.ms = millis() - t.startMs;
  } else if (h.active) {
    s.group = h.group;
    s.peer = h.from;
    s.peerName = nameOf(h.from);
    s.toAll = h.target == 0;
    s.ms = h.frames * kFrameMs;
    s.route = h.hops ? Route::Relayed : Route::Air;
    s.hops = h.hops;
    s.relayName = nameOf(h.relay);
    s.rssi = h.rssi;
  } else if (replay) {
    s.replay = true;
    s.group = core->replay.group;
    s.peer = core->replay.from;
    s.peerName = nameOf(core->replay.from);
    s.ms = core->replay.frames * kFrameMs;
    s.playingMessage = core->replay.message;
  } else if (core->out.recording) {  // the mic is on for a message: Talking, as for a call
    s.state = State::Talking;
    s.group = core->out.group;
    s.peer = core->out.to;
    s.peerName = nameOf(core->out.to);
    s.toAll = core->out.to == 0;
    s.ms = millis() - core->out.startMs;
  }
  s.incoming = core->incoming;
  s.ringsSeen = core->ringsSeen;
  s.conversationsSeen = core->conversationsSeen;
  s.conversationGroup = core->conversationGroup;
  s.messagesIn = core->messagesIn;
  s.error = core->error;
  core->pub = std::move(s);

  const uint32_t now = millis();
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
}

void tick(uint32_t now) {
  // Boards that went away.
  {
    std::lock_guard<std::mutex> l(core->mu);
    for (GroupRec &g : core->groups) {
      for (BoardRec &b : g.boards) noteShown(b, now);
    }
  }
  // Other calls that stopped without an end.
  for (Other &o : core->others) {
    if (!o.used || now - o.lastMs < kCallTimeoutMs) continue;
    std::lock_guard<std::mutex> l(core->mu);
    if (CallRec *c = findCall(o.group, o.from, o.call, false)) {
      c->ms = o.frames * kFrameMs;
      c->live = false;
    }
    o.used = false;
    bump();
  }
  // Conversations: members that went quiet, conversations over; one this board isn't in that
  // has come on is counted once (status().conversationsSeen: the screens offer to join it).
  {
    std::lock_guard<std::mutex> l(core->mu);
    for (size_t i = 0; i < core->groups.size(); ++i) {
      GroupRec &g = core->groups[i];
      ConvState &c = g.conv;
      if (c.mine() && core->talk.on && core->talk.group == static_cast<int>(i)) {
        c.spoke(now, core->talk.conv, core->id, core->id);
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
  for (GroupRec &g : core->groups) {
    g.hello = true;
    g.presenceDue = now + esp_random() % 300;
  }
  bump();
}

void stopListening() {
  if (core->talk.on) stopTalk();
  if (core->out.recording) finishMessage(true);
  if (core->hear.active) endHearing(true);
  if (core->replay.on) stopReplay(true);
  if (core->dialing.state == DialState::Ringing) dialOver(DialState::None, kEndGaveUp);
  ringingOver(RingEnd::Lost);
  for (Inbound &in : core->inbound) finishInbound(in);
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
    core->lastAudio.clear();
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
    case Cmd::Talk: startTalk(c.group, c.arg); break;
    case Cmd::TalkStop:
      if (core->talk.on) {
        stopTalk();
      } else {
        talkWanted = false;
      }
      break;
    case Cmd::PlayLast: {
      LastCall last;
      {
        std::lock_guard<std::mutex> l(core->mu);
        last = core->last;
      }
      startReplay(core->lastAudio, 0, last.group, last.from);
      break;
    }
    case Cmd::StopPlay:
      if (core->replay.on) stopReplay(true);
      if (core->replayTail && core->streamOurs && !core->hear.active) {  // still playing out
        audio::streamAbort();
        core->streamOurs = false;
        core->replayTail = false;
        bump();
      }
      if (core->hear.active && core->hear.playing) {
        audio::streamAbort();
        core->hear.playing = false;
        core->streamOurs = false;
        bump();
      }
      break;
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
            c.arg ? "the first voice frame of the last call" : "the last frame", n);
      break;
    }
  }
}

void task(void *) {
  uint32_t lastUsb = 0, lastSleep = 0, lastChannel = 0, lastTick = 0, lastBump = 0,
           lastPublish = 0, lingerFrom = 0;
  bool usbKnown = false;
  for (;;) {
    uint32_t now = millis();
    if (!usbKnown || now - lastUsb >= kUsbPollMs) {
      const pmic::Status p = pmic::status();
      core->onUsb = p.ok && p.vbus;
      lastUsb = now;
      usbKnown = true;
    }
    bool inbound = false;
    for (const Inbound &in : core->inbound) inbound = inbound || in.used;
    const bool ringOn = core->dialing.state == DialState::Ringing || core->ringing.on;
    const bool msgOn = core->out.recording || core->out.resending || inbound;
    const bool busy = core->talk.on || core->hear.active || core->replay.on || talkWanted ||
                      ringOn || msgOn;
    const bool want = hasGroups && !poweringOff && (holders.load() > 0 || core->onUsb || busy);
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

    // Frames from the radio: quick turns while a call is on, so the mic and the speaker keep up,
    // and while a message is recorded or frames of one go again.
    const bool call = core->talk.on || core->hear.active || core->replay.on ||
                      core->out.recording || core->out.resending;
    RxFrame &frame = core->rx;
    if (xQueueReceive(rxQueue, &frame, pdMS_TO_TICKS(call ? 4 : 20)) == pdTRUE) {
      int n = 0;
      do {
        handleFrame(frame);
      } while (++n < 16 && xQueueReceive(rxQueue, &frame, 0) == pdTRUE);
    }
    now = millis();
    sendPending(now);
    pumpTalk();
    pumpReplay();
    pumpMessageOut(now);
    pumpDial(now);
    pumpRinging(now);
    pumpInbound(now);
    Hearing &h = core->hear;
    if (h.active) {
      if (h.playing && holders.load() > 0) h.held = true;
      if (now - h.lastMs >= kCallTimeoutMs) endHearing(false);
    }
    if (!core->hear.active && !core->replay.on && !core->talk.on && !core->out.recording &&
        core->streamOurs && !audio::streaming()) {
      core->streamOurs = false;
      if (core->replayTail) {  // PLAY LAST has played out
        core->replayTail = false;
        bump();
      }
    }
    resumeMusicWhenFree();
    presence(now);
    if (now - lastSleep >= kSleepCheckMs) {
      lastSleep = now;
      if (WiFi.getSleep() != WIFI_PS_NONE) WiFi.setSleep(false);  // another feature turned it on
    }
    if (now - lastChannel >= kChannelCheckMs) {
      lastChannel = now;
      keepChannel();
    }
    if (now - lastTick >= kTickMs) {
      lastTick = now;
      tick(now);
    }
    const bool live = core->talk.on || core->hear.active || core->replay.on || ringOn ||
                      core->out.recording;
    if (live && now - lastBump >= kCallBumpMs) {
      lastBump = now;
      bump();
    }
    if (now - lastPublish >= (live ? 50u : 200u)) {
      lastPublish = now;
      publish();
    }
  }
}

// The task's stack, in internal RAM. Writing a message to the card (FatFs keeps long names on the
// stack) needs more than the calls alone did.
constexpr uint32_t kStack = 7168;

bool startTask() {
  if (taskHandle) return true;
  rxQueue = xQueueCreateWithCaps(kRxDepth, sizeof(RxFrame), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
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
  log_i("comms: %u groups, board %s (%08lX), boot %lu", static_cast<unsigned>(core->groups.size()),
        core->name.c_str(), static_cast<unsigned long>(core->id),
        static_cast<unsigned long>(core->boot));
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
    x.heardAt = b.heardAt;
    x.heardAgoS = b.heardMs ? (now - b.heardMs) / 1000 : 0;
    x.talkedAt = b.talkedAt;
    x.flags = b.flags;
    x.battery = b.battery;
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

bool talkStart(int group, uint32_t target) {
  if (!core) return false;
  auto fail = [](const char *why) {
    std::lock_guard<std::mutex> l(core->mu);
    core->error = why;
    core->pub.error = why;
    bump();
    return false;
  };
  if (!hasGroups) return fail("NO GROUP SET");
  {
    std::lock_guard<std::mutex> l(core->mu);
    if (group < 0 || group >= static_cast<int>(core->groups.size())) {
      core->error = core->pub.error = "NO SUCH GROUP";
      bump();
      return false;
    }
    if (core->groups[group].via == Via::Net) {
      core->error = core->pub.error = "NET ONLY: NO LINK YET";
      bump();
      return false;
    }
  }
  if (audio::recording()) return fail("THE RECORDER IS ON");
  if (audio::ringing()) return fail("AN ALARM IS RINGING");
  if (talkWanted.exchange(true)) return true;  // already on
  post(Cmd::Talk, group, target);
  return true;
}

void talkStop() {
  if (talkWanted.load()) post(Cmd::TalkStop);
}

bool talking() { return talkWanted.load(); }

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
      core->error = core->pub.error = "NO CALL TO PLAY";
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
  out.ms = now - c.startMs();
  out.quietMs = now - c.lastWordMs();
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

void forceOff() {
  poweringOff = true;
  std::lock_guard<std::mutex> g(radioMu);
  if (!espnowUp) return;
  esp_now_unregister_recv_cb();
  esp_now_unregister_send_cb();
  esp_now_deinit();
  espnowUp = false;
  isListening = false;
}

void dump(Print &out) {
  if (!core) {
    out.println("comms: not started (no group)");
    return;
  }
  const Status s = status();
  static const char *const kStates[] = {"OFF", "QUIET", "TALKING", "HEARING"};
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
  out.printf("  relay for others %s, call chirp %s, net %s\n", relayForOthers() ? "on" : "off",
             callChirp() ? "on" : "off", netUp() ? "up" : "down (not built)");
  out.printf("  counters for the screens: calls played %lu, rings %lu, conversations seen %lu "
             "(the last in group %d), messages in %lu\n",
             static_cast<unsigned long>(s.incoming), static_cast<unsigned long>(s.ringsSeen),
             static_cast<unsigned long>(s.conversationsSeen), s.conversationGroup,
             static_cast<unsigned long>(s.messagesIn));
  if (s.state == State::Talking || s.state == State::Hearing) {
    out.printf("  %s %s%s, %lu ms, level %.0f dBFS\n",
               s.state == State::Talking ? "talking to" : "hearing",
               s.peerName.empty() ? (s.peer ? "?" : "all") : s.peerName.c_str(),
               s.replay ? " (play last)" : "", static_cast<unsigned long>(s.ms),
               static_cast<double>(s.levelDb));
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
      if (b.route == Route::Air) out.printf(" %d dBm", b.rssi);
      if (b.route == Route::Relayed) {
        out.printf(" %d hop%s via %s", b.hops, b.hops == 1 ? "" : "s",
                   b.relayName.empty() ? "?" : b.relayName.c_str());
      }
      out.printf(", heard %lu s ago, flags%s%s%s, battery %d\n",
                 static_cast<unsigned long>(b.heardAgoS), b.flags & kFlagUsb ? " USB" : "",
                 b.flags & kFlagRelay ? " RELAY" : "", b.flags & kFlagRoom ? " ROOM" : "",
                 b.battery);
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
      out.printf("    conversation %08lX on %lu s, quiet %lu s: %s; this board %s\n",
                 static_cast<unsigned long>(cv.id), static_cast<unsigned long>(cv.ms / 1000),
                 static_cast<unsigned long>(cv.quietMs / 1000), who.c_str(),
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
  const std::vector<Call> cs = calls(-1, 8);
  if (!cs.empty()) out.println("  calls (newest first):");
  for (const Call &c : cs) {
    static const char *const kKinds[] = {"call", "RING", "MESSAGE"};
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
      out.printf(", %d heard\n", c.heardBy);
    } else {
      out.printf(", %s%s%s, %s, %s, %lu frames lost\n", c.hops ? "relayed " : "air ",
                 c.hops ? std::to_string(c.hops).c_str() : std::to_string(c.rssi).c_str(),
                 c.hops ? " hops" : " dBm", c.played ? "played" : "not played",
                 c.heard ? "heard" : "unheard", static_cast<unsigned long>(c.lost));
    }
  }
  const LastCall lc = lastCall();
  if (lc.have) {
    out.printf("  last call kept: %s, %lu ms\n", lc.fromName.c_str(),
               static_cast<unsigned long>(lc.ms));
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
