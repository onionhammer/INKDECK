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
#include <vector>

#include "audio.h"
#include "settings.h"
#include "wlan.h"

namespace voice {
namespace {

constexpr int kProto = 1;
constexpr uint8_t kControl = 1, kAudio = 2;  // frame types
constexpr size_t kHeader = 3;
constexpr size_t kMicFrame = 1024;                    // 32 ms of the mic per frame
constexpr size_t kRxBytes = kHeader + 65535 + 4096;   // the largest frame, and some of the next
constexpr size_t kMaxText = 1500;                     // of the reply kept for the screen
constexpr uint32_t kMaxTalkMs = 30000;
constexpr uint32_t kAnswerTimeoutMs = 45000;  // of silence from the daemon in an open turn

enum class Event : uint8_t { Open, Close, TalkStart, TalkStop };

QueueHandle_t events = nullptr;
SemaphoreHandle_t lock = nullptr;  // guards `shared`
Status shared;
std::atomic<State> state{State::Off};
std::atomic<uint32_t> generation{0};

struct Locked {
  Locked() { xSemaphoreTake(lock, portMAX_DELAY); }
  ~Locked() { xSemaphoreGive(lock); }
};

// The session (voice task only).
struct Session {
  int fd = -1;
  bool wanted = false;     // open() was called and close() wasn't
  bool wifi = false;       // we hold a wlan::connect()
  bool hello = false;      // the daemon answered ours
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

// The connection is gone (or never came up): back to no session, saying why.
void drop(const char *why) {
  if (s.fd >= 0) ::close(s.fd);
  s.fd = -1;
  s.hello = s.turnOpen = false;
  s.staleEnds = 0;
  s.rxLen = 0;
  endAudio();
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

bool sendControl(const std::string &json) {
  if (s.fd < 0) return false;
  std::vector<uint8_t> frame(kHeader + json.size());
  frame[0] = kControl;
  frame[1] = json.size() & 0xFF;
  frame[2] = json.size() >> 8;
  memcpy(frame.data() + kHeader, json.data(), json.size());
  return sendAll(frame.data(), frame.size());
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
  }
  cJSON_Delete(json);
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
    const uint8_t *payload = s.rx.data() + pos + kHeader;
    if (type == kControl) {
      onControl(payload, len);
    } else if (type == kAudio) {
      onAudio(payload, len);
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
  addrinfo hints = {};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo *res = nullptr;
  if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) {
    setState(State::Error, "VOICE HOST NOT FOUND");
    return;
  }
  const uint32_t ip = reinterpret_cast<const sockaddr_in *>(res->ai_addr)->sin_addr.s_addr;
  freeaddrinfo(res);
  s.fd = connectTo(ip, static_cast<uint16_t>(settings::voicePort()), 4000);
  if (s.fd < 0) {
    setState(State::Error, "VOICED ISN'T ANSWERING");
    return;
  }
  if (s.rx.empty()) s.rx.resize(kRxBytes);
  s.rxLen = 0;
  s.hello = false;
  const std::string hello = "{\"type\":\"hello\",\"proto\":" + std::to_string(kProto) +
                            ",\"device\":\"inkdeck\",\"token\":" + quoted(settings::voiceToken()) +
                            ",\"mic_rate\":" + std::to_string(audio::kRecordRate) + "}";
  if (!sendControl(hello)) return;
  const uint32_t t0 = millis();
  while (s.fd >= 0 && !s.hello && millis() - t0 < 4000) pump(100);
  if (s.fd < 0) {  // closed on us: a wrong token ends this way, after an error message
    Locked l;
    if (shared.error == "DISCONNECTED") shared.error = "VOICED REFUSED (TOKEN?)";
    return;
  }
  if (!s.hello) {
    drop("VOICED DIDN'T SAY HELLO");
    return;
  }
  log_i("voice: connected to %s:%d", host.c_str(), settings::voicePort());
  setState(State::Ready);
}

void closeSession() {
  if (s.fd >= 0) ::close(s.fd);
  s.fd = -1;
  s.hello = s.turnOpen = false;
  s.staleEnds = 0;
  endAudio();
  if (s.wifi) {
    WiFi.setSleep(true);
    wlan::disconnect();
    s.wifi = false;
  }
  setState(State::Off);
}

// No session yet (the voice sheet's first hold) or a failed one: this press opens one and is
// heard. The mic goes on first and the capture ring keeps what's said while Wi-Fi joins and
// voiced answers; sendMic() catches up once it has. A release while connecting is still queued
// and ends the utterance as usual.
void talkIntoNewSession() {
  s.wanted = true;
  if (!configured()) {  // nothing to connect to: say why, and leave the mic off
    setState(State::Error, settings::hasVoice() ? "NO WI-FI NETWORK SET" : "NO VOICE HOST SET");
    return;
  }
  const bool mic = audio::captureStart();
  if (mic) {
    s.capturing = true;
    s.talkStartMs = millis();
  }
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
  if (!audio::captureStart()) {
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
  uint8_t frame[kHeader + kMicFrame];
  const size_t n = audio::captureRead(frame + kHeader, kMicFrame, s.capturing ? 20 : 0);
  if (n > 0) {
    frame[0] = kAudio;
    frame[1] = n & 0xFF;
    frame[2] = n >> 8;
    if (!sendAll(frame, kHeader + n)) return false;
    return true;
  }
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
  out.levelDb = audio::captureLevelDb();
  out.mic = audio::capturing();
  return out;
}

bool busy() {
  const State now = state.load();
  return now == State::Connecting || now == State::Listening || now == State::Thinking ||
         now == State::Speaking;
}

}  // namespace voice
