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
#include "comms_crypto.h"
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
constexpr size_t kStoredFrame = 4 + kFrameBytes;  // predictor, index, bytes, then the data
constexpr uint8_t kFlagHello = 0x80;  // in a presence: answer with yours
constexpr int kMaxNameLen = 16;
const uint8_t kBroadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static_assert(kHeader + 4 + 4 + 2 + 2 + 2 + 1 + 1 + kFrameBytes + kTag <= kMaxFrame,
              "a voice packet must fit ESP-NOW v1's 250 bytes");

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
  kDropCount
};
const char *const kDropNames[kDropCount] = {
    "malformed", "queue full", "own (relayed back)", "duplicate", "other group (not relayed)",
    "bad tag",   "replayed",   "stale",              "via net",   "not for us",
    "while talking", "late frame", "relay queue full"};

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
};

struct CallRec {
  time_t at = 0;
  uint32_t startMs = 0;
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
  Slot slots[kSlots];
  Concealer conceal;
};

struct Talk {
  bool on = false;
  int group = 0;
  uint32_t target = 0;
  uint16_t call = 0;
  uint16_t frame = 0;
  uint32_t startMs = 0;
  adpcm::State enc;
  size_t fillBytes = 0;
  int16_t pcm[kFrameSamples];
};

struct Replay {
  bool on = false;
  size_t pos = 0;  // into lastAudio, in stored frames
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

enum class Cmd : uint8_t { Wake, Talk, TalkStop, PlayLast, StopPlay, Ping, Reload, Resend };
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
  bool relayOn = true, chirpOn = true;
  int devChannel = 0;

  // The task's own.
  uint32_t id = 0, boot = 0, seq = 0;
  std::string name;
  uint16_t callCounter = 0;
  uint32_t pingId = 0;
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

// Under mu.
CallRec &logCall(const CallRec &c) {
  if (core->log.size() >= static_cast<size_t>(kMaxCalls)) core->log.erase(core->log.begin());
  core->log.push_back(c);
  bump();
  return core->log.back();
}

// Under mu.
CallRec *findCall(int group, uint32_t from, uint16_t call, bool outgoing) {
  for (auto it = core->log.rbegin(); it != core->log.rend(); ++it) {
    if (it->group == group && it->from == from && it->call == call && it->outgoing == outgoing) {
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
  uint8_t body[8 + kMaxNameLen];
  Writer w(body, sizeof body);
  w.u32(sentAt());
  bool relay;
  {
    std::lock_guard<std::mutex> l(core->mu);
    relay = core->relayOn;
  }
  w.u8(static_cast<uint8_t>((core->onUsb ? kFlagUsb : 0) | (relay ? kFlagRelay : 0) |
                            (hello ? kFlagHello : 0)));
  const pmic::Status p = pmic::status();
  w.u8(p.ok && p.battery && p.percent >= 0 ? static_cast<uint8_t>(p.percent) : 0xFF);
  w.u8(static_cast<uint8_t>(core->name.size()));
  w.bytes(core->name.data(), core->name.size());
  sendOwn(g, kPresence, body, w.size());
}

// --- Audio -------------------------------------------------------------------------------------

// A call may play now: nothing that it mustn't interrupt is going on.
bool canPlay() {
  if (audio::recording() || audio::ringing()) return false;  // the Recorder, an alarm
  if (audio::capturing()) return false;                      // the agent listens
  if (audio::streaming() && !core->streamOurs) return false;  // the agent speaks
  return true;
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
                  uint32_t relay) {
  Hearing &h = core->hear;
  renew(h);
  h.active = true;
  h.group = gi;
  h.from = hd.sender;
  h.target = target;
  h.call = call;
  h.startMs = h.lastMs = millis();
  h.hops = hd.hops;
  h.relay = relay;
  h.rssi = hd.hops == 0 ? rssi : 0;
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
  } else if (hd.type == kVoiceEnd) {
    endFrames = r.u16();
  }
  if (!r.ok()) {
    drop(kDropMalformed);
    return;
  }
  if (target && target != core->id) {
    drop(kDropNotForUs);
    return;
  }
  if (core->talk.on) {  // half-duplex
    drop(kDropTalking);
    return;
  }
  if (core->finishedFrom == hd.sender && core->finishedCall == call) return;  // its tail
  Hearing &h = core->hear;
  if (h.active && !(h.from == hd.sender && h.call == call)) {
    otherCall(gi, hd, target, call, hd.type == kVoiceEnd);
    return;
  }
  if (!h.active) {
    if (hd.type == kVoiceEnd) return;  // all of it was missed
    beginHearing(gi, hd, target, call, f.rssi, relay);
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
  if (p.held) {
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

void startReplay() {
  if (core->talk.on || core->hear.active || core->replay.on) {
    std::lock_guard<std::mutex> l(core->mu);
    core->error = "BUSY";
    bump();
    return;
  }
  if (core->lastAudio.empty() || !canPlay()) {
    std::lock_guard<std::mutex> l(core->mu);
    core->error = core->lastAudio.empty() ? "NO CALL TO PLAY" : "AUDIO BUSY";
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
      if ((p.pos + 1) * kStoredFrame > core->lastAudio.size()) {
        stopReplay(false);
        return;
      }
      const uint8_t *stored = &core->lastAudio[p.pos * kStoredFrame];
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
  core->markFirstVoice = t.frame == 1;  // the one just numbered 0
  sendOwn(core->groups[t.group], kVoice, body, w.size());
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
  if (audio::recording()) return fail("THE RECORDER IS ON");
  if (audio::ringing()) return fail("AN ALARM IS RINGING");
  if (audio::capturing()) return fail("THE AGENT IS LISTENING");
  if (!isListening && !radioStart()) return fail("THE RADIO WON'T START");
  if (core->hear.active) endHearing(true);
  if (core->replay.on) stopReplay(true);
  rememberMusic();
  if (!audio::captureStart(true)) return fail("THE MIC WON'T START");
  renew(t);
  t.on = true;
  t.group = gi;
  t.target = target;
  t.call = ++core->callCounter;
  t.startMs = millis();
  uint8_t body[16];
  Writer w(body, sizeof body);
  w.u32(sentAt());
  w.u32(target);
  w.u16(t.call);
  w.u8(1);  // the codec: IMA ADPCM, 16 kHz mono, 20 ms frames
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
    c.to = target;
    c.call = t.call;
    logCall(c);
  }
  log_i("comms: talking in %s to %s", core->groups[gi].name.c_str(),
        target ? "one board" : "all");
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
  uint8_t body[16];
  Writer w(body, sizeof body);
  w.u32(sentAt());
  w.u32(t.target);
  w.u16(t.call);
  w.u16(t.frame);
  GroupRec &g = core->groups[t.group];
  for (const uint32_t after : {0u, 40u, 80u}) sendOwn(g, kVoiceEnd, body, w.size(), after);
  {
    std::lock_guard<std::mutex> l(core->mu);
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
  {
    std::lock_guard<std::mutex> l(core->mu);
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

  bool forUsAlone = false;
  switch (hd.type) {
    case kPresence: onPresence(g, hd, r); break;
    case kVoiceStart:
    case kVoice:
    case kVoiceEnd: {
      Reader peek(buf + kHeader + 4, bodyLen >= 4 ? bodyLen - 4 : 0);
      forUsAlone = peek.u32() == core->id;
      onVoice(gi, hd, f, r, relay);
      break;
    }
    case kPing: onPing(g, hd, f, r); break;
    case kPong: {
      Reader peek(buf + kHeader + 4, bodyLen >= 4 ? bodyLen - 4 : 0);
      forUsAlone = peek.u32() == core->id;
      onPong(hd, f, r);
      break;
    }
    case kHeard: {
      Reader peek(buf + kHeader + 4, bodyLen >= 4 ? bodyLen - 4 : 0);
      forUsAlone = peek.u32() == core->id;
      onHeard(gi, hd, r);
      break;
    }
    default: break;
  }
  if (hd.ttl > 0 && !forUsAlone) relayFrame(f, false);
}

// --- The task ----------------------------------------------------------------------------------

void publish() {
  Status s;
  const Hearing &h = core->hear;
  const Talk &t = core->talk;
  s.state = !isListening ? State::Off
            : t.on       ? State::Talking
            : (h.active && h.playing) || core->replay.on ? State::Hearing
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
  } else if (core->replay.on) {
    s.replay = true;
    s.group = core->last.group;
    s.peer = core->last.from;
    s.peerName = nameOf(core->last.from);
    s.ms = core->replay.frames * kFrameMs;
  }
  s.incoming = core->incoming;
  s.error = core->error;
  core->pub = std::move(s);
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
}

// Presence: at the start a hello, then every kPresenceMs; and the answer to a hello.
void presence(uint32_t now) {
  for (GroupRec &g : core->groups) {
    if (g.via == Via::Net) continue;
    if (static_cast<int32_t>(now - g.presenceDue) >= 0) {
      sendPresence(g, g.hello);
      g.hello = false;
      g.presenceDue = now + kPresenceMs + esp_random() % 3000;
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
  if (core->hear.active) endHearing(true);
  if (core->replay.on) stopReplay(true);
  radioStop();
  bump();
}

void reload() {
  if (isListening) stopListening();
  std::vector<GroupRec> loaded = loadGroups();
  {
    std::lock_guard<std::mutex> l(core->mu);
    freeGroups(core->groups);
    core->groups = std::move(loaded);
    core->log.clear();
    core->pongs.clear();
    core->last = LastCall();
    core->lastAudio.clear();
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
    case Cmd::PlayLast: startReplay(); break;
    case Cmd::StopPlay:
      if (core->replay.on) stopReplay(true);
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
    const bool busy = core->talk.on || core->hear.active || core->replay.on || talkWanted;
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

    // Frames from the radio: quick turns while a call is on, so the mic and the speaker keep up.
    const bool call = core->talk.on || core->hear.active || core->replay.on;
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
    Hearing &h = core->hear;
    if (h.active) {
      if (h.playing && holders.load() > 0) h.held = true;
      if (now - h.lastMs >= kCallTimeoutMs) endHearing(false);
    }
    if (!core->hear.active && !core->replay.on && !core->talk.on &&
        core->streamOurs && !audio::streaming()) {
      core->streamOurs = false;
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
    const bool live = core->talk.on || core->hear.active || core->replay.on;
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

bool startTask() {
  if (taskHandle) return true;
  rxQueue = xQueueCreateWithCaps(kRxDepth, sizeof(RxFrame), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!rxQueue) {
    log_e("comms: no memory for the receive queue");
    return false;
  }
  if (xTaskCreate(task, "comms", 6144, nullptr, 3, &taskHandle) != pdPASS) {
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
  core->boot = nextBoot();
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
    x.at = c.at;
    x.agoS = (now - c.startMs) / 1000;
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
    out.push_back(std::move(x));
  }
  return out;
}

void markHeard(int group) {
  if (!core) return;
  std::lock_guard<std::mutex> l(core->mu);
  bool changed = false;
  for (CallRec &c : core->log) {
    if ((group < 0 || c.group == group) && !c.outgoing && !c.heard && !c.live) {
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
  }
  const std::vector<Call> cs = calls(-1, 8);
  if (!cs.empty()) out.println("  calls (newest first):");
  for (const Call &c : cs) {
    out.printf("    %lu s ago, group %d, %s -> %s, %lu ms%s", static_cast<unsigned long>(c.agoS),
               c.group, c.outgoing ? "YOU" : (c.fromName.empty() ? "?" : c.fromName.c_str()),
               c.to ? (c.toName.empty() ? "?" : c.toName.c_str()) : "ALL",
               static_cast<unsigned long>(c.ms), c.live ? " (live)" : "");
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
