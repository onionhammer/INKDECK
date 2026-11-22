#include "features.h"

#if INKDECK_AGENT  // the whole file (features.h)

#include "agent_client.h"

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <cJSON.h>
#include <errno.h>
#include <esp_heap_caps.h>
#include <fcntl.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>
#include <mbedtls/base64.h>
#include <string.h>
#include <time.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <vector>

#include "agent_noise.h"
#include "audio.h"
#include "input.h"
#include "pmic.h"
#include "power.h"
#include "settings.h"
#include "tailnet.h"
#include "wlan.h"

namespace agent {
namespace {

constexpr int kProto = 2;  // encrypted (agent_noise.h)
constexpr uint8_t kControl = 1, kAudio = 2, kHandshake = 3, kSealed = 4;  // frame types
constexpr size_t kHeader = 3;
// A sealed frame: the header, then the type and payload of the frame inside, then the tag.
constexpr size_t kSealedPayload = kHeader + 1;
constexpr size_t kMaxSealed = 65535 - 1 - noise::kTag;  // the largest payload one can carry
constexpr size_t kMicFrame = 1024;                    // 32 ms of the mic per frame
constexpr size_t kRxBytes = kHeader + 65535 + 4096;   // the largest frame, and some of the next
constexpr size_t kMaxText = 1500;                     // of the reply kept for the screen
constexpr uint32_t kMaxTalkMs = 30000;
constexpr uint32_t kAnswerTimeoutMs = 45000;  // of silence from the daemon in an open turn
// After the last holder lets go (the sheet closed, the Agent left), the connection waits this
// long before it ends: one screen of the Agent often gives way to another (the sheet over the
// list, the list under a session), and Wi-Fi takes seconds to join again.
constexpr uint32_t kLingerMs = 2500;

enum class Event : uint8_t {
  Open, Close, TalkStart, TalkStop, Hold, Release, Retry, Kick, CheckIn, Speak, Read
};

// Check-ins: the poll ladder (minutes), and how long a poll may take.
constexpr uint32_t kLadderMin[] = {5, 15, 30};
constexpr uint32_t kMaxPollGapMs = 30 * 60 * 1000;
constexpr uint32_t kPollDeadlineMs = 20000;   // joining and connecting
constexpr uint32_t kPollListMs = 8000;        // then waiting for the list
constexpr uint32_t kIdleLiveMs = 120000;      // an Agent screen this long without a key: polls
constexpr uint32_t kUsbCheckMs = 5000;
constexpr uint32_t kSoonGapMs = 60000;        // checkSoon() at most this often
constexpr uint32_t kFloorS = 60;              // check_at sooner than this is taken as this
constexpr uint16_t kSummaryVersion = 1;
constexpr char kNvs[] = "agent";

QueueHandle_t events = nullptr;
SemaphoreHandle_t lock = nullptr;  // guards `shared` and `answers`
Status shared;
std::atomic<State> state{State::Off};
std::atomic<uint32_t> generation{0};
std::atomic<uint32_t> turns{0};

// The agent: what the daemon has said (agent()), and what the screens want sent. Guarded by
// `lock`, except holds.
Target (*targetSource)() = nullptr;
std::atomic<int> holds{0};        // agentHold() minus agentRelease()
Sessions agentShared;
std::vector<std::string> outbox;  // answer, stop and delete, waiting to go out
std::string wantOpen;             // agentOpen()'s session
std::string pendingPin;           // agentUnlock()'s digits, until the daemon answers

// Check-ins (agent task only, unless atomic).
std::atomic<uint32_t> nextPollMs{0};  // millis; 0: none planned
uint32_t lastPollMs = 0;
uint8_t ladder = 0;
uint8_t pollFailures = 0;
uint32_t lastSignature = 0;
std::atomic<bool> pollActive{false};  // a poll's connection, until its list comes
uint32_t pollStartMs = 0;
uint32_t pollLists = 0;
uint32_t listCount = 0;       // lists received since boot
bool usbLive = false;         // on USB power: the connection stays up
uint32_t usbCheckedMs = 0;
uint32_t liveRetryMs = 0;     // a failed live connect waits until then
uint8_t liveFailures = 0;
std::atomic<bool> soon{false};
std::atomic<bool> talkWanted{false};    // a talk press is queued: a background connect yields
std::atomic<uint32_t> nextCheckUnix{0};
SemaphoreHandle_t checkInDone = nullptr;
uint32_t checkInMs = 0;
bool checkInOk = false;
std::string speakSession;     // guarded by `lock`

// What the last list said, as the Agent face keeps it (guarded by `lock`), and when it came.
Summary summaryRam = {};
Summary summarySaved = {};
std::atomic<uint32_t> checkedAtUnix{0};
uint32_t checkedSavedUnix = 0;
bool (*contentPolicy)() = nullptr;

// readPage()'s request, waiting for its page (guarded by `lock`, except the atomics).
struct ReadRequest {
  uint32_t id = 0;
  std::string url;
  uint16_t pictureW = 0, pictureH = 0;
  bool sent = false;
  bool done = false;
  bool reached = false;  // the daemon answered
  Page page;
  std::string error;
};
ReadRequest reading;
uint32_t nextReadId = 1;
std::atomic<bool> readActive{false};  // a readPage() is waiting: the connection stays up
SemaphoreHandle_t readDone = nullptr;

// The board's tools (setTools()), and the answers waiting to go out (toolResult()).
const char *toolDefinitions = nullptr;
ToolCall toolCall = nullptr;
struct Answer {
  uint32_t id;
  std::string text;
  bool error;
};
std::vector<Answer> answers;

struct Locked {
  Locked() { xSemaphoreTake(lock, portMAX_DELAY); }
  ~Locked() { xSemaphoreGive(lock); }
};

// The session (agent task only).
struct Link {
  int fd = -1;
  bool wanted = false;     // open() was called and close() wasn't
  bool wifi = false;       // we hold a wlan::connect()
  bool tailnet = false;    // and a tailnet::hold(): agent_host is on the tailnet
  bool hello = false;      // the daemon answered ours
  bool reader = false;     // and it reads pages (`read`)
  bool sealed = false;     // the handshake is done: every frame is sealed both ways
  std::unique_ptr<noise::Initiator> handshake;  // between message 1 and message 2
  std::string refusal;     // why the daemon turned the session down, before hanging up
  noise::Cipher sendKey, recvKey;
  uint32_t cryptoUs = 0;     // spent sealing and opening since the last turn ended
  uint32_t cryptoBytes = 0;  // what that sealed and opened
  bool capturing = false;  // the mic is on
  bool draining = false;   // the mic is off; the utterance's tail is still in the ring
  bool turnOpen = false;   // listen_stop sent, reply_end not seen yet
  int staleEnds = 0;       // turns cut short whose reply_end is still to come: until then,
                           // what arrives belongs to them and is dropped
  bool stream = false;     // the output is open for this reply
  bool playingOut = false; // reply_end seen, the rest is still playing
  uint32_t talkStartMs = 0;
  uint32_t lastRxMs = 0;
  uint32_t lingerUntilMs = 0;  // when a connection nobody holds ends; 0: not ending
  bool heldIdle = false;       // let go of an Agent screen's connection after no key for a while
  std::vector<uint8_t> rx;
  size_t rxLen = 0;
  // The agent, on this connection.
  bool agentSupported = false;  // its hello had `agent`
  bool agentPin = false;        // it wants its PIN
  bool agentUnlocked = false;
  bool unlockSent = false;      // an unlock is waiting for its answer
  bool unlockByTicket = false;  // and it carries the ticket, not digits
  bool synced = false;          // sync sent
  std::string openSent;         // the session `open` was sent for ("": none, or `close`)
  std::string ticket;           // from the last right PIN, for later connections
  uint32_t ticketUntilMs = 0;
} s;

void setState(State next, const char *error = nullptr) {
  {
    Locked l;
    shared.state = next;
    if (error) shared.error = error;
  }
  state = next;
  generation++;
}

void post(Event e) {
  if (events) xQueueSend(events, &e, 0);
}

// Call with `lock` held.
void agentChanged() { agentShared.generation++; }

// The connection has ended: what the agent knew of it goes with it, the last list stays.
// readPage()'s answer: the page (error empty) or why not.
void finishRead(bool reached, const std::string &error, Page page = {}) {
  {
    Locked l;
    if (!readActive.load() || reading.done) return;
    reading.done = true;
    reading.reached = reached;
    reading.error = error;
    reading.page = std::move(page);
  }
  xSemaphoreGive(readDone);
}

void forgetAgentConnection() {
  s.agentSupported = s.agentPin = s.agentUnlocked = false;
  s.unlockSent = s.unlockByTicket = s.synced = false;
  s.openSent.clear();
  Locked l;
  outbox.clear();
  agentShared.connected = agentShared.synced = agentShared.unlocking = false;
  agentShared.unlocked = false;
  agentShared.draft = {};  // inkharness drops it with the connection
  agentChanged();
}

// --- Socket ----------------------------------------------------------------------------------

void endAudio() {
  if (s.capturing) audio::captureStop();
  if (s.stream) audio::streamAbort();
  s.capturing = s.draining = s.stream = s.playingOut = false;
}

// Answers for a session that has ended go nowhere.
void forgetAnswers() {
  Locked l;
  answers.clear();
}

void forgetKeys() {
  s.sealed = false;
  s.handshake.reset();
  s.sendKey.wipe();
  s.recvKey.wipe();
}

// The connection is gone (or never came up): back to no session, saying why.
void drop(const char *why) {
  if (s.fd >= 0) ::close(s.fd);
  s.fd = -1;
  s.hello = s.reader = s.turnOpen = false;
  finishRead(false, "CONNECTION LOST");
  s.staleEnds = 0;
  s.rxLen = 0;
  forgetKeys();
  endAudio();
  forgetAnswers();
  forgetAgentConnection();
  log_w("agent: %s", why);
  // A background connection (a poll, USB) that drops says nothing on the sheet.
  if (s.wanted || holds.load() > 0) {
    setState(State::Error, why);
  } else {
    setState(State::Off);
  }
}

bool sendAll(const uint8_t *data, size_t len) {
  while (len > 0) {
    const int n = ::send(s.fd, data, len, 0);
    if (n <= 0) {
      drop("CONNECTION LOST");
      return false;
    }
    data += n;
    len -= n;
  }
  return true;
}

// Seals a control or audio frame and sends it. `frame` holds the `len` bytes of payload at
// kSealedPayload, with noise::kTag bytes of room after them.
bool sendSealed(uint8_t *frame, uint8_t type, size_t len) {
  if (s.fd < 0 || !s.sealed) return false;
  const size_t sealedLen = 1 + len + noise::kTag;
  frame[0] = kSealed;
  frame[1] = sealedLen & 0xFF;
  frame[2] = sealedLen >> 8;
  frame[3] = type;
  const uint32_t t0 = micros();
  s.sendKey.seal(frame + kHeader, 1 + len);
  s.cryptoUs += micros() - t0;
  s.cryptoBytes += len;
  return sendAll(frame, kHeader + sealedLen);
}

bool sendControl(const std::string &json) {
  if (s.fd < 0) return false;
  if (json.size() > kMaxSealed) {
    log_w("agent: a %u-byte message is too long to send", json.size());
    return true;
  }
  std::vector<uint8_t> frame(kSealedPayload + json.size() + noise::kTag);
  memcpy(frame.data() + kSealedPayload, json.data(), json.size());
  return sendSealed(frame.data(), kControl, json.size());
}

// A JSON string literal, quotes included.
std::string quoted(const std::string &text) {
  std::string out = "\"";
  for (const char c : text) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (static_cast<unsigned char>(c) < 0x20) {
      char buf[8];
      snprintf(buf, sizeof buf, "\\u%04x", c);
      out += buf;
    } else {
      out += c;
    }
  }
  return out + "\"";
}

// A connected socket as the session wants it (blocking).
int configureSocket(int fd) {
  const int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
  timeval tv = {5, 0};  // a send stuck this long means the link is gone
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
  return fd;
}

int connectTo(uint32_t ip, uint16_t port, uint32_t timeoutMs) {
  const int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd < 0) return -1;
  sockaddr_in a = {};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = ip;
  a.sin_port = htons(port);
  const int flags = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  int rc = ::connect(fd, reinterpret_cast<sockaddr *>(&a), sizeof a);
  if (rc != 0 && errno == EINPROGRESS) {
    fd_set w;
    FD_ZERO(&w);
    FD_SET(fd, &w);
    timeval tv = {static_cast<time_t>(timeoutMs / 1000),
                  static_cast<suseconds_t>(timeoutMs % 1000 * 1000)};
    rc = -1;
    if (select(fd + 1, nullptr, &w, nullptr, &tv) == 1) {
      int err = 0;
      socklen_t len = sizeof err;
      getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
      rc = err == 0 ? 0 : -1;
    }
  }
  if (rc != 0) {
    ::close(fd);
    return -1;
  }
  fcntl(fd, F_SETFL, flags);
  return configureSocket(fd);
}

// --- Frames from the daemon ------------------------------------------------------------------

std::string field(const cJSON *json, const char *name) {
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(json, name);
  return cJSON_IsString(item) && item->valuestring ? item->valuestring : "";
}

int intField(const cJSON *json, const char *name, int fallback = -1) {
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(json, name);
  return cJSON_IsNumber(item) ? static_cast<int>(item->valuedouble) : fallback;
}

uint32_t timeField(const cJSON *json, const char *name) {
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(json, name);
  return cJSON_IsNumber(item) && item->valuedouble > 0 ? static_cast<uint32_t>(item->valuedouble)
                                                       : 0;
}

bool flag(const cJSON *json, const char *name) {
  return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, name));
}

// --- The agent's messages (docs/voice-protocol.md, "Agent sessions") --------------------------

// The daemon's hello: whether it has sessions, and whether this connection may see them.
void onAgentHello(const cJSON *hello) {
  const cJSON *a = cJSON_GetObjectItemCaseSensitive(hello, "agent");
  s.agentSupported = cJSON_IsObject(a);
  s.agentPin = s.agentSupported && flag(a, "pin");
  s.agentUnlocked = s.agentSupported && (!s.agentPin || flag(a, "unlocked"));
  Locked l;
  agentShared.known = true;
  agentShared.supported = s.agentSupported;
  agentShared.pin = s.agentPin;
  agentShared.unlocked = s.agentUnlocked;
  agentShared.connected = true;
  if (s.agentSupported) agentShared.place = field(a, "place");
  uint32_t features = 0;
  const cJSON *f;
  cJSON_ArrayForEach(f, cJSON_GetObjectItemCaseSensitive(a, "features")) {
    if (!cJSON_IsString(f) || !f->valuestring) continue;
    const std::string name = f->valuestring;
    if (name == "pause") features |= kPause;
    else if (name == "run") features |= kRun;
    else if (name == "jobs") features |= kJobs;
    else if (name == "schedule") features |= kSchedule;
    else if (name == "watch") features |= kWatch;
    else if (name == "deadline") features |= kDeadline;
    else if (name == "panel") features |= kPanel;
    else if (name == "speak") features |= kSpeak;
  }
  agentShared.features = features;
  agentChanged();
}

void readAsk(const cJSON *json, Ask &a) {
  a.session = field(json, "session");
  a.id = field(json, "id");
  a.title = field(json, "title");
  a.question = field(json, "question");
  a.detail = field(json, "detail");
  a.recommend = intField(json, "recommend");
  a.at = timeField(json, "at");
  const cJSON *deadline = cJSON_GetObjectItemCaseSensitive(json, "deadline");
  a.deadline = timeField(deadline, "at");
  a.then = field(deadline, "then");
  a.options.clear();
  const cJSON *option;
  cJSON_ArrayForEach(option, cJSON_GetObjectItemCaseSensitive(json, "options")) {
    a.options.push_back({field(option, "title"), field(option, "right"), field(option, "detail")});
  }
}

// `text` into a fixed field, cut at a character boundary.
template <size_t N>
void copyText(char (&out)[N], const std::string &text) {
  size_t n = std::min(text.size(), N - 1);
  while (n > 0 && n < text.size() && (static_cast<unsigned char>(text[n]) & 0xC0) == 0x80) --n;
  memcpy(out, text.data(), n);
  out[n] = '\0';
}

// The Agent face's summary of the list just read. Call with `lock` held.
void rebuildSummary() {
  Summary m = {};
  m.version = kSummaryVersion;
  m.locked = agentShared.locked;
  m.asks = agentShared.asks;
  m.running = agentShared.running;
  m.planned = agentShared.planned;
  m.proposed = agentShared.proposed;
  m.unread = agentShared.unread;
  m.checkAt = agentShared.checkAt;
  m.nextAt = agentShared.nextAt;
  m.latestAsk = agentShared.latestAsk;
  m.latestDone = agentShared.latestDone;
  m.content = !agentShared.locked && (!contentPolicy || contentPolicy());
  if (m.content) {
    if (agentShared.hasAsk) {
      const Ask &a = agentShared.ask;
      copyText(m.askTitle, a.title);
      copyText(m.askQuestion, a.question);
      m.optionCount = static_cast<uint8_t>(std::min<size_t>(a.options.size(), 4));
      for (int i = 0; i < m.optionCount; ++i) copyText(m.options[i], a.options[i].title);
      m.recommend = static_cast<int8_t>(a.recommend);
    }
    // What runs, then what watches, then what finished unseen.
    auto add = [&](const Row &r) {
      if (m.rowCount >= 5) return;
      SummaryRow &out = m.rows[m.rowCount++];
      copyText(out.title, r.title);
      copyText(out.state, r.state);
      out.step = static_cast<int8_t>(std::max(-1, std::min(r.step, 99)));
      out.steps = static_cast<int8_t>(std::max(-1, std::min(r.steps, 99)));
      out.at = r.state == "watch" ? r.nextAt : r.expectAt;
      const size_t n = std::min<size_t>(r.spark.size(), 16);
      if (n > 0) {
        const auto first = r.spark.end() - n;
        const float lo = *std::min_element(first, r.spark.end());
        const float hi = *std::max_element(first, r.spark.end());
        out.sparkN = static_cast<uint8_t>(n);
        out.sparkMin = lo;
        out.sparkMax = hi;
        out.sparkLast = r.spark.back();
        for (size_t i = 0; i < n; ++i) {
          const float v = *(first + i);
          out.spark[i] = hi > lo ? static_cast<uint8_t>((v - lo) / (hi - lo) * 255 + 0.5f) : 128;
        }
      }
    };
    for (const Row &r : agentShared.rows) if (r.state == "working") add(r);
    for (const Row &r : agentShared.rows) if (r.state == "watch") add(r);
    for (const Row &r : agentShared.rows) {
      if (r.unread && (r.state == "done" || r.state == "note" || r.state == "fail")) add(r);
    }
    for (const Row &r : agentShared.rows) {
      if (r.state == "scheduled" && r.nextAt != 0 && r.nextAt == agentShared.nextAt) {
        copyText(m.nextTitle, r.title);
        break;
      }
    }
  }
  summaryRam = m;
}

// Keeps the summary in NVS when it changed, and when the list came (at most once a minute).
void saveSummary() {
  Summary m;
  {
    Locked l;
    m = summaryRam;
  }
  const uint32_t checked = checkedAtUnix.load();
  const bool changed = memcmp(&m, &summarySaved, sizeof m) != 0;
  const bool stale = checked > checkedSavedUnix + 60;
  if (!changed && !stale) return;
  Preferences prefs;
  if (!prefs.begin(kNvs, false)) return;
  if (changed) {
    prefs.putBytes("sum", &m, sizeof m);
    summarySaved = m;
  }
  prefs.putUInt("chk", checked);
  checkedSavedUnix = checked;
  prefs.end();
}

void loadSummary() {
  Preferences prefs;
  if (!prefs.begin(kNvs, true)) return;
  Summary m = {};
  if (prefs.getBytesLength("sum") == sizeof m && prefs.getBytes("sum", &m, sizeof m) == sizeof m &&
      m.version == kSummaryVersion) {
    summaryRam = summarySaved = m;
  }
  checkedSavedUnix = prefs.getUInt("chk", 0);
  checkedAtUnix = checkedSavedUnix;
  prefs.end();
}

// The rows as the list draws them (`updated` isn't shown).
bool sameRows(const std::vector<Row> &a, const std::vector<Row> &b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    const Row &x = a[i], &y = b[i];
    if (x.id != y.id || x.title != y.title || x.state != y.state || x.place != y.place ||
        x.sub != y.sub || x.step != y.step || x.steps != y.steps || x.progress != y.progress ||
        x.unread != y.unread || x.herdr != y.herdr || x.program != y.program ||
        x.spark != y.spark || x.nextAt != y.nextAt) {
      return false;
    }
  }
  return true;
}

void onSessions(const cJSON *json) {
  std::vector<Row> rows;
  const cJSON *item;
  cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(json, "sessions")) {
    Row r;
    r.id = field(item, "id");
    r.title = field(item, "title");
    r.state = field(item, "state");
    r.place = field(item, "place");
    r.herdr = field(item, "via") == "herdr";
    r.program = field(item, "program");
    r.sub = field(item, "sub");
    r.step = intField(item, "step");
    r.steps = intField(item, "steps");
    r.progress = intField(item, "progress");
    r.updated = timeField(item, "updated");
    r.unread = flag(item, "unread");
    r.expectAt = timeField(item, "expect_at");
    r.nextAt = timeField(item, "next_at");
    const cJSON *v;
    cJSON_ArrayForEach(v, cJSON_GetObjectItemCaseSensitive(item, "spark")) {
      if (cJSON_IsNumber(v)) r.spark.push_back(static_cast<float>(v->valuedouble));
    }
    if (!r.id.empty()) rows.push_back(std::move(r));
  }
  const cJSON *counts = cJSON_GetObjectItemCaseSensitive(json, "counts");
  const bool locked = flag(json, "locked");
  const int asks = std::max(0, intField(counts, "ask", 0));
  const int running = std::max(0, intField(counts, "running", 0));
  const int idle = std::max(0, intField(counts, "idle", 0));
  const int planned = std::max(0, intField(counts, "planned", 0));
  const int unread = std::max(0, intField(counts, "unread", 0));
  const int proposed = std::max(0, intField(counts, "proposed", 0));
  const int older = std::max(0, intField(json, "older", 0));
  const cJSON *latest = cJSON_GetObjectItemCaseSensitive(json, "latest");
  const cJSON *askJson = cJSON_GetObjectItemCaseSensitive(json, "ask");
  Ask ask;
  const bool hasAsk = cJSON_IsObject(askJson);
  if (hasAsk) readAsk(askJson, ask);
  ++listCount;
  checkedAtUnix = static_cast<uint32_t>(time(nullptr));
  {
    Locked l;
    // The times change the board's plans, not what it shows: kept whatever else happens.
    agentShared.checkAt = timeField(json, "check_at");
    agentShared.nextAt = timeField(json, "next_at");
    agentShared.latestAsk = timeField(latest, "ask");
    agentShared.latestDone = timeField(latest, "done");
    agentShared.sayId = field(latest, "say");
    agentShared.noteId = field(latest, "note");
    agentShared.hasAsk = hasAsk;
    agentShared.ask = std::move(ask);
    // A working session sends a list about once a second (every log line is a change). Only
    // what the board shows counts as a change here, so the panel redraws only for that.
    const bool same = agentShared.synced && agentShared.locked == locked &&
                      agentShared.asks == asks && agentShared.running == running &&
                      agentShared.idle == idle && agentShared.planned == planned &&
                      agentShared.unread == unread && agentShared.proposed == proposed &&
                      agentShared.older == older && sameRows(agentShared.rows, rows);
    if (!same) {
      agentShared.synced = true;
      agentShared.locked = locked;
      agentShared.asks = asks;
      agentShared.running = running;
      agentShared.idle = idle;
      agentShared.planned = planned;
      agentShared.unread = unread;
      agentShared.proposed = proposed;
      agentShared.older = older;
      agentShared.rows.swap(rows);
      agentChanged();
    }
    rebuildSummary();
  }
  saveSummary();
}

// A number, or a number given as text ("712"): a model sends either.
float number(const cJSON *json, const char *name) {
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(json, name);
  if (cJSON_IsNumber(item)) return static_cast<float>(item->valuedouble);
  if (cJSON_IsString(item) && item->valuestring) return strtof(item->valuestring, nullptr);
  return 0;
}

// A field as text, whatever JSON it is: a value can come as a number ("value": 712).
std::string text(const cJSON *json, const char *name) {
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(json, name);
  if (cJSON_IsString(item) && item->valuestring) return item->valuestring;
  if (cJSON_IsNumber(item)) {
    char buf[24];
    snprintf(buf, sizeof buf, "%g", item->valuedouble);
    return buf;
  }
  return "";
}

void readPanel(const cJSON *json, Panel &p) {
  p.title = field(json, "title");
  p.at = timeField(json, "at");
  const cJSON *b;
  cJSON_ArrayForEach(b, cJSON_GetObjectItemCaseSensitive(json, "blocks")) {
    PanelBlock block;
    block.kind = field(b, "kind");
    block.id = field(b, "id");
    block.label = text(b, "label");
    block.right = text(b, "right");
    block.text = field(b, "markdown");
    block.question = field(b, "question");
    block.value = number(b, "value");
    block.max = number(b, "max");
    block.recommend = intField(b, "recommend");
    const cJSON *v;
    cJSON_ArrayForEach(v, cJSON_GetObjectItemCaseSensitive(b, "values")) {
      if (cJSON_IsNumber(v)) block.values.push_back(static_cast<float>(v->valuedouble));
    }
    const bool choice = block.kind == "choice";
    cJSON_ArrayForEach(v, cJSON_GetObjectItemCaseSensitive(b, choice ? "options" : "items")) {
      PanelItem item;
      if (block.kind == "stat") {
        item = {text(v, "label"), text(v, "value"), "", ""};
      } else if (block.kind == "kv") {
        item = {text(v, "key"), text(v, "value"), "", ""};
      } else if (cJSON_IsString(v) && v->valuestring) {
        item.a = v->valuestring;
      } else {
        item = {text(v, "title"), text(v, "sub"), text(v, "right"), text(v, "tag")};
      }
      block.items.push_back(std::move(item));
    }
    p.blocks.push_back(std::move(block));
  }
}

void onSession(const cJSON *json) {
  Session a;
  a.id = field(json, "id");
  a.title = field(json, "title");
  a.state = field(json, "state");
  a.place = field(json, "place");
  a.herdr = field(json, "via") == "herdr";
  a.program = field(json, "program");
  a.backend = field(json, "backend");
  a.prompt = field(json, "prompt");
  a.result = field(json, "result");
  a.started = timeField(json, "started");
  a.updated = timeField(json, "updated");
  a.step = intField(json, "step");
  a.steps = intField(json, "steps");
  const cJSON *item;
  cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(json, "plan")) {
    const std::string state = field(item, "state");
    a.plan.push_back({field(item, "text"), state == "done" ? 'd' : state == "now" ? 'n' : 't'});
  }
  cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(json, "log")) {
    a.log.push_back({timeField(item, "at"), field(item, "text")});
  }
  cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(json, "screen")) {
    if (cJSON_IsString(item) && item->valuestring) a.screen.push_back(item->valuestring);
  }
  const cJSON *ask = cJSON_GetObjectItemCaseSensitive(json, "ask");
  a.asking = cJSON_IsObject(ask);
  if (a.asking) {
    readAsk(ask, a.ask);
    if (a.ask.session.empty()) a.ask.session = a.id;
  }
  a.expectAt = timeField(json, "expect_at");
  const cJSON *cost = cJSON_GetObjectItemCaseSensitive(json, "cost");
  const cJSON *usd = cJSON_GetObjectItemCaseSensitive(cost, "usd");
  if (cJSON_IsNumber(usd)) {
    a.cost = static_cast<float>(usd->valuedouble);
    a.costNotional = flag(cost, "notional");
  }
  const cJSON *sched = cJSON_GetObjectItemCaseSensitive(json, "schedule");
  if (cJSON_IsObject(sched)) {
    ScheduleView &v = a.schedule;
    v.present = true;
    v.at = field(sched, "at");
    v.days = field(sched, "days");
    v.prompt = field(sched, "prompt");
    v.next = timeField(sched, "next");
    v.wake = flag(sched, "wake_board");
    v.note = flag(sched, "show_note");
    v.speak = flag(sched, "speak");
    cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(sched, "runs")) {
      v.runs.push_back({timeField(item, "at"), field(item, "state"),
                        static_cast<uint32_t>(std::max(0, intField(item, "secs", 0)))});
    }
  }
  const cJSON *watch = cJSON_GetObjectItemCaseSensitive(json, "watch");
  if (cJSON_IsObject(watch)) {
    WatchView &v = a.watch;
    v.present = true;
    v.condition = field(watch, "condition");
    v.every = std::max(0, intField(watch, "every_minutes", 0));
    v.next = timeField(watch, "next");
    v.met = flag(watch, "met");
    cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(watch, "values")) {
      if (cJSON_IsNumber(item)) v.values.push_back(static_cast<float>(item->valuedouble));
    }
  }
  const cJSON *panel = cJSON_GetObjectItemCaseSensitive(json, "panel");
  if (cJSON_IsObject(panel)) {
    a.hasPanel = true;
    readPanel(panel, a.panel);
  }
  Locked l;
  if (a.id != wantOpen) return;  // one asked for before another open: not wanted now
  agentShared.open = std::move(a);
  agentShared.hasOpen = true;
  agentChanged();
}

void onUnlocked(const cJSON *json) {
  const bool byTicket = s.unlockByTicket;
  s.unlockSent = s.unlockByTicket = false;
  const bool ok = flag(json, "ok");
  if (ok) {
    s.agentUnlocked = true;
    const std::string ticket = field(json, "ticket");
    if (!ticket.empty()) {
      s.ticket = ticket;
      s.ticketUntilMs = millis() + static_cast<uint32_t>(std::max(0, intField(json, "ttl_s", 0))) * 1000;
    }
    s.synced = false;  // again, for the unlocked list
    s.openSent.clear();
  } else if (byTicket) {
    s.ticket.clear();  // run out on the daemon's side; the digits are needed again
  }
  const int wait = intField(json, "wait_s", 0);
  Locked l;
  pendingPin.clear();
  agentShared.unlocking = false;
  agentShared.unlocked = ok;
  if (ok) {
    agentShared.unlockNote.clear();
  } else if (!byTicket) {
    agentShared.unlockNote =
        wait > 0 ? "WRONG PIN: WAIT " + std::to_string(wait < 60 ? wait : (wait + 59) / 60) +
                       (wait < 60 ? " S" : " MIN")
                 : "WRONG PIN";
  }
  agentChanged();
}

void onRefused(const cJSON *json) {
  const std::string message = field(json, "message");
  log_w("agent: inkharness refused %s: %s", field(json, "request").c_str(), message.c_str());
  if (message == "locked") s.agentUnlocked = false;
  Locked l;
  agentShared.refusal = message;
  agentShared.refusalSession = field(json, "session");
  agentShared.refusalRequest = field(json, "request");
  agentShared.refusals++;
  if (message == "locked") agentShared.unlocked = false;
  agentChanged();
}

void onControl(const uint8_t *payload, size_t len) {
  cJSON *json = cJSON_ParseWithLength(reinterpret_cast<const char *>(payload), len);
  if (!json) return;
  const std::string type = field(json, "type");
  if (type == "hello") {
    s.hello = true;
    s.reader = flag(json, "reader");
  } else if (type == "page_image") {
    // One of the pictures, before its page: kept with the request until the page comes.
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(json, "id");
    const int w = intField(json, "width"), h = intField(json, "height");
    const std::string b64 = field(json, "bits");
    PagePicture pic;
    pic.index = static_cast<uint32_t>(std::max(0, intField(json, "index")));
    const size_t want = w > 0 && h > 0 && w <= 2048 && h <= 2048
                            ? static_cast<size_t>((w + 7) / 8) * h
                            : 0;
    size_t got = 0;
    if (want) {
      pic.width = static_cast<uint16_t>(w);
      pic.height = static_cast<uint16_t>(h);
      pic.bits.resize(want);
      if (mbedtls_base64_decode(pic.bits.data(), want, &got,
                                reinterpret_cast<const unsigned char *>(b64.data()),
                                b64.size()) != 0) {
        got = 0;
      }
    }
    Locked l;
    if (got == want && want && cJSON_IsNumber(id) && readActive.load() && !reading.done &&
        static_cast<uint32_t>(id->valuedouble) == reading.id) {
      reading.page.pictures.push_back(std::move(pic));
    }
  } else if (type == "page") {
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(json, "id");
    bool mine = false;
    {
      Locked l;
      mine = cJSON_IsNumber(id) && static_cast<uint32_t>(id->valuedouble) == reading.id;
    }
    if (mine) {
      const std::string error = field(json, "error");
      Page page;
      {
        Locked l;
        page.pictures = std::move(reading.page.pictures);
      }
      if (error.empty()) {
        page.title = field(json, "title");
        page.byline = field(json, "byline");
        page.site = field(json, "site");
        page.published = timeField(json, "published");
        page.markdown = field(json, "markdown");
      }
      finishRead(true, error, std::move(page));
    }
  } else if (type == "sessions") {
    onSessions(json);
  } else if (type == "session") {
    onSession(json);
  } else if (type == "ask") {
    // A new question (or one asked again): the ask sheet takes it from here. The list that
    // follows carries it too.
    Ask ask;
    readAsk(json, ask);
    Locked l;
    if (ask.at >= agentShared.ask.at || !agentShared.hasAsk) {
      agentShared.ask = std::move(ask);
      agentShared.hasAsk = true;
      agentShared.latestAsk = std::max(agentShared.latestAsk, agentShared.ask.at);
      agentChanged();
    }
  } else if (type == "started") {
    Locked l;
    agentShared.started = field(json, "session");
    agentShared.startedTurn = turns.load();
    agentChanged();
  } else if (type == "draft") {
    Locked l;
    agentShared.draft = {field(json, "session"), field(json, "id"), field(json, "text"),
                         turns.load()};
    agentChanged();
  } else if (type == "unlocked") {
    onUnlocked(json);
  } else if (type == "refused") {
    onRefused(json);
  } else if (type == "stt") {
    if (s.staleEnds == 0) {
      Locked l;
      shared.heard = field(json, "text");
      generation++;
    }
  } else if (type == "text") {
    if (s.staleEnds == 0) {
      Locked l;
      if (shared.reply.size() < kMaxText) shared.reply += field(json, "delta");
      generation++;
    }
  } else if (type == "reply_start") {
    const cJSON *rate = cJSON_GetObjectItemCaseSensitive(json, "rate");
    const int hz = cJSON_IsNumber(rate) ? rate->valueint : 0;
    if (s.staleEnds == 0 && s.turnOpen && hz >= 8000 && hz <= 48000) {
      s.stream = audio::streamStart(static_cast<uint32_t>(hz));
      if (s.stream) setState(State::Speaking);
    }
  } else if (type == "reply_end") {
    log_i("agent: sealing and opening %u KB took %u ms", s.cryptoBytes / 1024,
          s.cryptoUs / 1000);
    s.cryptoUs = s.cryptoBytes = 0;
    if (s.staleEnds > 0) {
      --s.staleEnds;  // the end of a turn cut short; a new utterance is already under way
    } else if (s.turnOpen) {
      s.turnOpen = false;
      if (s.stream) {
        audio::streamEnd();
        s.playingOut = true;
      } else {
        setState(State::Ready);
      }
    }
  } else if (type == "error") {
    if (s.staleEnds == 0) {
      Locked l;
      shared.error = field(json, "message");
      generation++;
    }
  } else if (type == "tool_call") {
    // From a turn cut short: inkharness has forgotten it, so it isn't run.
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(json, "id");
    if (s.staleEnds == 0 && s.turnOpen && cJSON_IsNumber(id)) {
      const uint32_t callId = static_cast<uint32_t>(id->valuedouble);
      if (!toolCall) {
        toolResult(callId, "this board runs no tools", true);
      } else {
        cJSON *input = cJSON_DetachItemFromObjectCaseSensitive(json, "input");
        toolCall(callId, field(json, "name"), input, turns.load());
      }
    }
  }
  cJSON_Delete(json);
}

// Sends the answers the tools have given since the last call.
void sendAnswers() {
  std::vector<Answer> ready;
  {
    Locked l;
    ready.swap(answers);
  }
  for (const Answer &a : ready) {
    const std::string json = "{\"type\":\"tool_result\",\"id\":" + std::to_string(a.id) +
                             ",\"text\":" + quoted(a.text) +
                             ",\"error\":" + (a.error ? "true" : "false") + "}";
    if (!sendControl(json)) return;
  }
}

void onAudio(const uint8_t *pcm, size_t len) {
  if (s.staleEnds > 0 || !s.stream) return;
  size_t off = 0;
  while (off < len) {
    // The ring is full while the speaker catches up: this is what holds the daemon back.
    // A key (talk again, leave the screen) ends the wait; the rest of the reply is dropped
    // with the stream anyway.
    if (uxQueueMessagesWaiting(events) > 0) return;
    const size_t n = audio::streamWrite(pcm + off, len - off, 50);
    if (n == 0 && !audio::streaming()) return;
    off += n;
  }
}

// Message 2: the daemon's answer to the hello (its own, or why not), and the keys for the
// rest of the session.
void onHandshake(const uint8_t *msg, size_t len) {
  if (!s.handshake) return;  // not waiting for one
  std::string answer;
  const bool ok = s.handshake->finish(msg, len, answer, s.sendKey, s.recvKey);
  s.handshake.reset();
  if (!ok) {  // the daemon had the token (it opened message 1), so this didn't come from it
    drop("HANDSHAKE FAILED");
    return;
  }
  s.sealed = true;
  cJSON *json = cJSON_ParseWithLength(answer.data(), answer.size());
  const std::string type = field(json, "type");
  if (type == "hello") {
    s.hello = true;
    s.reader = flag(json, "reader");
    s.refusal.clear();
    onAgentHello(json);
  } else if (type == "error") {
    s.refusal = field(json, "message");
  }
  cJSON_Delete(json);
}

// Unencrypted, before the handshake: the daemon turning the session down (it couldn't open
// message 1: the tokens differ), just before it hangs up. Only shown.
void onRefusal(const uint8_t *payload, size_t len) {
  cJSON *json = cJSON_ParseWithLength(reinterpret_cast<const char *>(payload), len);
  if (field(json, "type") == "error") s.refusal = field(json, "message");
  cJSON_Delete(json);
}

// Opens a sealed frame in place and handles the frame inside. False if it doesn't open: it
// was changed, replayed or reordered on the way, and the session can't go on.
bool openSealed(uint8_t *payload, size_t len) {
  if (len < 1 + noise::kTag) return false;
  const uint32_t t0 = micros();
  const bool ok = s.recvKey.open(payload, len);
  s.cryptoUs += micros() - t0;
  if (!ok) return false;
  const size_t n = len - 1 - noise::kTag;
  s.cryptoBytes += n;
  if (payload[0] == kControl) {
    onControl(payload + 1, n);
  } else if (payload[0] == kAudio) {
    onAudio(payload + 1, n);
  }  // any other type: a later protocol version's, skipped
  return true;
}

// The board's way of saying why the daemon refused it.
std::string refusalText(const std::string &message) {
  return message == "wrong token" ? "WRONG AGENT TOKEN" : "INKHARNESS: " + message;
}

// Reads what the socket has (waiting up to timeoutMs for it) and handles the whole frames.
// True if anything arrived.
bool pump(uint32_t timeoutMs) {
  fd_set r;
  FD_ZERO(&r);
  FD_SET(s.fd, &r);
  timeval tv = {static_cast<time_t>(timeoutMs / 1000),
                static_cast<suseconds_t>(timeoutMs % 1000 * 1000)};
  if (select(s.fd + 1, &r, nullptr, nullptr, &tv) != 1) return false;
  const int n = ::recv(s.fd, s.rx.data() + s.rxLen, s.rx.size() - s.rxLen, 0);
  if (n <= 0) {
    drop("DISCONNECTED");
    return false;
  }
  s.rxLen += n;
  s.lastRxMs = millis();
  size_t pos = 0;
  while (s.fd >= 0 && s.rxLen - pos >= kHeader) {
    const size_t len = s.rx[pos + 1] | s.rx[pos + 2] << 8;
    if (s.rxLen - pos < kHeader + len) break;
    const uint8_t type = s.rx[pos];
    uint8_t *payload = s.rx.data() + pos + kHeader;
    if (s.sealed) {  // nothing else is taken once the handshake is done
      if (type != kSealed || !openSealed(payload, len)) {
        drop("BAD FRAME FROM INKHARNESS");
        break;
      }
    } else if (type == kHandshake) {
      onHandshake(payload, len);
    } else if (type == kControl) {
      onRefusal(payload, len);
    }  // any other type: a later protocol version's, skipped
    pos += kHeader + len;
  }
  if (s.fd < 0) return false;  // dropped while handling
  memmove(s.rx.data(), s.rx.data() + pos, s.rxLen - pos);
  s.rxLen -= pos;
  return true;
}

// --- Session ---------------------------------------------------------------------------------

// listen_start for the utterance beginning now: a talk turn, or the agent's (setTarget()).
std::string listenStart() {
  const Target to = targetSource ? targetSource() : Target{};
  {
    Locked l;
    shared.agentTurn = to.agent;
    shared.turnSession = to.agent ? to.session : std::string();
  }
  if (!to.agent) return "{\"type\":\"listen_start\"}";
  std::string json = "{\"type\":\"listen_start\",\"mode\":\"agent\"";
  if (!to.session.empty()) json += ",\"session\":" + quoted(to.session);
  return json + "}";
}

// What the Agent's screens want of the connection, sent once it's up: the PIN or the ticket,
// the list, the open session, and their requests.
void agentPump() {
  if (s.fd < 0 || !s.hello) return;
  std::string pin, open;
  std::vector<std::string> requests;
  {
    Locked l;
    if (!s.agentSupported || !s.agentPin || s.agentUnlocked) pendingPin.clear();  // not needed
    pin = pendingPin;
    open = wantOpen;
    requests.swap(outbox);
  }
  if (!s.agentSupported) return;
  if (s.agentPin && !s.agentUnlocked && !s.unlockSent) {
    std::string json;
    if (!pin.empty()) {
      json = "{\"type\":\"unlock\",\"pin\":" + quoted(pin) + "}";
      s.unlockByTicket = false;
    } else if (!s.ticket.empty() && static_cast<int32_t>(s.ticketUntilMs - millis()) > 0) {
      json = "{\"type\":\"unlock\",\"ticket\":" + quoted(s.ticket) + "}";
      s.unlockByTicket = true;
    }
    if (!json.empty()) {
      if (!sendControl(json)) return;
      s.unlockSent = true;
      Locked l;
      agentShared.unlocking = true;
      agentChanged();
    }
  }
  // Every connection follows the list: the sheet's, a poll's, the Agent's. It keeps Home, the
  // status bar and the face current.
  if (!s.synced) {
    if (!sendControl("{\"type\":\"sync\"}")) return;
    s.synced = true;
  }
  if (open != s.openSent && s.agentUnlocked) {
    const std::string json = open.empty()
                                 ? std::string("{\"type\":\"close\"}")
                                 : "{\"type\":\"open\",\"session\":" + quoted(open) + "}";
    if (!sendControl(json)) return;
    s.openSent = open;
  }
  for (const std::string &json : requests) {
    if (!sendControl(json)) return;
  }
}

void closeSession();

bool heldActive() {
  return holds.load() > 0 && (usbLive || input::idleMs() < kIdleLiveMs);
}

// Whether the connection should be up: an Agent screen in use, the sheet, USB power, a poll.
bool wantLink() {
  return heldActive() || s.wanted || usbLive || pollActive.load() || readActive.load();
}

// Sends readPage()'s request once the daemon has said hello, or ends it if it can't read.
void readPump() {
  if (s.fd < 0 || !s.hello || !readActive.load()) return;
  std::string json;
  {
    Locked l;
    if (reading.sent || reading.done) return;
    if (s.reader) {
      json = "{\"type\":\"read\",\"id\":" + std::to_string(reading.id) +
             ",\"url\":" + quoted(reading.url);
      if (reading.pictureW && reading.pictureH) {
        json += ",\"image_width\":" + std::to_string(reading.pictureW) +
                ",\"image_height\":" + std::to_string(reading.pictureH);
      }
      json += "}";
      reading.sent = true;
    }
  }
  if (json.empty()) {
    finishRead(false, "INKHARNESS CAN'T READ PAGES");
    return;
  }
  sendControl(json);
}

// Ends the connection once nobody holds it, after kLingerMs.
void lingerOrClose() {
  if (wantLink()) {
    s.lingerUntilMs = 0;
    return;
  }
  if (s.fd < 0) {
    closeSession();  // lets Wi-Fi go after a failed connect too
    return;
  }
  if (s.lingerUntilMs == 0) s.lingerUntilMs = millis() + kLingerMs;
}

// Presses made while a connect was blocking this task are stale by now: acting on them would
// open the mic with the key already up, or start another connect straight after a failed one.
// Open and Close stay queued.
void dropTalkEvents() {
  Event kept[8];
  int n = 0;
  Event e;
  while (xQueueReceive(events, &e, 0) == pdTRUE) {
    if (e != Event::TalkStart && e != Event::TalkStop && n < 8) kept[n++] = e;
  }
  for (int i = 0; i < n; ++i) xQueueSend(events, &kept[i], 0);
}

// Time left until `deadline` (millis), at most `cap`.
uint32_t left(uint32_t deadline, uint32_t cap) {
  const int32_t d = static_cast<int32_t>(deadline - millis());
  return d <= 0 ? 0 : std::min<uint32_t>(static_cast<uint32_t>(d), cap);
}

// Joins Wi-Fi (and the tailnet if the host is there), connects and says hello, all before
// `deadline` (millis). A quiet connect (a poll, the live connection on USB, a check-in) shows
// nothing: it leaves the state and the sheet's texts alone, and gives up between steps as soon
// as a talk press is waiting, which then connects as usual (the mic first). True if connected.
bool connectLink(bool quiet, uint32_t deadline) {
  auto fail = [&](const char *why) {
    if (quiet) {
      log_i("agent: background connect: %s", why);
    } else {
      setState(State::Error, why);
    }
    return false;
  };
  auto yields = [&] { return quiet && talkWanted.load(); };
  if (!quiet) {
    {
      Locked l;
      shared.error.clear();
      shared.heard.clear();
      shared.reply.clear();
      answers.clear();  // late ones, for a session that has gone
    }
    setState(State::Connecting);
  }
  if (!settings::hasAgent()) return fail("NO AGENT HOST SET");
  if (!s.wifi) {
    std::string error;
    const uint32_t ms = left(deadline, 15000);
    if (ms == 0 || !wlan::connect(ms, error)) return fail(ms == 0 ? "NO TIME LEFT" : error.c_str());
    s.wifi = true;
    // Modem sleep wakes the radio only at the access point's beacons: every frame would wait
    // for one. Off for the session (shares.cpp and ota.cpp do the same).
    WiFi.setSleep(false);
  }
  if (yields()) return false;
  const std::string host = settings::agentHost();
  const uint16_t port = static_cast<uint16_t>(settings::agentPort());
  // A agent host on the tailnet is often on this LAN too (at home): its LAN addresses first, as
  // the tailnet last gave them, which needs no join. The token vouches for whoever answers.
  if (!s.tailnet && tailnet::routes(host)) {
    const int fd = tailnet::connectLan(host, port, 1000);
    if (fd >= 0) s.fd = configureSocket(fd);
  }
  if (s.fd < 0) {
    if (!s.tailnet && tailnet::routes(host)) {
      if (yields()) return false;
      std::string error;
      const uint32_t ms = left(deadline, 20000);
      if (ms == 0 || !tailnet::hold(ms, error)) {
        return fail(ms == 0 ? "NO TIME LEFT" : error.c_str());
      }
      s.tailnet = true;
    }
    // The WireGuard session with the daemon's machine first: a connect before it would lose its
    // SYN and wait a second or more for TCP to send it again.
    if (s.tailnet) tailnet::reach(host, left(deadline, 8000));
    if (yields()) return false;
    addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *res = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) {  // tailnet names too
      return fail(s.tailnet ? "AGENT HOST NOT ON THE TAILNET" : "AGENT HOST NOT FOUND");
    }
    const uint32_t ip = reinterpret_cast<const sockaddr_in *>(res->ai_addr)->sin_addr.s_addr;
    freeaddrinfo(res);
    const uint32_t ms = left(deadline, 4000);
    s.fd = ms > 0 ? connectTo(ip, port, ms) : -1;
    if (s.fd < 0) return fail("INKHARNESS ISN'T ANSWERING");
  }
  if (s.rx.empty()) s.rx.resize(kRxBytes);
  s.rxLen = 0;
  s.hello = false;
  s.refusal.clear();
  forgetKeys();
  // The hello rides in the handshake's first message, sealed with a key made from the token;
  // the token itself stays here. The daemon's answer comes back in the second.
  std::string hello = "{\"type\":\"hello\",\"proto\":" + std::to_string(kProto) +
                      ",\"device\":\"inkdeck\",\"mic_rate\":" +
                      std::to_string(audio::kRecordRate);
  if (toolDefinitions) {
    hello += ",\"tools\":";
    hello += toolDefinitions;
  }
  hello += "}";
  const uint32_t t0 = millis();
  s.handshake.reset(new noise::Initiator(settings::agentToken()));
  const std::vector<uint8_t> msg1 = s.handshake->hello(hello);
  const uint32_t keyMs = millis() - t0;
  if (msg1.empty() || msg1.size() > 65535) {
    drop(msg1.empty() ? "NO AGENT KEY (MBEDTLS)" : "TOO MANY BOARD TOOLS");
    return false;
  }
  uint8_t header[kHeader] = {kHandshake, static_cast<uint8_t>(msg1.size() & 0xFF),
                             static_cast<uint8_t>(msg1.size() >> 8)};
  if (!sendAll(header, kHeader) || !sendAll(msg1.data(), msg1.size())) return false;
  while (s.fd >= 0 && !s.hello && millis() - t0 < 4000 && left(deadline, 1) > 0) pump(100);
  s.handshake.reset();
  if (s.fd < 0) {  // hung up on us: after saying why, unless it was something else
    if (!s.refusal.empty()) fail(refusalText(s.refusal).c_str());
    return false;
  }
  if (!s.hello) {
    if (!s.refusal.empty()) {
      drop(refusalText(s.refusal).c_str());
    } else {
      // A daemon from before encryption skips the handshake and waits for a plain hello.
      drop(s.sealed ? "INKHARNESS DIDN'T SAY HELLO" : "NO HANDSHAKE (OLD INKHARNESS?)");
    }
    return false;
  }
  log_i("agent: connected to %s:%d%s, encrypted (handshake %u ms, %u of them for keys)%s",
        host.c_str(), settings::agentPort(),
        s.tailnet ? " through the tailnet" : tailnet::routes(host) ? " on the LAN" : "",
        millis() - t0, keyMs,
        s.agentSupported ? (s.agentUnlocked ? ", sessions" : ", sessions behind its PIN") : "");
  setState(State::Ready);
  return true;
}

// A connect for someone waiting on it (the sheet, an Agent screen): it shows how it goes.
void connectSession() { connectLink(false, millis() + 45000); }

void closeSession() {
  if (s.fd >= 0) ::close(s.fd);
  s.fd = -1;
  s.hello = s.reader = s.turnOpen = false;
  finishRead(false, "NO CONNECTION TO INKHARNESS");
  s.staleEnds = 0;
  s.lingerUntilMs = 0;
  forgetKeys();
  endAudio();
  forgetAnswers();
  forgetAgentConnection();
  if (s.tailnet) {
    tailnet::release();
    s.tailnet = false;
  }
  if (s.wifi) {
    WiFi.setSleep(true);
    wlan::disconnect();
    s.wifi = false;
  }
  setState(State::Off);
}

// No session yet (the agent sheet's first hold) or a failed one: this press opens one and is
// heard. The mic goes on first and the capture ring keeps what's said while Wi-Fi joins and
// inkharness answers; sendMic() catches up once it has. A release while connecting is still queued
// and ends the utterance as usual.
void talkIntoNewSession() {
  s.wanted = true;
  if (!configured()) {  // nothing to connect to: say why, and leave the mic off
    setState(State::Error, settings::hasAgent() ? "NO WI-FI NETWORK SET" : "NO AGENT HOST SET");
    return;
  }
  s.lingerUntilMs = 0;
  const bool mic = audio::captureStart(true);
  if (mic) {
    s.capturing = true;
    s.talkStartMs = millis();
  }
  turns++;
  connectSession();  // blocks; key events wait in the queue
  if (s.fd < 0) {
    if (s.capturing) audio::captureStop();
    s.capturing = false;
    dropTalkEvents();  // a retry is the next press, not one made while this one failed
    return;
  }
  if (!mic) {
    dropTalkEvents();
    setState(State::Ready, "THE MIC WON'T START");
    return;
  }
  agentPump();  // the PIN or ticket first, so an agent turn finds the connection unlocked
  if (s.fd < 0 || !sendControl(listenStart())) return;
  setState(State::Listening);
}

void beginTalk() {
  if (s.fd < 0) {
    talkIntoNewSession();
    return;
  }
  if (!s.hello || s.capturing) return;
  s.wanted = true;  // the sheet this hold raises holds it now, until its close()
  s.lingerUntilMs = 0;
  turns++;  // whatever happens next, the turn before is over: its screen change is dropped
  // A reply still coming or playing is cut short. The daemon ends that turn when it sees
  // listen_start (or has just ended it itself): either way one reply_end is still to come,
  // and everything before it belongs to the old turn.
  if (s.turnOpen) {
    s.turnOpen = false;
    ++s.staleEnds;
  }
  if (s.stream) audio::streamAbort();
  s.stream = s.playingOut = s.draining = false;
  {
    Locked l;
    shared.heard.clear();
    shared.reply.clear();
    shared.error.clear();
  }
  if (!audio::captureStart(true)) {
    setState(State::Ready, "THE MIC WON'T START");
    return;
  }
  s.capturing = true;
  s.talkStartMs = millis();
  if (!sendControl(listenStart())) return;
  setState(State::Listening);
}

void endTalk() {
  if (!s.capturing) return;
  audio::captureStop();
  s.capturing = false;
  s.draining = true;  // sendMic() sends the tail, then listen_stop
}

// Moves mic audio from the capture ring to the daemon. True if it sent any.
bool sendMic() {
  uint8_t frame[kSealedPayload + kMicFrame + noise::kTag];
  const size_t n = audio::captureRead(frame + kSealedPayload, kMicFrame, s.capturing ? 20 : 0);
  if (n > 0) return sendSealed(frame, kAudio, n);
  if (s.draining) {
    s.draining = false;
    if (!sendControl("{\"type\":\"listen_stop\"}")) return false;
    s.turnOpen = true;
    s.lastRxMs = millis();
    setState(State::Thinking);
  }
  return false;
}

// --- Check-ins ---------------------------------------------------------------------------------

void refreshUsb() {
  if (usbCheckedMs != 0 && millis() - usbCheckedMs < kUsbCheckMs) return;
  usbCheckedMs = millis() | 1;
  const pmic::Status p = pmic::status();
  const bool usb = p.ok && p.vbus && configured();
  if (usb == usbLive) return;
  usbLive = usb;
  liveFailures = 0;
  liveRetryMs = millis();
  log_i("agent: %s", usb ? "on USB: the connection stays up" : "off USB: polls instead");
}

// What the list says that a poll's ladder watches: when it changes, polls come every 5 minutes.
uint32_t signature() {
  Locked l;
  const Sessions &a = agentShared;
  uint32_t h = 2166136261u;
  for (const uint32_t v : {static_cast<uint32_t>(a.asks), static_cast<uint32_t>(a.running),
                           static_cast<uint32_t>(a.planned), static_cast<uint32_t>(a.proposed),
                           static_cast<uint32_t>(a.unread), a.latestAsk, a.latestDone,
                           static_cast<uint32_t>(a.rows.size())}) {
    h = (h ^ v) * 16777619u;
  }
  return h;
}

// When to poll next, after a poll (or a live connection) that got a list (ok) or didn't: the
// ladder, failures backing off, sooner at check_at, never on a low battery.
void planPoll(bool ok) {
  bool supported, known;
  uint32_t checkAt;
  {
    Locked l;
    supported = agentShared.supported;
    known = agentShared.known;
    checkAt = agentShared.checkAt;
  }
  uint32_t gap;
  if (!ok) {
    pollFailures = std::min<uint8_t>(pollFailures + 1, 4);
    gap = std::min<uint32_t>(kMaxPollGapMs, kLadderMin[0] * 60000u << (pollFailures - 1));
  } else {
    pollFailures = 0;
    if (known && !supported) {  // no sessions on this daemon: nothing to poll for
      nextPollMs = 0;
      nextCheckUnix = 0;
      return;
    }
    const uint32_t sig = signature();
    if (sig != lastSignature) {
      ladder = 0;
    } else if (ladder < 2) {
      ++ladder;
    }
    lastSignature = sig;
    gap = kLadderMin[ladder] * 60000u;
  }
  const uint32_t now = static_cast<uint32_t>(time(nullptr));
  if (checkAt > now) gap = std::min<uint32_t>(gap, std::max(kFloorS, checkAt - now) * 1000u);
  const pmic::Status p = pmic::status();
  if (p.ok && !p.vbus && p.percent >= 0 && p.percent < power::kLowBatteryPercent) {
    nextPollMs = 0;  // only when asked (checkSoon)
    nextCheckUnix = 0;
    return;
  }
  nextPollMs = (millis() + gap) | 1;
  nextCheckUnix = now + gap / 1000;
}

bool pollIsDue() {
  const uint32_t at = nextPollMs.load();
  return at != 0 && static_cast<int32_t>(millis() - at) >= 0;
}

void startPoll() {
  nextPollMs = 0;
  lastPollMs = millis() | 1;
  pollActive = true;
  pollStartMs = millis();
  pollLists = listCount;
  if (!connectLink(true, millis() + kPollDeadlineMs)) {
    pollActive = false;
    closeSession();  // lets Wi-Fi go
    planPoll(false);
  }
}

// With the connection up: a poll ends once its list has come (or it waited long enough).
void pollDuties() {
  if (!pollActive.load()) return;
  if (listCount != pollLists) {
    pollActive = false;
    planPoll(true);
  } else if (millis() - pollStartMs > kPollDeadlineMs + kPollListMs) {
    pollActive = false;
    planPoll(false);
  }
}

// With no connection: a key back on an Agent screen, USB, or a poll that's due.
void idleDuties() {
  refreshUsb();
  if (!configured()) return;
  const uint32_t now = millis();
  if (s.heldIdle && heldActive()) {
    s.heldIdle = false;
    connectSession();
    return;
  }
  if (usbLive && static_cast<int32_t>(now - liveRetryMs) >= 0) {
    if (connectLink(true, now + kPollDeadlineMs)) {
      liveFailures = 0;
    } else {
      closeSession();
      liveFailures = std::min<uint8_t>(liveFailures + 1, 2);
      liveRetryMs = millis() + (liveFailures == 1 ? 60000 : 300000);
    }
    return;
  }
  if (soon.exchange(false) && (lastPollMs == 0 || now - lastPollMs >= kSoonGapMs)) {
    nextPollMs = now | 1;
  }
  if (pollIsDue()) startPoll();
}

// How long the idle task may sleep: until the next poll, and at least every kUsbCheckMs.
uint32_t idleWaitMs() {
  uint32_t wait = kUsbCheckMs;
  const uint32_t at = nextPollMs.load();
  if (at != 0) {
    const int32_t d = static_cast<int32_t>(at - millis());
    wait = std::min<uint32_t>(wait, d > 0 ? static_cast<uint32_t>(d) : 0);
  }
  return std::max<uint32_t>(wait, 10);
}

// From setup() on an RTC wake: connect, wait for the list, let go.
void checkIn() {
  const uint32_t deadline = millis() + checkInMs;
  checkInOk = false;
  const uint32_t lists = listCount;
  if (s.fd < 0) connectLink(true, deadline);
  while (s.fd >= 0 && listCount == lists && static_cast<int32_t>(deadline - millis()) > 0) {
    agentPump();
    if (s.fd < 0) break;
    pump(50);
  }
  checkInOk = listCount != lists;
  // A schedule's run to show as a note, new since the board last looked: get it whole.
  std::string note;
  uint32_t latestDone = 0;
  {
    Locked l;
    note = agentShared.noteId;
    latestDone = agentShared.latestDone;
  }
  uint32_t seenAsk = 0, seenDone = 0;
  seen(seenAsk, seenDone);
  if (checkInOk && !note.empty() && latestDone > seenDone) {
    {
      Locked l;
      wantOpen = note;
      agentShared.hasOpen = false;
    }
    bool have = false;
    while (s.fd >= 0 && !have && static_cast<int32_t>(deadline - millis()) > 0) {
      agentPump();
      if (s.fd < 0) break;
      pump(50);
      Locked l;
      have = agentShared.hasOpen && agentShared.open.id == note;
    }
  }
  if (!wantLink()) closeSession();
  if (checkInDone) xSemaphoreGive(checkInDone);
}

// A session's result, said: sent like a turn, so its reply plays as one.
void sayResult() {
  std::string id;
  {
    Locked l;
    id.swap(speakSession);
  }
  if (id.empty() || s.fd < 0 || !s.hello || s.capturing || s.draining || s.turnOpen) return;
  {
    Locked l;
    shared.heard.clear();
    shared.reply.clear();
    shared.error.clear();
  }
  if (!sendControl("{\"type\":\"speak\",\"session\":" + quoted(id) + "}")) return;
  turns++;
  s.turnOpen = true;
  s.lastRxMs = millis();
  setState(State::Thinking);
}

void handle(Event e) {
  switch (e) {
    case Event::Open:
      s.wanted = true;
      s.lingerUntilMs = 0;
      if (s.fd < 0) {
        connectSession();
        dropTalkEvents();
      }
      break;
    case Event::Close:
      s.wanted = false;
      lingerOrClose();
      break;
    case Event::Hold:
    case Event::Retry:
      if (holds.load() <= 0) break;
      s.lingerUntilMs = 0;
      s.heldIdle = false;
      if (s.fd < 0) {
        connectSession();
        dropTalkEvents();
      }
      break;
    case Event::Release: lingerOrClose(); break;
    case Event::Kick: break;  // agentPump() sends it, idleDuties() polls
    case Event::TalkStart:
      talkWanted = false;
      beginTalk();
      break;
    case Event::TalkStop: endTalk(); break;
    case Event::CheckIn: checkIn(); break;
    case Event::Speak: sayResult(); break;
    case Event::Read:
      if (s.fd < 0 && readActive.load() && !connectLink(true, millis() + kPollDeadlineMs)) {
        finishRead(false, "CAN'T REACH INKHARNESS");
        if (!wantLink()) closeSession();  // lets Wi-Fi go
      }
      break;
  }
}

void task(void *) {
  for (;;) {
    Event e;
    const TickType_t wait = s.fd >= 0 ? 0 : pdMS_TO_TICKS(idleWaitMs());
    if (xQueueReceive(events, &e, wait) == pdTRUE) {
      handle(e);
      continue;
    }
    bool sent = false;
    if (s.capturing || s.draining) sent = sendMic();
    if (s.fd < 0) {
      idleDuties();
      continue;
    }
    refreshUsb();
    if (s.capturing && millis() - s.talkStartMs > kMaxTalkMs) endTalk();  // a stuck key
    pump(sent || s.capturing ? 0 : 20);
    if (s.fd < 0) continue;
    sendAnswers();
    if (s.fd < 0) continue;
    readPump();
    if (s.fd < 0) continue;
    agentPump();
    if (s.fd < 0) continue;
    pollDuties();
    // Nobody wants it (a release can be lost when the event queue is full, and an Agent screen
    // left alone stops counting): start the linger. Someone wanting it again stops it.
    const bool wanted = wantLink();
    if (wanted) {
      s.lingerUntilMs = 0;
    } else if (s.lingerUntilMs == 0) {
      s.lingerUntilMs = millis() + kLingerMs;
    }
    if (s.lingerUntilMs != 0 && static_cast<int32_t>(millis() - s.lingerUntilMs) >= 0 &&
        !s.capturing && !s.draining && !s.turnOpen && !s.playingOut) {
      s.lingerUntilMs = 0;
      if (!wantLink()) {
        s.heldIdle = holds.load() > 0;  // a key on that screen connects again
        closeSession();
        if (nextPollMs.load() == 0) planPoll(true);  // polls take over from here
        continue;
      }
    }
    if (s.playingOut && !audio::streaming()) {  // the reply has played out
      s.playingOut = s.stream = false;
      if (state.load() == State::Speaking) setState(State::Ready);
    }
    if (s.turnOpen && !s.stream && millis() - s.lastRxMs > kAnswerTimeoutMs) drop("NO ANSWER");
  }
}

// cJSON in PSRAM where it can be: a session list parses into hundreds of small items, which
// IDF's malloc() would otherwise put in internal RAM (AGENTS.md, "Memory"). free() takes
// either kind back, so every other cJSON user is unaffected.
void *jsonAlloc(size_t size) {
  return heap_caps_malloc_prefer(size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                 MALLOC_CAP_DEFAULT);
}

void start() {
  if (events) return;
  cJSON_Hooks hooks = {jsonAlloc, free};
  cJSON_InitHooks(&hooks);
  lock = xSemaphoreCreateMutex();
  checkInDone = xSemaphoreCreateBinary();
  readDone = xSemaphoreCreateBinary();
  loadSummary();
  events = xQueueCreate(8, sizeof(Event));
  xTaskCreate(task, "agent", 10240, nullptr, 4, nullptr);
}

}  // namespace

bool configured() { return settings::hasAgent() && wlan::configured(); }

void begin() { start(); }

void open() {
  start();
  post(Event::Open);
}

void close() { post(Event::Close); }

void talkStart() {
  talkWanted = true;  // a background connect under way gives way
  post(Event::TalkStart);
}

void talkStop() { post(Event::TalkStop); }

Status status() {
  Status out;
  if (!lock) return out;
  {
    Locked l;
    out = shared;
  }
  out.generation = generation.load();
  out.turn = turns.load();
  out.levelDb = audio::captureLevelDb();
  out.mic = audio::capturing();
  return out;
}

bool busy() {
  const State now = state.load();
  return now == State::Connecting || now == State::Listening || now == State::Thinking ||
         now == State::Speaking;
}

void setTools(const char *definitions, ToolCall call) {
  toolDefinitions = definitions;
  toolCall = call;
}

void toolResult(uint32_t id, const std::string &text, bool error) {
  if (!lock) return;
  Locked l;
  answers.push_back({id, text, error});
}

void setTarget(Target (*source)()) { targetSource = source; }

void hold() {
  start();
  holds++;
  post(Event::Hold);
}

void release() {
  if (holds.load() > 0) holds--;
  post(Event::Release);
}

void retry() { post(Event::Retry); }

Sessions sessions() {
  if (!lock) return {};
  Locked l;
  return agentShared;
}

void follow(const std::string &session) {
  if (!lock) return;
  {
    Locked l;
    if (wantOpen == session) return;
    wantOpen = session;
    agentShared.hasOpen = false;
    agentShared.open = {};
    agentChanged();
  }
  post(Event::Kick);
}

namespace {
void request(std::string json) {
  if (!lock) return;
  {
    Locked l;
    outbox.push_back(std::move(json));
  }
  post(Event::Kick);
}
}  // namespace

void answer(const std::string &session, const std::string &ask, int choice) {
  request("{\"type\":\"answer\",\"session\":" + quoted(session) + ",\"ask\":" + quoted(ask) +
          ",\"choice\":" + std::to_string(choice) + "}");
}

void stop(const std::string &session) {
  request("{\"type\":\"stop\",\"session\":" + quoted(session) + "}");
}

void remove(const std::string &session) {
  request("{\"type\":\"delete\",\"session\":" + quoted(session) + "}");
}

void pause(const std::string &session) {
  request("{\"type\":\"pause\",\"session\":" + quoted(session) + "}");
}

void resume(const std::string &session) {
  request("{\"type\":\"resume\",\"session\":" + quoted(session) + "}");
}

void runNow(const std::string &session) {
  request("{\"type\":\"run\",\"session\":" + quoted(session) + "}");
}

void approve(const std::string &session) {
  request("{\"type\":\"approve\",\"session\":" + quoted(session) + "}");
}

void panelChoice(const std::string &session, const std::string &block, int choice) {
  request("{\"type\":\"panel_choice\",\"session\":" + quoted(session) +
          ",\"block\":" + quoted(block) + ",\"choice\":" + std::to_string(choice) + "}");
}

void speak(const std::string &session) {
  if (!lock) return;
  {
    Locked l;
    speakSession = session;
  }
  post(Event::Speak);
}

Glance glance() {
  Glance g;
  if (!lock) return g;
  Locked l;
  const Sessions &a = agentShared;
  if (a.synced) {
    g.known = true;
    g.locked = a.locked;
    g.asks = a.asks;
    g.running = a.running;
    g.planned = a.planned;
    g.proposed = a.proposed;
    g.unread = a.unread;
    g.nextAt = a.nextAt;
    g.checkAt = a.checkAt;
    g.latestAsk = a.latestAsk;
    g.latestDone = a.latestDone;
    g.sayId = a.sayId;
    g.noteId = a.noteId;
    if (a.hasAsk && a.asks > 0) {
      g.askSession = a.ask.session;
      g.askId = a.ask.id;
      g.askAt = a.ask.at;
    }
  } else if (summaryRam.version == kSummaryVersion) {
    const Summary &m = summaryRam;
    g.known = true;
    g.locked = m.locked;
    g.asks = m.asks;
    g.running = m.running;
    g.planned = m.planned;
    g.proposed = m.proposed;
    g.unread = m.unread;
    g.nextAt = m.nextAt;
    g.checkAt = m.checkAt;
    g.latestAsk = m.latestAsk;
    g.latestDone = m.latestDone;
  }
  g.checkedAt = checkedAtUnix.load();
  g.nextCheck = a.connected ? 0 : nextCheckUnix.load();
  g.live = a.connected;
  g.generation = a.generation;
  return g;
}

Summary summary() {
  start();
  Locked l;
  return summaryRam;
}

uint32_t checkedAt() { return checkedAtUnix.load(); }

void setContentPolicy(bool (*allowed)()) { contentPolicy = allowed; }

namespace {
bool seenLoaded = false;
uint32_t seenAsk = 0, seenDone = 0;
}  // namespace

void seen(uint32_t &ask, uint32_t &done) {
  if (!seenLoaded) {
    Preferences prefs;
    if (prefs.begin(kNvs, true)) {
      seenAsk = prefs.getUInt("seenA", 0);
      seenDone = prefs.getUInt("seenD", 0);
      prefs.end();
    }
    seenLoaded = true;
  }
  ask = seenAsk;
  done = seenDone;
}

void markSeen(uint32_t ask, uint32_t done) {
  if (seenLoaded && ask == seenAsk && done == seenDone) return;
  seenAsk = ask;
  seenDone = done;
  seenLoaded = true;
  Preferences prefs;
  if (!prefs.begin(kNvs, false)) return;
  prefs.putUInt("seenA", ask);
  prefs.putUInt("seenD", done);
  prefs.end();
}

void checkSoon() {
  if (!events) return;
  soon = true;
  post(Event::Kick);
}

bool polling() {
  if (pollActive.load()) return true;
  if (!pollIsDue() && !soon.load()) return false;
  post(Event::Kick);  // the task may be asleep in its wait: it polls now
  return true;
}

bool readPage(const std::string &url, uint16_t pictureW, uint16_t pictureH, uint32_t timeoutMs,
              Page &out, std::string &error, bool &reached, bool (*abandon)(void *),
              void *context) {
  reached = false;
  if (!configured()) {
    error = "NO INKHARNESS";
    return false;
  }
  start();
  {
    Locked l;
    reading = ReadRequest{};
    reading.id = nextReadId++;
    reading.url = url;
    reading.pictureW = pictureW;
    reading.pictureH = pictureH;
  }
  xSemaphoreTake(readDone, 0);  // a stale give from before
  readActive = true;
  post(Event::Read);
  const uint32_t until = millis() + timeoutMs;
  bool done = false;
  while (static_cast<int32_t>(until - millis()) > 0) {
    if (xSemaphoreTake(readDone, pdMS_TO_TICKS(100)) == pdTRUE) {
      done = true;
      break;
    }
    if (abandon && abandon(context)) break;
  }
  Locked l;
  readActive = false;  // the connection lingers, then goes, unless something else holds it
  if (!done) {
    reading.done = true;
    error = "INKHARNESS DIDN'T ANSWER";
    reached = reading.sent;
    return false;
  }
  reached = reading.reached;
  error = reading.error;
  if (!error.empty()) return false;
  out = std::move(reading.page);
  return true;
}

bool checkInNow(uint32_t deadlineMs, bool (*abort)()) {
  start();
  xSemaphoreTake(checkInDone, 0);  // a stale give from before
  checkInMs = deadlineMs;
  post(Event::CheckIn);
  const uint32_t until = millis() + deadlineMs + 3000;
  while (static_cast<int32_t>(until - millis()) > 0) {
    if (xSemaphoreTake(checkInDone, pdMS_TO_TICKS(100)) == pdTRUE) return checkInOk;
    if (abort && abort()) return false;
  }
  return false;
}

namespace {
// Sends what becomes of the draft, and forgets it here.
void settleDraft(const char *type) {
  if (!lock) return;
  {
    Locked l;
    const Draft d = agentShared.draft;
    if (d.id.empty()) return;
    outbox.push_back(std::string("{\"type\":\"") + type + "\",\"session\":" + quoted(d.session) +
                     ",\"draft\":" + quoted(d.id) + "}");
    agentShared.draft = {};
    agentChanged();
  }
  post(Event::Kick);
}
}  // namespace

void sendDraft() { settleDraft("send_draft"); }

void dropDraft() { settleDraft("drop_draft"); }

void unlock(const std::string &pin) {
  start();
  {
    Locked l;
    pendingPin = pin;
    agentShared.unlockNote.clear();
    agentChanged();
  }
  post(Event::Kick);
}

}  // namespace agent

#endif  // INKDECK_AGENT
