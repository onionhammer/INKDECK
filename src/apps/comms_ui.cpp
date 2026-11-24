#include "features.h"

#if INKDECK_COMMS  // the whole file (features.h)

#include "apps/comms_ui.h"

#include <Arduino.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <vector>

#include "apps/home.h"
#include "comms_mic.h"
#include "input.h"
#include "rtc.h"
#include "timeutil.h"
#include "ui/menu.h"

namespace apps {

namespace {
// What the last HOLD BOOT began, so its release ends it: a message (the ringing sheet), or the
// mic. The input timer calls both ends (hold_boot.cpp); the UI task's key filter latches and
// unlatches the mic (commsui::latchKey()), so the latch is under a lock.
std::atomic<bool> heldMessage{false};
std::mutex latchMu;
comms::wire::TalkLatch latch;
}  // namespace

namespace commsui {
namespace {

constexpr uint32_t kErrorShowMs = 8000;  // a failed attempt's notice
constexpr uint32_t kKeyedMs = 30000;     // keyedLately()
std::atomic<uint32_t> attemptMs{0};      // the last attempt from these screens
std::atomic<uint32_t> attemptGen{0};     // comms::generation() then

constexpr ui::TextStyle kLogText{&ui::fonts::kMono14Bold, 0};    // the log's who → whom
constexpr ui::TextStyle kLogTime{&ui::fonts::kMono14Medium, 0};  // its time
constexpr ui::TextStyle kMeterLabel{&ui::fonts::kMono13ExtraBold, 0};

const std::string kDot = std::string(" ") + ui::glyph::kDot + " ";

// ms into the current minute of the clock the status bar shows (of millis() while it isn't
// set), so that every length in whole minutes turns over with it, in the same refresh.
uint32_t intoMinuteMs() {
  if (rtc::timeValid()) {
    timeval tv;
    gettimeofday(&tv, nullptr);
    return static_cast<uint32_t>(tv.tv_sec % 60) * 1000 + static_cast<uint32_t>(tv.tv_usec / 1000);
  }
  return millis() % 60000;
}

}  // namespace

void noteAttempt() {
  attemptGen = comms::generation();
  attemptMs = millis() | 1;
}

std::string attemptError() {
  const uint32_t at = attemptMs.load();
  if (!at || millis() - at > kErrorShowMs) return {};
  // Not before the engine has acted on it (it bumps the generation when it fails, and clears
  // the error when it doesn't): until then status().error is an older attempt's.
  if (comms::generation() == attemptGen.load()) return {};
  return comms::status().error;
}

bool playing() { return comms::status().replay; }

bool stopIfPlaying() {
  if (!playing()) return false;
  comms::stopPlaying();
  return true;
}

bool micHeld() {
  std::lock_guard<std::mutex> l(latchMu);
  return latch.held();
}

bool micLatched() {
  std::lock_guard<std::mutex> l(latchMu);
  return latch.latched();
}

ui::Hint micHint() {
  std::lock_guard<std::mutex> l(latchMu);
  if (latch.latched()) return {"BOOT", "STOP"};
  if (latch.held()) return {ui::glyph::kPress, "HOLD"};
  return {"HOLD BOOT", "TALK"};
}

ui::TalkTo micTalkTo(int group, uint32_t target) {
  ui::TalkTo to;
  to.comms = true;
  to.group = group;
  to.board = target;
  return to;
}

ui::TalkTo ringTalkTo(int group, uint32_t target) {
  ui::TalkTo to = micTalkTo(group, target);
  to.answer = true;
  return to;
}

bool latchKey(ui::Key key) {
  if (key != ui::Key::Select && key != ui::Key::Back) return false;
  comms::wire::TalkLatch::Act act = comms::wire::TalkLatch::Act::None;
  {
    std::lock_guard<std::mutex> l(latchMu);
    if (!latch.held() && !latch.latched()) return false;
    // A latch ends on the screens where HOLD BOOT keys the mic (the Comms screens and sheets);
    // elsewhere ● and BOOT do their own jobs, and the mic stays latched.
    const bool ends = ui::nav::talkTarget().comms;
    if (!latch.press(key == ui::Key::Select, ends, act)) return false;
  }
  if (act == comms::wire::TalkLatch::Act::Latch) {
    comms::latchMic();
  } else if (act == comms::wire::TalkLatch::Act::MicOff) {
    comms::micOff();
  }
  return true;  // the navigator redraws: the hints change
}

void syncLatch() {
  std::lock_guard<std::mutex> l(latchMu);
  // The mic went off another way (a leave, the engine, a power-off): nothing is latched.
  if (latch.latched() && !latch.held() && !comms::micEnabled()) latch.reset();
}

ui::MicMark micMark() {
  if (!comms::micEnabled()) return ui::MicMark::None;
  const comms::Mic m = comms::mic();
  // Not for its first moments (STARTING), which would flash the mark outlined.
  if (m.on && !m.capturing && !m.paused.empty() && m.paused != "STARTING") {
    return ui::MicMark::Paused;
  }
  return ui::MicMark::On;
}

const char *stateWord(comms::State s) {
  switch (s) {
    case comms::State::Off: return "OFF";
    case comms::State::Quiet: return "QUIET";
    case comms::State::Mic: return "MIC ON";
    case comms::State::Hearing: return "HEARING";
  }
  return "";
}

int meterFill(float levelDb) { return std::clamp(static_cast<int>((levelDb + 60) / 3), 0, 20); }

std::string lengthText(uint32_t ms) { return timeutil::clockText((ms + 500) / 1000); }

std::string minutesText(uint32_t ms) {
  const uint32_t into = intoMinuteMs();
  const uint32_t m = ms > into ? (ms - into) / 60000 : 0;
  if (m == 0) return "<1 MIN";
  if (m < 60) return std::to_string(m) + " MIN";
  char buf[24];
  snprintf(buf, sizeof buf, "%lu H %02lu", static_cast<unsigned long>(m / 60),
           static_cast<unsigned long>(m % 60));
  return buf;
}

std::string lostText(uint32_t ms) {
  if (ms < 60000) return timeutil::clockText(ms / 10000 * 10);
  return minutesText(ms);
}

std::string whenText(time_t at, uint32_t agoS) {
  if (at && rtc::timeValid()) {
    const time_t now = time(nullptr);
    if (timeutil::sameLocalDay(at, now)) return timeutil::hhmm(at);
    tm t;
    localtime_r(&at, &t);
    return std::to_string(t.tm_mday) + " " + timeutil::kMonthNames[t.tm_mon];
  }
  if (agoS < 60) return "NOW";
  if (agoS < 3600) return std::to_string(agoS / 60) + " MIN";
  return std::to_string(agoS / 3600) + " H";
}

std::string boardName(const std::string &name, uint32_t id) {
  if (!name.empty()) return name;
  char buf[16];
  snprintf(buf, sizeof buf, "%08lX", static_cast<unsigned long>(id));
  return buf;
}

std::string whoText(const comms::Call &c) {
  if (c.outgoing) return "YOU \xE2\x86\x92 " + (c.to ? boardName(c.toName, c.to) : "ALL");
  return boardName(c.fromName, c.from) + " \xE2\x86\x92 " + (c.to ? "YOU" : "ALL");
}

std::string noteText(const comms::Call &c) {
  // Rings and messages share the log with mic sessions (comms.h, Kind).
  if (c.kind == comms::Kind::Ring) {
    if (c.live) return "RINGING";
    if (c.answered) return "ANSWERED";
    if (c.outgoing) return "NO ANSWER";
    return c.missed ? "MISSED" : "RANG";  // RANG: ignored here, or a message instead
  }
  if (c.kind == comms::Kind::Message) {
    if (c.outgoing) {
      if (c.live) return "RECORDING";
      return c.heardBy > 0 ? "MSG" + std::string(" ") + ui::glyph::kDot + " " +
                                 std::to_string(c.heardBy) + " GOT IT"
                           : "MSG SENT";
    }
    if (c.live) return "MSG COMING";
    if (!c.played) return "NEW MSG";
    return c.complete ? "MSG" : "MSG PART";
  }
  if (c.live) return "LIVE";
  if (c.outgoing) return "";
  if (c.linkLost) return "LOST";
  if (c.cutOff) return "CUT OFF";
  if (!c.played) return "NOT PLAYED";
  if (c.route == comms::Route::Relayed) {
    if (!c.relayName.empty()) return "VIA " + c.relayName;
    return c.hops == 1 ? "1 HOP" : std::to_string(c.hops) + " HOPS";
  }
  return "";
}

bool noteNeedsYou(const comms::Call &c) {
  if (c.outgoing || c.live) return false;
  if (c.kind == comms::Kind::Ring) return c.missed && !c.heard;
  if (c.kind == comms::Kind::Message) return !c.played;
  return c.linkLost && !c.heard;
}

std::string callLength(const comms::Call &c) {
  return c.live ? minutesText(c.ms) : lengthText(c.ms);
}

std::string countText(const std::vector<comms::Call> &calls) {
  if (calls.empty()) return {};
  bool today = rtc::timeValid();
  const time_t now = time(nullptr);
  for (const comms::Call &c : calls) today = today && c.at && timeutil::sameLocalDay(c.at, now);
  const std::string n = std::to_string(calls.size());
  return today ? "TODAY \xC2\xB7 " + n : n + (calls.size() == 1 ? " CALL" : " CALLS");
}

std::string boardsText(int n) { return std::to_string(n) + (n == 1 ? " BOARD" : " BOARDS"); }

std::string membersText(const comms::Conversation &c) {
  std::string s = c.in ? "YOU" : "";
  for (size_t i = 0; i < c.members.size(); ++i) {
    if (!s.empty()) s += " + ";
    s += boardName(i < c.names.size() ? c.names[i] : std::string(), c.members[i]);
  }
  return s;
}

std::string forText(uint32_t ms) {
  const std::string m = minutesText(ms);
  return m == "<1 MIN" ? "JUST BEGUN" : "FOR " + m;
}

std::string groupName(int group) {
  const std::vector<comms::Group> gs = comms::groups();
  return group >= 0 && group < static_cast<int>(gs.size()) ? gs[group].name : std::string();
}

int steadyRssi(uint32_t id, int rssi) {
  static std::map<uint32_t, int> shown;
  if (rssi == 0) return 0;
  int &s = shown[id];
  if (s == 0 || abs(rssi - s) >= 6) s = rssi;
  return s;
}

std::string reachText(uint32_t id, comms::Route route, int hops, const std::string &relay,
                      int rssi) {
  if (route == comms::Route::Relayed) {
    return "VIA " + (relay.empty() ? std::string("A RELAY") : relay) + kDot +
           std::to_string(hops) + (hops == 1 ? " HOP" : " HOPS");
  }
  if (route == comms::Route::Air) {
    const int r = steadyRssi(id, rssi);
    return "ON AIR" + (r ? kDot + ui::signedNumber(static_cast<float>(r), 0) + " dBm" : "");
  }
  if (route == comms::Route::Net) return "VIA INKHARNESS";
  return {};
}

std::string pausedText(const std::string &why) {
  static const struct {
    const char *from, *to;
  } kShort[] = {{"THE AGENT IS LISTENING", "AGENT LISTENING"},
                {"THE AGENT IS SPEAKING", "AGENT SPEAKING"},
                {"THE RECORDER IS ON", "RECORDER ON"},
                {"AN ALARM IS RINGING", "ALARM RINGING"},
                {"MUSIC IS PLAYING", "MUSIC PLAYING"},
                {"A MESSAGE IS RECORDING", "MESSAGE RECORDING"},
                {"THE MIC WON'T START", "WON'T START"}};
  for (const auto &k : kShort) {
    if (why == k.from) return k.to;
  }
  return why;
}

std::string micLine(const comms::OpenMic &m) {
  if (m.link == comms::Link::Lost) return "LOST " + lostText(m.linkMs);
  std::string s = "MIC ON " + minutesText(m.onS * 1000);
  if (m.toMe) {
    s += kDot + "TO YOU";
  } else if (m.target) {
    std::string name;
    for (const comms::Board &b : comms::boards(m.group)) {
      if (b.id == m.target) name = b.name;
    }
    s += kDot + "TO " + boardName(name, m.target);
  }
  if (m.paused) return s + kDot + "PAUSED";
  return m.latched ? s + kDot + "LATCHED" : s;
}

const comms::OpenMic *findMic(const std::vector<comms::OpenMic> &mics, int group, uint32_t id) {
  for (const comms::OpenMic &m : mics) {
    if (m.id == id && (group < 0 || m.group == group)) return &m;
  }
  return nullptr;
}

std::string micsGlance() {
  const std::vector<comms::OpenMic> mics = comms::openMics(-1);
  std::vector<const comms::OpenMic *> lost, on;
  for (const comms::OpenMic &m : mics) (m.link == comms::Link::Lost ? lost : on).push_back(&m);
  const std::string dot = " \xC2\xB7 ";
  if (lost.size() == 1) return boardName(lost[0]->name, lost[0]->id) + " lost";
  if (lost.size() > 1) return std::to_string(lost.size()) + " lost";
  if (comms::micEnabled()) {
    return on.empty() ? "yours on" : "yours + " + std::to_string(on.size()) + " on";
  }
  if (on.size() == 1) {
    std::string m = minutesText(on[0]->onS * 1000);
    for (char &ch : m) ch = static_cast<char>(tolower(ch));
    const std::string name = boardName(on[0]->name, on[0]->id);
    // The minutes when they fit Home's cell (about 21 characters).
    return name.size() + 4 + dot.size() + m.size() > 23 ? name + " on" : name + " on" + dot + m;
  }
  if (on.size() > 1) return std::to_string(on.size()) + " on";
  return "none on";
}

bool keyedLately() { return input::idleMs() < kKeyedMs; }

void drawCallRow(ui::Canvas &c, const ui::Rect &r, const comms::Call &call, bool noteColumn) {
  using namespace ui;
  dashedH(c, r.x, r.bottom() - 1, r.w);
  const Rect band{r.x + kPadX, r.y, r.w - 2 * kPadX, r.h - 1};
  constexpr int kTimeW = 58;
  drawText(c, {band.x, band.y, kTimeW, band.h}, whenText(call.at, call.agoS), kLogTime);
  const std::string len = callLength(call);
  const int lenW = std::max(40, textWidth(kLogText, len));
  drawText(c, {band.right() - lenW, band.y, lenW, band.h}, len, kLogText, Align::Right);
  int right = band.right() - lenW - 8;
  std::string who = whoText(call);
  const std::string note = noteText(call);
  if (!note.empty()) {
    if (noteColumn && noteNeedsYou(call)) {  // a missed ring, a LOST, a message not played
      const int w = tagWidth(note, style::kCap, 4);
      drawTag(c, right - w, band.centerY(), 19, note, style::kCap, TagKind::Inverted, 4);
      right -= w + 8;
    } else if (noteColumn) {
      const int w = textWidth(style::kKey, note);
      drawText(c, {right - w, band.y, w, band.h}, note, style::kKey);
      right -= w + 8;
    } else {
      who += std::string(" ") + glyph::kDot + " " + note;
    }
  }
  drawText(c, {band.x + kTimeW, band.y, right - band.x - kTimeW, band.h}, who, kLogText);
}

std::string callRowKey(const comms::Call &call) {
  return whenText(call.at, call.agoS) + "|" + whoText(call) + "|" + noteText(call) +
         (noteNeedsYou(call) ? "!" : "") + "|" + callLength(call) + "\n";
}

void drawMicMeter(ui::Canvas &c, int x, int y, int w, int h, const char *label, int filled,
                  uint16_t color) {
  using namespace ui;
  constexpr int kGap = 2, kSegs = 20;
  const int kLabelW = std::max(34, textWidth(kMeterLabel, label) + 6);
  drawText(c, {x, y, kLabelW, h}, label, kMeterLabel, Align::Left, color);
  const int mx = x + kLabelW + 4, mw = w - kLabelW - 4;
  const int segW = (mw - kGap * (kSegs - 1)) / kSegs;
  for (int i = 0; i < kSegs; ++i) {
    const Rect seg{mx + i * (segW + kGap), y, segW, h};
    if (i < filled) {
      fill(c, seg, color);
    } else {
      frame(c, seg, 2, color);
    }
  }
}

void drawStateRow(ui::Canvas &c, const ui::Rect &row, const std::string &word, int filled,
                  const std::string &right) {
  using namespace ui;
  dashedH(c, row.x, row.bottom() - 1, row.w);
  const int base = baselineCentered(style::kReading, row.y, row.h - 1);
  const int end = drawText(c, row.x + kPadX, base, word, style::kReading);
  if (filled >= 0) {
    const int meterX = std::max(row.x + kPadX + 120 + 14, end + 14);
    drawSegments(c, {meterX, row.y + (row.h - 16) / 2, row.right() - kPadX - meterX, 16}, 20,
                 filled, 2);
  } else if (!right.empty()) {
    drawText(c, row.right() - kPadX - textWidth(style::kReading, right), base, right,
             style::kReading);
  }
}

int drawNameBlock(ui::Canvas &c, const ui::Rect &r, const std::string &label,
                  const std::string &name, const std::string &line, const std::string &line2) {
  using namespace ui;
  const int x = r.x + kPadX, w = r.w - 2 * kPadX;
  int y = r.y + 14;
  drawText(c, {x, y, w, 17}, label, style::kKey);
  y += 17 + 6;
  drawText(c, {x, y, w, 38}, name, style::kTitle);
  y += 38 + 6;
  drawText(c, {x, y, w, 17}, line, style::kLabel);
  y += 17;
  if (!line2.empty()) {
    y += 4;
    drawText(c, {x, y, w, 17}, line2, style::kLabel);
    y += 17;
  }
  return y + 12;
}

void drawEarlier(ui::Canvas &c, ui::Rect r, const std::vector<comms::Call> &calls,
                 const std::string &foot) {
  using namespace ui;
  constexpr int kEarlierRowH = 30;
  hrule(c, r.x, r.y, r.w);
  r.takeTop(kRule);
  drawSectionHeader(c, r.takeTop(kSectionH), "EARLIER", calls.empty() ? "NONE" : countText(calls));
  const Rect f = r.takeBottom(8 + 17);
  for (const comms::Call &k : calls) {
    if (r.h < kEarlierRowH + 6) break;  // a gap over the foot
    drawCallRow(c, r.takeTop(kEarlierRowH), k, false);
  }
  drawText(c, {f.x + kPadX, f.y, f.w - 2 * kPadX, 17}, foot, style::kLabel);
}

}  // namespace commsui

void commsTalk(const ui::TalkTo &to) {
  if (to.message) {
    commsui::noteAttempt();
    heldMessage = true;
    comms::messageStart();  // the ring nobody answered: the message goes to whom it rang
    return;
  }
  // The mic, on while it's held (the ring answered first on the ring sheet: it becomes the call
  // sheet). A latched mic: the hold takes it over, and the release ends it. A mic on already
  // otherwise (a dev command's) is left as it is, and the release leaves it too.
  if (to.answer) comms::answer();
  comms::wire::TalkLatch::Act act;
  {
    std::lock_guard<std::mutex> l(latchMu);
    act = latch.holdStart(comms::micEnabled());
  }
  if (act != comms::wire::TalkLatch::Act::MicOn) return;
  commsui::noteAttempt();
  if (!comms::micOn(to.group, to.board)) {  // the screen says why
    std::lock_guard<std::mutex> l(latchMu);
    latch.reset();
  }
}

void commsTalkStop() {
  if (heldMessage.exchange(false)) comms::messageSend();
  comms::wire::TalkLatch::Act act;
  {
    std::lock_guard<std::mutex> l(latchMu);
    act = latch.holdEnd();
  }
  if (act == comms::wire::TalkLatch::Act::MicOff) comms::micOff();
}

namespace {

using namespace commsui;
using ui::Key;
using ui::Rect;

const std::string kDot = std::string(" ") + ui::glyph::kDot + " ";

constexpr int kChannelH = 156;   // under the CHANNEL header
constexpr int kMaxTalkRows = 5;  // TALK TO's rows on screen; more page
constexpr int kLogRowH = 30;
constexpr int kMaxLog = 24;
constexpr int kLogKeyRows = 8;   // log rows that count for a redraw (more than fit)
constexpr uint32_t kStartingMs = 6000;  // OFF at first says the radio is starting

constexpr ui::TextStyle kMapText{&ui::fonts::kMono13Bold, 0};

// --- Boards and groups as the rows say them ---------------------------------------------------

std::string plural(int n, const char *one, const char *many) {
  return std::to_string(n) + " " + (n == 1 ? one : many);
}

std::string dbm(int rssi) { return ui::signedNumber(static_cast<float>(rssi), 0) + " dBm"; }

std::string routeTag(const comms::Board &b) {
  if (b.route == comms::Route::Away) return "AWAY";
  if (b.asleep) return "ASLEEP";  // reachable on battery: it polls
  if (b.flags & comms::kFlagMic) return "MIC";
  switch (b.route) {
    case comms::Route::Air: return "AIR";
    case comms::Route::Relayed: return plural(b.hops, "HOP", "HOPS");
    case comms::Route::Net: return "NET";
    case comms::Route::Away: break;
  }
  return "AWAY";
}

// How a board is reached, for its row's second line.
std::string routeLine(const comms::Board &b) {
  std::string s;
  if (b.asleep && b.route != comms::Route::Away) {
    // Asleep on battery, reachable: a ring, voice it would hear or a message wakes it at its next
    // poll.
    s = "ASLEEP" + kDot + "WAKES WITHIN " + std::to_string(b.pollS) + " S";
    if (b.battery >= 0) s += kDot + std::to_string(b.battery) + "%";
    return s;
  }
  switch (b.route) {
    case comms::Route::Air: {
      const int r = steadyRssi(b.id, b.rssi);
      s = r ? "ON AIR" + kDot + dbm(r) : "ON AIR";
      break;
    }
    case comms::Route::Relayed:
      s = b.relayName.empty() ? "ON AIR THROUGH " + plural(b.hops, "RELAY", "RELAYS")
                              : "ON AIR VIA " + b.relayName;
      break;
    case comms::Route::Net: s = "VIA INKHARNESS"; break;
    case comms::Route::Away: {
      if (b.heardAt && rtc::timeValid()) return "NOT HEARD SINCE " + whenText(b.heardAt, 0);
      const uint32_t m = b.heardAgoS / 60;
      return m < 60 ? "NOT HEARD FOR " + std::to_string(m) + " MIN"
                    : "NOT HEARD FOR " + std::to_string(m / 60) + " H";
    }
  }
  if (b.flags & comms::kFlagMic) return s + kDot + "MIC ON";
  if (b.talkedAt) s += kDot + "TALKED " + whenText(b.talkedAt, 0);
  return s;
}

// "7F3A · 3 ON AIR · 1 THROUGH INKHARNESS", with "1 NEW" when it has calls, missed rings or
// messages not heard.
std::string groupLine(const comms::Group &g) {
  std::string s = g.tag;
  if (g.unheard > 0) s += kDot + std::to_string(g.unheard) + " NEW";
  if (g.via != comms::Via::Net) {
    s += kDot + (g.onAir ? std::to_string(g.onAir) : "NONE") + " ON AIR";
  }
  if (g.micsOn > 0) s += kDot + plural(g.micsOn, "MIC", "MICS") + " ON";
  if (g.via == comms::Via::Air) {
    s += kDot + "NEVER LEAVES THE RADIO";
  } else if (g.onNet > 0) {
    s += kDot + std::to_string(g.onNet) + " THROUGH INKHARNESS";
  } else if (g.via == comms::Via::Net) {
    s += kDot + (comms::netUp() ? "NOBODY THROUGH INKHARNESS" : "NET ONLY: NO LINK YET");
  }
  return s;
}

// "AIR CH 6 · NET UP", "AIR CH 6", "AIR CH 6 · LONG RANGE" (this board sends at the long-range
// rate), "NET DOWN", "OFF".
std::string channelText(const comms::Status &s, comms::Via via) {
  if (s.state == comms::State::Off || s.channel == 0) return "OFF";
  const std::string air =
      "AIR CH " + std::to_string(s.channel) + (s.longRange ? kDot + "LONG RANGE" : "");
  switch (via) {
    case comms::Via::Air: return air;
    case comms::Via::Net: return s.netUp ? "NET UP" : "NET DOWN";
    case comms::Via::Both: return s.netUp ? air + kDot + "NET UP" : air;
  }
  return air;
}

// --- Drawing helpers --------------------------------------------------------------------------

// A line 2 px thick: solid (off 0), or `on` px of ink then `off` of paper along it.
void patternLine(ui::Canvas &c, int x0, int y0, int x1, int y1, int on, int off) {
  const int dx = abs(x1 - x0), dy = -abs(y1 - y0);
  const int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
  const bool steep = -dy > dx;
  int err = dx + dy;
  for (int step = 0;; ++step) {
    if (off == 0 || step % (on + off) < on) {
      c.drawPixel(x0, y0, ui::kInk);
      c.drawPixel(steep ? x0 + 1 : x0, steep ? y0 : y0 + 1, ui::kInk);
    }
    if (x0 == x1 && y0 == y1) break;
    const int e2 = 2 * err;
    if (e2 >= dy) {
      err += dy;
      x0 += sx;
    }
    if (e2 <= dx) {
      err += dx;
      y0 += sy;
    }
  }
}

// How a route is drawn on the map: solid over the air, dashed through relays, dotted over the net.
void routePattern(comms::Route r, int &on, int &off) {
  on = 1;
  off = 0;
  if (r == comms::Route::Relayed) {
    on = 4;
    off = 3;
  } else if (r == comms::Route::Net) {
    on = 1;
    off = 4;
  }
}

void patternBox(ui::Canvas &c, const Rect &b, comms::Route route) {
  int on, off;
  routePattern(route, on, off);
  if (off == 0) {
    ui::frame(c, b, 2);
    return;
  }
  patternLine(c, b.x, b.y, b.right() - 1, b.y, on, off);
  patternLine(c, b.x, b.bottom() - 2, b.right() - 1, b.bottom() - 2, on, off);
  patternLine(c, b.x, b.y, b.x, b.bottom() - 1, on, off);
  patternLine(c, b.right() - 2, b.y, b.right() - 2, b.bottom() - 1, on, off);
}

// A name for the map's small boxes: a default name (INKDECK-7F3A) that doesn't fit loses its
// common start rather than the part that tells boards apart.
std::string mapName(const std::string &name, int maxW) {
  static const std::string kDefault = "INKDECK-";
  if (ui::textWidth(kMapText, name) <= maxW) return name;
  if (name.size() > kDefault.size() && name.compare(0, kDefault.size(), kDefault) == 0) {
    return ui::ellipsize(kMapText, name.substr(kDefault.size()), maxW);
  }
  return ui::ellipsize(kMapText, name, maxW);
}

// The boards reached, as a small map: this board inverted on the left, up to four others in
// fixed slots on the right, each joined to it by its route's line (more than four: the last slot
// says how many more).
void drawMap(ui::Canvas &c, const Rect &m, const std::vector<comms::Board> &boards) {
  using namespace ui;
  constexpr int kSlots = 4, kSlotH = 22, kSelfH = 28, kMaxSelfW = 80, kLineGap = 32;
  std::vector<const comms::Board *> reached;
  for (const comms::Board &b : boards) {
    if (b.route != comms::Route::Away) reached.push_back(&b);
  }

  const std::string self = mapName(comms::boardName(), kMaxSelfW - 12);
  const int selfW = textWidth(kMapText, self) + 12;
  const Rect me{m.x, m.y + (m.h - kSelfH) / 2, selfW, kSelfH};
  fill(c, me, kInk);
  drawText(c, me.inset(4, 0), self, kMapText, Align::Center, kPaper);

  const int slotX = me.right() + kLineGap;
  const int slotMaxW = m.right() - slotX;
  const int pitch = (m.h - kSlotH) / (kSlots - 1);
  if (reached.empty()) {
    const int mid = me.centerY();
    drawText(c, {slotX - 8, mid - 19, slotMaxW + 8, 18}, "NOBODY", kMapText, Align::Center);
    drawText(c, {slotX - 8, mid + 1, slotMaxW + 8, 18}, "IN REACH", kMapText, Align::Center);
    return;
  }
  const int n = static_cast<int>(reached.size());
  const int shown = n > kSlots ? kSlots - 1 : n;
  // Fewer than four: centred on this board's height.
  const int slots = n > kSlots ? kSlots : n;
  const int firstY = m.y + (m.h - kSlotH) / 2 - (slots - 1) * pitch / 2;
  for (int k = 0; k < shown; ++k) {
    const comms::Board &b = *reached[k];
    const std::string name = mapName(boardName(b.name, b.id), slotMaxW - 14);
    const int w = textWidth(kMapText, name) + 14;
    const Rect box{slotX, firstY + k * pitch, w, kSlotH};
    int on, off;
    routePattern(b.route, on, off);
    const int fromY = me.y + 6 + (shown > 1 ? k * (kSelfH - 12) / (shown - 1) : (kSelfH - 12) / 2);
    patternLine(c, me.right(), fromY, box.x - 1, box.centerY(), on, off);
    patternBox(c, box, b.route);
    drawText(c, box.inset(5, 0), name, kMapText, Align::Center);
  }
  if (n > shown) {
    const Rect slot{slotX, firstY + shown * pitch, slotMaxW, kSlotH};
    drawText(c, slot, "+" + std::to_string(n - shown) + " MORE", kMapText);
  }
}

// What the map shows, for the screen's record of what it drew.
std::string mapKey(const std::vector<comms::Board> &boards) {
  std::string k = comms::boardName() + ">";
  for (const comms::Board &b : boards) {
    if (b.route == comms::Route::Away) continue;
    k += boardName(b.name, b.id) + std::to_string(static_cast<int>(b.route)) + ",";
  }
  return k;
}

// "─ AIR  ┄ RELAYED  ··· NET": the map's lines, drawn, each before its word.
void drawLegend(ui::Canvas &c, int x, int baseline) {
  using namespace ui;
  const int midY = baseline - capHeight(style::kLabel) / 2 - 1;
  const struct {
    comms::Route route;
    const char *word;
  } items[] = {{comms::Route::Air, "AIR"}, {comms::Route::Relayed, "RELAYED"},
               {comms::Route::Net, "NET"}};
  for (const auto &it : items) {
    int on, off;
    routePattern(it.route, on, off);
    patternLine(c, x, midY, x + 15, midY, on, off);
    x = drawText(c, x + 21, baseline, it.word, style::kLabel) + 14;
  }
}

// --- The Groups sheet ---------------------------------------------------------------------------

class GroupsSheet : public ui::Screen {
 public:
  GroupsSheet() {
    load();
    cursor_.reset(static_cast<int>(groups_.size()), comms::activeGroup());
    drawn_ = key();
  }

  std::string tag() const override { return {}; }  // the Comms screen's chrome
  bool overlay() const override { return true; }
  int hints(ui::Hint *out) const override {
    out[0] = {ui::glyph::kDial, "MOVE"};
    out[1] = {ui::glyph::kPress, "SWITCH"};
    out[2] = {ui::glyph::kHoldPress, "VIA"};
    out[3] = {"BOOT", "CLOSE"};
    return 4;
  }

  bool onKey(Key key) override {
    switch (key) {
      case Key::Up: return cursor_.move(-1, true);
      case Key::Down: return cursor_.move(+1, true);
      case Key::Select:
        comms::setActiveGroup(cursor_.cursor());
        ui::nav::pop();  // the Comms screen comes back on that group
        return true;
      case Key::SelectLong:
        comms::cycleVia(cursor_.cursor());
        load();
        return true;
      default: return false;  // BOOT closes
    }
  }

  bool onTick() override {
    if (comms::generation() == gen_) return false;
    load();
    return key() != drawn_;
  }

  void draw(ui::Canvas &c, const Rect &body) override {
    using namespace ui;
    drawn_ = key();
    constexpr int kFootH = 2 * 20 + 9 + 6;
    const int count = static_cast<int>(groups_.size());
    const int fit = std::max(1, (body.h - kRule - kSectionH - kFootH) / kRowTallH);
    const int rows = std::max(1, std::min(count, fit));
    Rect sheet{body.x, body.bottom() - (kRule + kSectionH + rows * kRowTallH + kFootH), body.w, 0};
    sheet.h = body.bottom() - sheet.y;
    fill(c, sheet, kPaper);
    hrule(c, sheet.x, sheet.y, sheet.w);
    Rect r = sheet;
    r.takeTop(kRule);
    drawSectionHeader(c, r.takeTop(kSectionH), "GROUPS", count ? cursor_.label() : "");
    const Rect foot = r.takeBottom(kFootH);
    drawList(c, r, kRowTallH, cursor_, [this](int i) { return row(i); });
    int y = foot.y + 6 + capHeight(style::kLabel) + 4;
    drawText(c, foot.x + kPadX, y,
             "\xE2\x9C\x93 ON SCREEN" + kDot + "CALLS FROM EVERY GROUP STILL COME IN",
             style::kLabel);
    y += 20;
    drawText(c, foot.x + kPadX, y, "VIA: AIR+NET \xE2\x86\x92 AIR \xE2\x86\x92 NET", style::kLabel);
  }

 private:
  void load() {
    gen_ = comms::generation();
    groups_ = comms::groups();
  }
  ui::Row row(int i) const {
    const comms::Group &g = groups_[i];
    ui::Row row;
    row.title = g.name;
    row.subtitle = groupLine(g);
    row.tag = comms::viaName(g.via);
    if (g.active) row.mark = "\xE2\x9C\x93";  // ✓
    return row;
  }
  std::string key() const {
    std::string k;
    for (size_t i = 0; i < groups_.size(); ++i) {
      const ui::Row r = row(static_cast<int>(i));
      k += r.title + "|" + r.subtitle + "|" + r.tag + (r.mark ? "v" : "") + "\n";
    }
    return k;
  }

  std::vector<comms::Group> groups_;
  ui::ListCursor cursor_;
  uint32_t gen_ = 0;
  std::string drawn_;
};

// --- The Messages sheet -------------------------------------------------------------------------

// The group's messages, newest first: ● plays one (or stops the one playing), BOOT closes (or
// stops). Over the talk screen, grown to fit like the Groups sheet.
class MessagesSheet : public ui::Screen {
 public:
  explicit MessagesSheet(int group) : group_(group) {
    load();
    cursor_.reset(static_cast<int>(msgs_.size()));
  }

  std::string tag() const override { return {}; }  // the Comms screen's chrome
  bool overlay() const override { return true; }
  int hints(ui::Hint *out) const override {
    int n = 0;
    if (!msgs_.empty()) {
      out[n++] = {ui::glyph::kDial, "MOVE"};
      const comms::Message &m = msgs_[cursor_.cursor()];
      if (!m.receiving) out[n++] = {ui::glyph::kPress, m.id == playingId_ ? "STOP" : "PLAY"};
    }
    out[n++] = {"BOOT", playingId_ ? "STOP" : "CLOSE"};
    return n;
  }

  bool onKey(Key key) override {
    switch (key) {
      case Key::Up: return cursor_.move(-1, true);
      case Key::Down: return cursor_.move(+1, true);
      case Key::Select: {
        if (msgs_.empty()) return false;
        const comms::Message &m = msgs_[cursor_.cursor()];
        if (m.receiving) return true;  // still coming
        if (m.id == playingId_) {
          comms::stopPlaying();
          return true;
        }
        if (playing()) comms::stopPlaying();  // the task stops it before it plays this one
        noteAttempt();
        comms::playMessage(m.id);
        return true;
      }
      case Key::Back: return stopIfPlaying();  // else it closes
      default: return false;
    }
  }

  bool onTick() override {
    if (comms::generation() == gen_ && attemptError() == drawnError_) return false;
    load();
    return key() != drawn_;
  }

  void draw(ui::Canvas &c, const Rect &body) override {
    using namespace ui;
    drawn_ = key();
    drawnError_ = attemptError();
    constexpr int kFootH = 2 * 20 + 9 + 6;
    const int count = static_cast<int>(msgs_.size());
    const int fit = std::max(1, (body.h - kRule - kSectionH - kFootH) / kRowTallH);
    const int rows = std::max(1, std::min(count, fit));
    Rect sheet{body.x, body.bottom() - (kRule + kSectionH + rows * kRowTallH + kFootH), body.w, 0};
    sheet.h = body.bottom() - sheet.y;
    fill(c, sheet, kPaper);
    hrule(c, sheet.x, sheet.y, sheet.w);
    Rect r = sheet;
    r.takeTop(kRule);
    const Rect head = r.takeTop(kSectionH);
    if (!drawnError_.empty()) {
      drawNotice(c, head, drawnError_);
    } else {
      drawSectionHeader(c, head, "MESSAGES" + kDot + groupName(group_),
                        count ? cursor_.label() : "");
    }
    const Rect foot = r.takeBottom(kFootH);
    if (msgs_.empty()) {
      drawText(c, {r.x + kPadX, r.y, r.w - 2 * kPadX, r.h}, "NO MESSAGES YET", style::kLabel,
               Align::Center);
    } else {
      drawList(c, r, kRowTallH, cursor_, [this](int i) { return messageRow(msgs_[i]); });
    }
    bool inRam = false;
    for (const comms::Message &m : msgs_) inRam = inRam || !m.onCard;
    int y = foot.y + 6 + capHeight(style::kLabel) + 4;
    drawText(c, foot.x + kPadX, y,
             inRam ? "KEPT IN MEMORY: A POWER-OFF LOSES THEM" : "KEPT UNTIL PLAYED, AND A DAY AFTER",
             style::kLabel);
    y += 20;
    drawText(c, foot.x + kPadX, y, "LEFT FROM 5 S INTO A RING NOBODY ANSWERS", style::kLabel);
  }

 private:
  void load() {
    gen_ = comms::generation();
    const uint32_t was = msgs_.empty() ? 0 : msgs_[cursor_.cursor()].id;
    msgs_ = comms::messages(group_);
    const comms::Status s = comms::status();
    playingId_ = s.playingMessage;
    // The cursor stays on its message as new ones come in on top.
    int at = 0;
    for (size_t i = 0; i < msgs_.size(); ++i) {
      if (msgs_[i].id == was) at = static_cast<int>(i);
    }
    cursor_.reset(static_cast<int>(msgs_.size()), at);
  }

  std::string key() const {
    std::string k = attemptError() + "|" + std::to_string(cursor_.cursor()) + "\n";
    for (const comms::Message &m : msgs_) {
      const ui::Row r = messageRow(m);
      k += r.title + "|" + r.subtitle + "|" + r.tag + "\n";
    }
    return k;
  }

  // KITCHEN · 14:08 over 0:12 · TO ALL, its state boxed: NEW (inverted), PLAYED, COMING,
  // PLAYING (inverted).
  ui::Row messageRow(const comms::Message &m) const {
    ui::Row row;
    row.title = boardName(m.fromName, m.from) + kDot + whenText(m.at, m.agoS);
    row.subtitle = lengthText(m.ms) + kDot + (m.to ? "TO YOU" : "TO ALL");
    if (!m.complete && !m.receiving) row.subtitle += kDot + "PART: SOME OF IT LOST";
    if (m.receiving) {
      row.tag = "COMING";
    } else if (m.id == playingId_) {
      row.tag = "PLAYING";
      row.tagInverted = true;
      row.subtitle = lengthText(m.ms) + kDot + "PLAYING";
    } else if (!m.played) {
      row.tag = "NEW";
      row.tagInverted = true;
    } else {
      row.tag = "PLAYED";
    }
    return row;
  }

  int group_;
  std::vector<comms::Message> msgs_;
  ui::ListCursor cursor_;
  uint32_t playingId_ = 0;
  uint32_t gen_ = 0;
  std::string drawn_;
  std::string drawnError_;
};

// --- The talk screen ----------------------------------------------------------------------------

// What CHANNEL's left column shows: the state in large type (inverted for MIC ON and LOST), two
// lines, and the meter.
struct StateBlock {
  std::string word;
  bool inverted = false;
  std::string line1, line2;
  const char *meterLabel = "IN";
  int meter = 0;
  std::string key() const {
    return word + (inverted ? "*" : "") + "|" + line1 + "|" + line2 + "|" + meterLabel +
           std::to_string(meter);
  }
};

class CommsScreen : public ui::Screen {
 public:
  CommsScreen() : openedMs_(millis()) {
    comms::hold();  // calls come in while it's up, on battery too
    load();
  }
  ~CommsScreen() override { comms::release(); }

  std::string tag() const override { return moduleTag("COMMS"); }
  std::string context() const override {
    if (!comms::configured()) return "NOT SET UP";
    if (groups_.empty()) return {};
    // The canvas adds "· 5 BOARDS", which the status bar's WIFI leaves no room for: ALL OF's
    // row says it.
    const comms::Group &g = groups_[group_];
    return g.name + kDot + g.tag;
  }
  int hints(ui::Hint *out) const override {
    if (!comms::configured()) {
      out[0] = {"BOOT", "BACK"};
      return 1;
    }
    // HOLD BOOT the mic, then ● rings, joins or leaves (four hints at most: the dial's goes).
    // While BOOT keys it: ● latches it. Latched: BOOT (or ●) ends it.
    if (micHeld()) {
      out[0] = micHint();  // ● HOLD
      return 1;
    }
    if (micLatched()) {
      out[0] = micHint();  // BOOT STOP
      out[1] = {ui::glyph::kHoldPress, "MENU"};
      return 2;
    }
    out[0] = micHint();
    out[1] = {ui::glyph::kPress, pressWord()};
    out[2] = {ui::glyph::kHoldPress, "MENU"};
    out[3] = {"BOOT", playing() ? "STOP" : "BACK"};
    return 4;
  }
  // HOLD BOOT keys the mic to the row selected (to a board in the group's conversation it goes to
  // all of it: the engine).
  ui::TalkTo talkTo() const override {
    return groups_.empty() ? ui::TalkTo() : micTalkTo(group_, selected_);
  }

  // Back from a sheet: the Groups sheet may have switched the group.
  void onEnter() override {
    load();
    retarget();
  }

  bool onKey(Key key) override {
    if (!comms::configured()) return false;
    switch (key) {
      case Key::Up:
      case Key::Down: {
        if (!cursor_.move(key == Key::Up ? -1 : 1)) return false;
        const int i = cursor_.cursor();
        selected_ = i == 0 ? 0 : boards_[i - 1].id;
        retarget();
        return true;
      }
      case Key::Select: pressPush(); return true;
      case Key::SelectLong: openMenu(); return true;
      case Key::Back: return stopIfPlaying();  // else back; the mic stays as it is
      default: return false;
    }
  }

  // Each second: the snapshots again, and a redraw only if what the screen shows has changed.
  bool onTick() override {
    if (!comms::configured()) return false;
    load();
    retarget();
    return key() != drawn_;
  }

  void draw(ui::Canvas &c, const Rect &body) override {
    using namespace ui;
    drawn_ = key();
    Rect r = body;
    if (!comms::configured()) {
      drawNotSetUp(c, r);
      return;
    }
    if (groups_.empty()) return;  // the task hasn't loaded them yet
    drawGroupBand(c, r.takeTop(kSectionH));
    drawChannel(c, r.takeTop(kSectionH + kChannelH));

    hrule(c, r.x, r.y, r.w);
    r.takeTop(kRule);
    const int rows = std::clamp(cursor_.count(), 1, kMaxTalkRows);
    Rect talk = r.takeTop(kSectionH + rows * kRowTallH);
    drawSectionHeader(c, talk.takeTop(kSectionH), "TALK TO", cursor_.label());
    drawList(c, talk, kRowTallH, cursor_, [this](int i) { return talkRow(i); });

    hrule(c, r.x, r.y, r.w);
    r.takeTop(kRule);
    drawSectionHeader(c, r.takeTop(kSectionH), "HEARD", countText(calls_));
    if (calls_.empty()) {
      drawText(c, {r.x, r.y + 12, r.w, 24},
               "NOTHING YET" + kDot + "HOLD BOOT TO TALK",
               style::kLabel, Align::Center);
      return;
    }
    for (size_t i = 0; i < calls_.size() && r.h >= kLogRowH; ++i) {
      drawCallRow(c, r.takeTop(kLogRowH), calls_[i], true);
    }
  }

 private:
  // A failed attempt's notice, in place of CHANNEL's header; a sheet over the screen (the
  // ringing sheet, Messages) shows its own.
  std::string notice() const { return ui::nav::top() == this ? attemptError() : std::string(); }

  // HOLD BOOT's target changed (the row, the group): the navigator asks talkTo() again.
  void retarget() {
    const uint64_t k = (static_cast<uint64_t>(static_cast<uint32_t>(group_)) << 32) | selected_;
    if (k == talked_) return;
    talked_ = k;
    ui::nav::talkChanged();
  }

  // ●: rings the row selected while the group has no conversation, else joins it or leaves it
  // (LEAVE also switches this board's mic off).
  const char *pressWord() const { return !conv_.on ? "RING" : conv_.in ? "LEAVE" : "JOIN"; }
  void pressPush() {
    if (conv_.on && conv_.in) {
      comms::leave(group_);
    } else if (conv_.on) {
      if (!comms::join(group_)) noteAttempt();  // over a moment ago
    } else if (comms::ringStart(group_, selected_)) {
      ui::nav::push(makeRingingSheet(group_, selected_));  // it says if the ring won't start
    } else {
      noteAttempt();
    }
  }

  void load() {
    groups_ = comms::groups();
    status_ = comms::status();
    mic_ = comms::mic();
    mics_ = comms::openMics(-1);
    if (groups_.empty()) {
      boards_.clear();
      calls_.clear();
      conv_ = comms::Conversation();
      cursor_.reset(1);
      selected_ = 0;
      block_ = stateBlock();
      return;
    }
    const int active = std::clamp(comms::activeGroup(), 0, static_cast<int>(groups_.size()) - 1);
    if (active != group_) {
      group_ = active;
      selected_ = 0;  // a new group: ALL OF it
    }
    boards_ = comms::boards(group_);
    calls_ = comms::calls(group_, kMaxLog);
    conv_ = comms::conversation(group_);
    // The cursor stays on its board as the list reorders; ALL if it has gone.
    int at = 0;
    for (size_t i = 0; i < boards_.size(); ++i) {
      if (boards_[i].id == selected_) at = static_cast<int>(i) + 1;
    }
    if (at == 0) selected_ = 0;
    cursor_.reset(static_cast<int>(boards_.size()) + 1, at);
    block_ = stateBlock();
    // On screen, the group's calls and rings count as heard (a message once it has played).
    if (groups_[group_].unheard > 0) comms::markHeard(group_);
  }

  // Everything the screen draws, in short: it redraws when this changes.
  std::string key() const {
    std::string k = context() + "\n" + (comms::micEnabled() ? "m" : "") + (playing() ? "p" : "") +
                    pressWord() + micHint().verb;
    for (const comms::Group &g : groups_) k += g.name + std::to_string(g.unheard) + ",";
    if (groups_.empty()) return k;
    k += comms::viaName(groups_[group_].via);
    const std::string n = notice();
    k += "\n" + (n.empty() ? channelText(status_, groups_[group_].via) : "!" + n);
    k += "\n" + block_.key() + "\n" + mapKey(boards_) + "\n" + cursor_.label() + "\n";
    for (int i = 0; i < cursor_.count(); ++i) {
      const ui::Row r = talkRow(i);
      k += r.title + "|" + r.subtitle + "|" + r.tag + (r.tagInverted ? "*" : "") + "\n";
    }
    k += countText(calls_) + "\n";
    for (size_t i = 0; i < calls_.size() && i < kLogKeyRows; ++i) k += callRowKey(calls_[i]);
    return k;
  }

  // The names of the boards whose voice comes out of the speaker now: "KITCHEN + GARAGE".
  std::string playingNames() const {
    std::string s;
    for (const comms::OpenMic &m : mics_) {
      if (!m.playing) continue;
      if (!s.empty()) s += " + ";
      s += boardName(m.name, m.id);
    }
    return s;
  }

  // The conversation, for a line: "WITH KITCHEN + GARAGE" (this board in it), "YOU LEFT ·
  // KITCHEN", "WITHOUT YOU · KITCHEN".
  std::string convLine() const {
    if (!conv_.on) return {};
    comms::Conversation others = conv_;
    others.in = false;
    const std::string names = membersText(others);
    if (conv_.in) return names.empty() ? "CONVERSATION" + kDot + "JUST YOU" : "WITH " + names;
    return (conv_.left ? "YOU LEFT" : "WITHOUT YOU") + kDot + names;
  }

  StateBlock stateBlock() const {
    StateBlock b;
    const comms::Status &s = status_;
    if (s.state == comms::State::Off) {
      b.word = "OFF";
      b.line1 = millis() - openedMs_ < kStartingMs ? "THE RADIO IS STARTING\xE2\x80\xA6"
                                                    : "THE RADIO WON'T START";
      return b;
    }
    const bool mine = comms::micEnabled();
    // What moves (meters, seconds): a press, this board's or one heard, and anything for 30 s after
    // a key; a latched mic (a room) sends all the time, so it moves nothing by itself, or the panel
    // would refresh every second for as long as it's on.
    const bool live = keyedLately();
    bool heldVoice = false;  // a held mic's voice plays here
    for (const comms::OpenMic &m : mics_) heldVoice = heldVoice || (m.playing && !m.latched);
    const bool mineMoves = live || micHeld();
    // This board's mic: where it goes, and for how long (seconds while it moves, whole minutes
    // otherwise).
    std::string to;
    if (mine) {
      const int g = mic_.on ? mic_.group : group_;
      to = mic_.target ? "TO " + boardName(mic_.targetName, mic_.target)
                       : "TO ALL OF " + groupName(g);
      to += kDot + (mineMoves ? lengthText(mic_.ms) : minutesText(mic_.ms));
    }
    const std::string paused =
        mine && !mic_.paused.empty() && mic_.paused != "STARTING"
            ? "PAUSED" + kDot + pausedText(mic_.paused)
            : std::string();
    auto micMeter = [&] {
      b.meterLabel = "MIC";
      b.meter = mic_.capturing && mineMoves ? meterFill(mic_.levelDb) : 0;
    };
    auto playMeter = [&] {
      const bool moves = s.replay || (s.hearing && (heldVoice || live));
      b.meter = moves ? meterFill(s.playLevelDb) : 0;
    };

    // LOST first: a mic heard here that went silent without its off.
    std::string lostNames;
    const comms::OpenMic *lost = nullptr;
    for (const comms::OpenMic &m : mics_) {
      if (m.link != comms::Link::Lost) continue;
      if (!lost) lost = &m;
      lostNames += (lostNames.empty() ? "" : " + ") + boardName(m.name, m.id);
    }
    if (lost) {
      b.word = "LOST";
      b.inverted = true;
      b.line1 = lostNames + kDot + lostText(lost->linkMs);
      b.line2 = mine ? "YOUR MIC ON" + kDot + to
                     : "NOT HEARD" + kDot + "MIC WAS ON " + minutesText(lost->onS * 1000);
      if (mine) {
        micMeter();
      } else {
        playMeter();
      }
      return b;
    }
    const std::string heard = playingNames();
    if (mine) {
      b.word = "MIC ON";
      b.inverted = true;
      if (s.talkedOver) {  // half duplex: another board talks, and isn't heard here
        b.line1 = boardName(s.talkedOverName, s.talkedOverBy) + " TALKING TOO";
        b.line2 = paused.empty() ? to : paused;
      } else if (!heard.empty()) {  // still playing out as the mic went on
        b.line1 = "HEARING " + heard;
        b.line2 = paused.empty() ? to : paused;
      } else {
        b.line1 = to;
        b.line2 = !paused.empty() ? paused
                  : micLatched()  ? "LATCHED" + kDot + "BOOT STOPS IT"
                                  : convLine();
      }
      micMeter();
      return b;
    }
    if (s.hearing || s.replay) {
      b.word = "HEARING";
      if (s.playingMessage) {
        b.line1 = "MESSAGE" + kDot + boardName(s.peerName, s.peer) + kDot + lengthText(s.ms);
      } else if (s.replay && heard.empty()) {
        b.line1 = "REPLAY" + kDot + boardName(s.peerName, s.peer) + kDot + lengthText(s.ms);
      } else {
        uint32_t soundMs = s.ms;  // how long its voice has come
        for (const comms::OpenMic &m : mics_) {
          if (m.playing) {
            soundMs = m.linkMs;
            break;
          }
        }
        b.line1 = heard + kDot + (heldVoice || live ? lengthText(soundMs) : minutesText(soundMs));
      }
      b.line2 = convLine();
      if (b.line2.empty()) b.line2 = reachText(s.peer, s.route, s.hops, s.relayName, s.rssi);
      playMeter();
      return b;
    }
    b.word = "QUIET";
    // Open mics in the group on screen, quiet: whose, and for how long.
    std::vector<const comms::OpenMic *> on;
    for (const comms::OpenMic &m : mics_) {
      if (m.group == group_) on.push_back(&m);
    }
    if (on.size() == 1) {
      b.line1 = boardName(on[0]->name, on[0]->id) + "'S MIC" + kDot +
                minutesText(on[0]->onS * 1000);
    } else if (on.size() > 1) {
      b.line1 = std::to_string(on.size()) + " MICS ON";
      for (const comms::OpenMic *m : on) b.line1 += kDot + boardName(m->name, m->id);
    } else {
      b.line1 = lastLine();
    }
    b.line2 = convLine();
    return b;
  }

  // The last call, ring or message, for the line under QUIET.
  std::string lastLine() const {
    for (const comms::Call &c : calls_) {
      if (c.live) continue;
      const char *what = c.kind == comms::Kind::Ring      ? "RING"
                         : c.kind == comms::Kind::Message ? "MSG"
                                                          : "LAST";
      return what + kDot + (c.outgoing ? std::string("YOU") : boardName(c.fromName, c.from)) +
             kDot + whenText(c.at, c.agoS);
    }
    return "NOTHING YET";
  }

  ui::Row talkRow(int i) const {
    ui::Row row;
    const comms::Group &g = groups_[group_];
    if (i == 0) {
      row.title = "ALL OF " + g.name;
      if (g.boards == 0) {
        row.subtitle = "NO OTHER BOARD HEARD YET";
      } else {
        row.subtitle = plural(g.boards + 1, "BOARD", "BOARDS") + kDot + std::to_string(g.onAir) +
                       " ON AIR";
        if (g.onNet > 0) row.subtitle += kDot + std::to_string(g.onNet) + " ON NET";
        if (g.micsOn > 0) row.subtitle += kDot + plural(g.micsOn, "MIC", "MICS") + " ON";
      }
      row.tag = "ALL";
      return row;
    }
    const comms::Board &b = boards_[i - 1];
    row.title = boardName(b.name, b.id);
    if (const comms::OpenMic *m = findMic(mics_, group_, b.id)) {
      row.subtitle = micLine(*m);
      row.tag = m->link == comms::Link::Lost ? "LOST" : "MIC";
      row.tagInverted = true;
      return row;
    }
    row.subtitle = routeLine(b);
    row.tag = routeTag(b);
    row.tagInverted = (b.flags & comms::kFlagMic) && b.route != comms::Route::Away;
    return row;
  }

  // The board the cursor is on, if its mic is on (the menu's KITCHEN'S MIC).
  const comms::OpenMic *selectedMic() const {
    return selected_ ? findMic(mics_, group_, selected_) : nullptr;
  }

  void openMenu() {
    const int group = group_;
    std::vector<ui::MenuItem> items;
    if (const comms::OpenMic *m = selectedMic()) {
      const uint32_t id = m->id;
      items.push_back({boardName(m->name, m->id) + "'S MIC",
                       [group, id]() -> std::string {
                         const std::vector<comms::OpenMic> ms = comms::openMics(group);
                         const comms::OpenMic *o = findMic(ms, group, id);
                         if (!o) return "OFF";
                         if (o->link == comms::Link::Lost) return "LOST";
                         return "ON" + kDot + minutesText(o->onS * 1000);
                       },
                       [group, id] { ui::nav::replace(makeMicScreen(group, id)); }});
    }
    // ● rings, joins and leaves on the screen itself, and HOLD BOOT (● while held latching)
    // keys the mic.
    items.push_back({"PLAY LAST",
                     []() -> std::string {
                       const comms::LastCall l = comms::lastCall();
                       if (!l.have) return "NONE";
                       return boardName(l.fromName, l.from) + kDot + lengthText(l.ms);
                     },
                     [] {
                       ui::nav::pop();  // the screen shows it playing
                       noteAttempt();
                       comms::playLast();
                     }});
    items.push_back({"MESSAGES",
                     [group]() -> std::string {
                       const std::vector<comms::Message> ms = comms::messages(group);
                       int fresh = 0;
                       for (const comms::Message &m : ms) fresh += !m.played && !m.receiving;
                       if (fresh) return std::to_string(fresh) + " NEW";
                       return ms.empty() ? "NONE" : std::to_string(ms.size()) + " KEPT";
                     },
                     [group] { ui::nav::replace(makeMessagesSheet(group)); }});
    items.push_back({"RELAY FOR OTHERS",
                     []() -> std::string { return comms::relayForOthers() ? "ON" : "OFF"; },
                     [] { comms::setRelayForOthers(!comms::relayForOthers()); }});
    items.push_back({"CALL CHIRP",
                     []() -> std::string { return comms::callChirp() ? "ON" : "OFF"; },
                     [] { comms::setCallChirp(!comms::callChirp()); }});
    items.push_back({"GROUPS",
                     []() -> std::string {
                       const std::vector<comms::Group> gs = comms::groups();
                       const int a = comms::activeGroup();
                       if (a < 0 || a >= static_cast<int>(gs.size())) return {};
                       return gs[a].name + kDot + std::to_string(a + 1) + " OF " +
                              std::to_string(gs.size());
                     },
                     [] { ui::nav::replace(std::make_unique<GroupsSheet>()); }});
    ui::nav::push(std::make_unique<ui::MenuScreen>("COMMS" + kDot + comms::boardName(),
                                                   std::move(items)));
  }

  // The groups' names, the one on screen in a pill, a boxed count after one with calls not
  // heard; on the right VIA and this group's via, boxed.
  void drawGroupBand(ui::Canvas &c, const Rect &r) const {
    using namespace ui;
    const Rect band{r.x + kPadX, r.y, r.w - 2 * kPadX, r.h - kRule};
    hrule(c, r.x, band.bottom(), r.w);
    const int cy = band.centerY();
    constexpr int kH = 19;
    const std::string via = comms::viaName(groups_[group_].via);
    int right = band.right() - tagWidth(via, style::kCap, 5);
    drawTag(c, right, cy, kH, via, style::kCap, TagKind::Outlined, 5);
    right -= 6 + textWidth(style::kSection, "VIA");
    drawText(c, right, baselineCentered(style::kSection, band.y, band.h), "VIA", style::kSection);
    right -= 16;

    const int n = static_cast<int>(groups_.size());
    auto count = [this](int i) {
      return groups_[i].unheard > 0 ? std::to_string(std::min(groups_[i].unheard, 99))
                                    : std::string();
    };
    // Each name 6 px either side (the count box 6 px after it), 6 px between groups. Names are
    // cut to an even share when they don't all fit.
    int fixed = 0, names = 0;
    for (int i = 0; i < n; ++i) {
      const std::string k = count(i);
      fixed += 12 + 6 + (k.empty() ? 0 : tagWidth(k, style::kCap, 4) + 6);
      names += textWidth(i == group_ ? style::kCap : style::kSection, groups_[i].name);
    }
    const int avail = right - band.x;
    const int maxName = fixed + names > avail ? std::max(24, (avail - fixed) / n) : avail;
    int x = band.x;
    for (int i = 0; i < n; ++i) {
      const bool on = i == group_;
      const TextStyle &s = on ? style::kCap : style::kSection;
      const std::string name = ellipsize(s, groups_[i].name, maxName);
      if (on) {
        x += drawTag(c, x, cy, kH, name, s, TagKind::Inverted, 4);
      } else {
        x = drawText(c, x + 6, baselineCentered(s, band.y, band.h), name, s) + 6;
      }
      const std::string k = count(i);
      if (!k.empty()) x += drawTag(c, x + 6, cy, kH, k, style::kCap, TagKind::Outlined, 4) + 6;
      x += 6;
    }
  }

  // CHANNEL: the state (inverted for MIC ON and LOST), two lines under it, the meter (this
  // board's mic while it's on) and the legend on the left; the map right.
  void drawChannel(ui::Canvas &c, Rect r) const {
    using namespace ui;
    const std::string error = notice();
    const Rect head = r.takeTop(kSectionH);
    if (!error.empty()) {
      drawNotice(c, head, error);
    } else {
      drawSectionHeader(c, head, "CHANNEL", channelText(status_, groups_[group_].via));
    }
    constexpr int kMapW = 190, kMapH = 132, kGap = 14;
    const int top = r.y + 12;
    const int x = r.x + kPadX;
    const int w = r.w - 2 * kPadX - kGap - kMapW;
    const StateBlock &b = block_;
    if (b.inverted) {
      fill(c, {r.x, top - 7, kPadX + w + 4, 46}, kInk);
      drawText(c, x, top + 32, b.word, style::kTitle, kPaper);
    } else {
      drawText(c, x, top + 32, b.word, style::kTitle);
    }
    drawText(c, {x, top + 46, w, 17}, b.line1, style::kLabel);
    if (!b.line2.empty()) drawText(c, {x, top + 66, w, 17}, b.line2, style::kLabel);
    drawMicMeter(c, x, top + 89, w, 14, b.meterLabel, b.meter);
    drawLegend(c, x, r.bottom() - 10);
    drawMap(c, {r.right() - kPadX - kMapW, top, kMapW, kMapH}, boards_);
  }

  void drawNotSetUp(ui::Canvas &c, Rect r) const {
    using namespace ui;
    drawNotice(c, r.takeTop(kSectionH), "NO GROUP SET");
    const int x = r.x + 20, w = r.w - 40;
    int y = r.y + 16;
    auto para = [&](const std::string &text, const TextStyle &s, int maxLines) {
      for (const std::string &line : wrap(s, text, w, maxLines)) {
        y += 22;
        drawText(c, x, y, line, s);
      }
      y += 10;
    };
    para("Boards with the same group line hear each other. Add one to SD:/inkdeck.ini on every "
         "board of the group, under [comms]:",
         style::kBody, 4);
    para("mesh_group = a-long-random-secret?name=Family", style::kRowSmall, 2);
    para("and give each board its name:", style::kBody, 2);
    para("mesh_name = KITCHEN", style::kRowSmall, 1);
    para("A board with an agent_token is in a HOME group with the other boards of that token, "
         "without a line. Then restart the board.",
         style::kBody, 4);
  }

  uint32_t openedMs_;
  std::string drawn_;
  std::vector<comms::Group> groups_;
  std::vector<comms::Board> boards_;
  std::vector<comms::Call> calls_;
  std::vector<comms::OpenMic> mics_;
  comms::Status status_;
  comms::Mic mic_;
  comms::Conversation conv_;
  StateBlock block_;
  int group_ = -1;        // the group on screen
  uint32_t selected_ = 0;  // TALK TO's row: a board's id, 0 for all
  ui::ListCursor cursor_;
  uint64_t talked_ = ~0ull;  // HOLD BOOT's target as last told to the navigator (retarget())
};

}  // namespace

std::unique_ptr<ui::Screen> commsui::makeMessagesSheet(int group) {
  return std::make_unique<MessagesSheet>(group);
}

std::unique_ptr<ui::Screen> makeComms() { return std::make_unique<CommsScreen>(); }

}  // namespace apps

#endif  // INKDECK_COMMS
