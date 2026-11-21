#include "features.h"

#if INKDECK_VOICE  // the whole file (features.h)

#include "voice_client.h"

#include <Arduino.h>
#include <WiFi.h>
#include <cJSON.h>
#include <errno.h>
#include <fcntl.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>
#include <string.h>

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

enum class Event : uint8_t { Open, Close, TalkStart, TalkStop };

QueueHandle_t events = nullptr;
SemaphoreHandle_t lock = nullptr;  // guards `shared` and `answers`
Status shared;
std::atomic<State> state{State::Off};
std::atomic<uint32_t> generation{0};
std::atomic<uint32_t> turns{0};

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
  std::vector<uint8_t> rx;
  size_t rxLen = 0;
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
  const int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
  timeval tv = {5, 0};  // a send stuck this long means the link is gone
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
  return fd;
}

// --- Frames from the daemon ------------------------------------------------------------------

std::string field(const cJSON *json, const char *name) {
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(json, name);
  return cJSON_IsString(item) && item->valuestring ? item->valuestring : "";
}

void onControl(const uint8_t *payload, size_t len) {
  cJSON *json = cJSON_ParseWithLength(reinterpret_cast<const char *>(payload), len);
  if (!json) return;
  const std::string type = field(json, "type");
  if (type == "hello") {
    s.hello = true;
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
  s.fd = connectTo(ip, static_cast<uint16_t>(settings::voicePort()), 4000);
  if (s.fd < 0) {
    setState(State::Error, "INKHARNESS ISN'T ANSWERING");
    return;
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
  log_i("voice: connected to %s:%d, encrypted (handshake %u ms, %u of them for keys)",
        host.c_str(), settings::voicePort(), millis() - t0, keyMs);
  setState(State::Ready);
}

void closeSession() {
  if (s.fd >= 0) ::close(s.fd);
  s.fd = -1;
  s.hello = s.turnOpen = false;
  s.staleEnds = 0;
  forgetKeys();
  endAudio();
  forgetAnswers();
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
  if (!sendControl("{\"type\":\"listen_start\"}")) return;
  setState(State::Listening);
}

void beginTalk() {
  if (s.fd < 0) {
    talkIntoNewSession();
    return;
  }
  if (!s.hello || s.capturing) return;
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
  if (!sendControl("{\"type\":\"listen_start\"}")) return;
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
      if (s.fd < 0) {
        connectSession();
        dropTalkEvents();
      }
      break;
    case Event::Close:
      s.wanted = false;
      closeSession();
      break;
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
    if (s.playingOut && !audio::streaming()) {  // the reply has played out
      s.playingOut = s.stream = false;
      if (state.load() == State::Speaking) setState(State::Ready);
    }
    if (s.turnOpen && !s.stream && millis() - s.lastRxMs > kAnswerTimeoutMs) drop("NO ANSWER");
  }
}

void start() {
  if (events) return;
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

}  // namespace voice

#endif  // INKDECK_VOICE
