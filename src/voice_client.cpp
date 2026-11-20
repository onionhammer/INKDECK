#include "features.h"

#if INKDECK_VOICE  // the whole file (features.h)

#include "voice_client.h"

#include <Arduino.h>
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
#include <string.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <vector>

#include "audio.h"
#include "settings.h"
#include "tailnet.h"
#include "voice_noise.h"
#include "wlan.h"

namespace voice {
namespace {

constexpr int kProto = 2;  // encrypted (voice_noise.h)
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

enum class Event : uint8_t { Open, Close, TalkStart, TalkStop, Hold, Release, Retry, Kick };

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
Agent agentShared;
std::vector<std::string> outbox;  // answer, stop and delete, waiting to go out
std::string wantOpen;             // agentOpen()'s session
std::string pendingPin;           // agentUnlock()'s digits, until the daemon answers

// The board's tools (setTools()), and the answers waiting to go out (answer()).
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

// The session (voice task only).
struct Session {
  int fd = -1;
  bool wanted = false;     // open() was called and close() wasn't
  bool wifi = false;       // we hold a wlan::connect()
  bool tailnet = false;    // and a tailnet::hold(): voice_host is on the tailnet
  bool hello = false;      // the daemon answered ours
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
  s.hello = s.turnOpen = false;
  s.staleEnds = 0;
  s.rxLen = 0;
  forgetKeys();
  endAudio();
  forgetAnswers();
  forgetAgentConnection();
  log_w("voice: %s", why);
  setState(State::Error, why);
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
    log_w("voice: a %u-byte message is too long to send", json.size());
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
  a.options.clear();
  const cJSON *option;
  cJSON_ArrayForEach(option, cJSON_GetObjectItemCaseSensitive(json, "options")) {
    a.options.push_back({field(option, "title"), field(option, "right"), field(option, "detail")});
  }
}

// The rows as the list draws them (`updated` isn't shown).
bool sameRows(const std::vector<AgentRow> &a, const std::vector<AgentRow> &b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    const AgentRow &x = a[i], &y = b[i];
    if (x.id != y.id || x.title != y.title || x.state != y.state || x.place != y.place ||
        x.sub != y.sub || x.step != y.step || x.steps != y.steps || x.progress != y.progress ||
        x.unread != y.unread || x.herdr != y.herdr || x.program != y.program) {
      return false;
    }
  }
  return true;
}

void onSessions(const cJSON *json) {
  std::vector<AgentRow> rows;
  const cJSON *item;
  cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(json, "sessions")) {
    AgentRow r;
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
    if (!r.id.empty()) rows.push_back(std::move(r));
  }
  const cJSON *counts = cJSON_GetObjectItemCaseSensitive(json, "counts");
  const bool locked = flag(json, "locked");
  const int asks = std::max(0, intField(counts, "ask", 0));
  const int running = std::max(0, intField(counts, "running", 0));
  const int idle = std::max(0, intField(counts, "idle", 0));
  const int planned = std::max(0, intField(counts, "planned", 0));
  const int unread = std::max(0, intField(counts, "unread", 0));
  const int older = std::max(0, intField(json, "older", 0));
  Locked l;
  // A working session sends a list about once a second (every log line is a change). Only
  // what the board shows counts as a change here, so the panel redraws only for that.
  const bool same = agentShared.synced && agentShared.locked == locked &&
                    agentShared.asks == asks && agentShared.running == running &&
                    agentShared.idle == idle &&
                    agentShared.planned == planned && agentShared.unread == unread &&
                    agentShared.older == older && sameRows(agentShared.rows, rows);
  if (same) return;
  agentShared.synced = true;
  agentShared.locked = locked;
  agentShared.asks = asks;
  agentShared.running = running;
  agentShared.idle = idle;
  agentShared.planned = planned;
  agentShared.unread = unread;
  agentShared.older = older;
  agentShared.rows.swap(rows);
  agentChanged();
}

void onSession(const cJSON *json) {
  AgentSession a;
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
  log_w("voice: inkharness refused %s: %s", field(json, "request").c_str(), message.c_str());
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
  } else if (type == "sessions") {
    onSessions(json);
  } else if (type == "session") {
    onSession(json);
  } else if (type == "ask") {
    // The board shows questions from the list and the session for now; the ask sheet over
    // other screens comes with the check-ins (issue #52's next steps). The list follows anyway.
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
    log_i("voice: sealing and opening %u KB took %u ms", s.cryptoBytes / 1024,
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
        answer(callId, "this board runs no tools", true);
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
  return message == "wrong token" ? "WRONG VOICE TOKEN" : "INKHARNESS: " + message;
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
  if (holds.load() > 0 && !s.synced) {
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

// Ends the connection once nobody holds it, after kLingerMs.
void lingerOrClose() {
  if (holds.load() > 0 || s.wanted) {
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

void connectSession() {
  {
    Locked l;
    shared.error.clear();
    shared.heard.clear();
    shared.reply.clear();
    answers.clear();  // late ones, for a session that has gone
  }
  setState(State::Connecting);
  if (!settings::hasVoice()) {
    setState(State::Error, "NO VOICE HOST SET");
    return;
  }
  if (!s.wifi) {
    std::string error;
    if (!wlan::connect(15000, error)) {
      setState(State::Error, error.c_str());
      return;
    }
    s.wifi = true;
    // Modem sleep wakes the radio only at the access point's beacons: every frame would wait
    // for one. Off for the session (shares.cpp and ota.cpp do the same).
    WiFi.setSleep(false);
  }
  const std::string host = settings::voiceHost();
  const uint16_t port = static_cast<uint16_t>(settings::voicePort());
  // A voice host on the tailnet is often on this LAN too (at home): its LAN addresses first, as
  // the tailnet last gave them, which needs no join. The token vouches for whoever answers.
  if (!s.tailnet && tailnet::routes(host)) {
    const int fd = tailnet::connectLan(host, port, 1000);
    if (fd >= 0) s.fd = configureSocket(fd);
  }
  if (s.fd < 0) {
    if (!s.tailnet && tailnet::routes(host)) {
      std::string error;
      if (!tailnet::hold(20000, error)) {
        setState(State::Error, error.c_str());
        return;
      }
      s.tailnet = true;
    }
    // The WireGuard session with the daemon's machine first: a connect before it would lose its
    // SYN and wait a second or more for TCP to send it again.
    if (s.tailnet) tailnet::reach(host, 8000);
    addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *res = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) {  // tailnet names too
      setState(State::Error, s.tailnet ? "VOICE HOST NOT ON THE TAILNET" : "VOICE HOST NOT FOUND");
      return;
    }
    const uint32_t ip = reinterpret_cast<const sockaddr_in *>(res->ai_addr)->sin_addr.s_addr;
    freeaddrinfo(res);
    s.fd = connectTo(ip, port, 4000);
    if (s.fd < 0) {
      setState(State::Error, "INKHARNESS ISN'T ANSWERING");
      return;
    }
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
  s.handshake.reset(new noise::Initiator(settings::voiceToken()));
  const std::vector<uint8_t> msg1 = s.handshake->hello(hello);
  const uint32_t keyMs = millis() - t0;
  if (msg1.empty() || msg1.size() > 65535) {
    drop(msg1.empty() ? "NO VOICE KEY (MBEDTLS)" : "TOO MANY VOICE TOOLS");
    return;
  }
  uint8_t header[kHeader] = {kHandshake, static_cast<uint8_t>(msg1.size() & 0xFF),
                             static_cast<uint8_t>(msg1.size() >> 8)};
  if (!sendAll(header, kHeader) || !sendAll(msg1.data(), msg1.size())) return;
  while (s.fd >= 0 && !s.hello && millis() - t0 < 4000) pump(100);
  s.handshake.reset();
  if (s.fd < 0) {  // hung up on us: after saying why, unless it was something else
    if (!s.refusal.empty()) setState(State::Error, refusalText(s.refusal).c_str());
    return;
  }
  if (!s.hello) {
    if (!s.refusal.empty()) {
      drop(refusalText(s.refusal).c_str());
    } else {
      // A daemon from before encryption skips the handshake and waits for a plain hello.
      drop(s.sealed ? "INKHARNESS DIDN'T SAY HELLO" : "NO HANDSHAKE (OLD INKHARNESS?)");
    }
    return;
  }
  log_i("voice: connected to %s:%d%s, encrypted (handshake %u ms, %u of them for keys)%s",
        host.c_str(), settings::voicePort(),
        s.tailnet ? " through the tailnet" : tailnet::routes(host) ? " on the LAN" : "",
        millis() - t0, keyMs,
        s.agentSupported ? (s.agentUnlocked ? ", sessions" : ", sessions behind its PIN") : "");
  setState(State::Ready);
}

void closeSession() {
  if (s.fd >= 0) ::close(s.fd);
  s.fd = -1;
  s.hello = s.turnOpen = false;
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

// No session yet (the voice sheet's first hold) or a failed one: this press opens one and is
// heard. The mic goes on first and the capture ring keeps what's said while Wi-Fi joins and
// inkharness answers; sendMic() catches up once it has. A release while connecting is still queued
// and ends the utterance as usual.
void talkIntoNewSession() {
  s.wanted = true;
  if (!configured()) {  // nothing to connect to: say why, and leave the mic off
    setState(State::Error, settings::hasVoice() ? "NO WI-FI NETWORK SET" : "NO VOICE HOST SET");
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
      if (s.fd < 0) {
        connectSession();
        dropTalkEvents();
      }
      break;
    case Event::Release: lingerOrClose(); break;
    case Event::Kick: break;  // agentPump() sends it
    case Event::TalkStart: beginTalk(); break;
    case Event::TalkStop: endTalk(); break;
  }
}

void task(void *) {
  for (;;) {
    Event e;
    if (xQueueReceive(events, &e, s.fd >= 0 ? 0 : portMAX_DELAY) == pdTRUE) {
      handle(e);
      continue;
    }
    bool sent = false;
    if (s.capturing || s.draining) sent = sendMic();
    if (s.fd < 0) continue;
    if (s.capturing && millis() - s.talkStartMs > kMaxTalkMs) endTalk();  // a stuck key
    pump(sent || s.capturing ? 0 : 20);
    if (s.fd < 0) continue;
    sendAnswers();
    if (s.fd < 0) continue;
    agentPump();
    if (s.fd < 0) continue;
    // Nobody holds it (a release can be lost when the event queue is full): start the linger.
    if (holds.load() <= 0 && !s.wanted && s.lingerUntilMs == 0) s.lingerUntilMs = millis() + kLingerMs;
    if (s.lingerUntilMs != 0 && static_cast<int32_t>(millis() - s.lingerUntilMs) >= 0 &&
        !s.capturing && !s.draining && !s.turnOpen && !s.playingOut) {
      s.lingerUntilMs = 0;
      if (holds.load() <= 0 && !s.wanted) {
        closeSession();
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
  events = xQueueCreate(8, sizeof(Event));
  xTaskCreate(task, "voice", 10240, nullptr, 4, nullptr);
}

}  // namespace

bool configured() { return settings::hasVoice() && wlan::configured(); }

void begin() { start(); }

void open() {
  start();
  post(Event::Open);
}

void close() { post(Event::Close); }

void talkStart() { post(Event::TalkStart); }

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

void answer(uint32_t id, const std::string &text, bool error) {
  if (!lock) return;
  Locked l;
  answers.push_back({id, text, error});
}

void setTarget(Target (*source)()) { targetSource = source; }

void agentHold() {
  start();
  holds++;
  post(Event::Hold);
}

void agentRelease() {
  if (holds.load() > 0) holds--;
  post(Event::Release);
}

void agentRetry() { post(Event::Retry); }

Agent agent() {
  if (!lock) return {};
  Locked l;
  return agentShared;
}

void agentOpen(const std::string &session) {
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

void agentAnswer(const std::string &session, const std::string &ask, int choice) {
  request("{\"type\":\"answer\",\"session\":" + quoted(session) + ",\"ask\":" + quoted(ask) +
          ",\"choice\":" + std::to_string(choice) + "}");
}

void agentStop(const std::string &session) {
  request("{\"type\":\"stop\",\"session\":" + quoted(session) + "}");
}

void agentDelete(const std::string &session) {
  request("{\"type\":\"delete\",\"session\":" + quoted(session) + "}");
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

void agentSendDraft() { settleDraft("send_draft"); }

void agentDropDraft() { settleDraft("drop_draft"); }

void agentUnlock(const std::string &pin) {
  start();
  {
    Locked l;
    pendingPin = pin;
    agentShared.unlockNote.clear();
    agentChanged();
  }
  post(Event::Kick);
}

}  // namespace voice

#endif  // INKDECK_VOICE
