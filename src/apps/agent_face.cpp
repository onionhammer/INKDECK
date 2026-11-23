#include "apps/agent_face.h"

#include "apps/off_face.h"
#include "features.h"

#if !INKDECK_AGENT

namespace apps {
bool agentFaceWanted() { return false; }
void showAgentFace(bool, bool) {}
bool wakeAgentFace() { return false; }
time_t agentCheckWake() { return 0; }
time_t withAgentCheck(time_t wake) { return wake; }
bool agentWake() { return false; }
bool agentStandbyCheck() { return false; }
void requestAgentFace() {}
void pollAgentFace() {}
}  // namespace apps

#else

#include <Arduino.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <string>

#include "agent_client.h"
#include "alarms.h"
#include "apps/agent.h"
#include "apps/agent_ui.h"
#include "apps/clock.h"
#include "apps/home.h"
#include "apps/lock.h"
#include "apps/news_edition.h"
#include "audio.h"
#include "board_pins.h"
#include "fonts.h"
#include "kv_store.h"
#include "netget.h"
#include "notedoc.h"
#include "pmic.h"
#include "power.h"
#include "rtc.h"
#include "screen.h"
#include "ui/components.h"
#include "ui/nav.h"

namespace apps {
namespace {

using namespace agentui;
using ui::Key;
using ui::Rect;

constexpr int kBandH = 47;              // the foot band, its rule included
constexpr uint32_t kLadderMin[] = {5, 15, 30};
constexpr time_t kFloorS = 5 * 60;      // an RTC check-in no sooner than this
constexpr time_t kFullEveryS = 30 * 60; // a full refresh against ghosting at least this often
constexpr uint32_t kCheckInMs = 15000;
constexpr char kNvs[] = "agentface";
constexpr uint8_t kRecordVersion = 1;

const std::string kDot = ui::glyph::kDot;

std::atomic<bool> faceRequest{false};

// What the face drew, kept so a wake can draw it again exactly as the "old" frame and refresh
// partially.
struct FaceRecord {
  uint8_t version;
  uint8_t flags;  // kLow, kUsb, kMic
  uint32_t checked, next;
  agent::Summary sum;
};
// kMic: live on USB with the comms mic on (it stays on there; issue #66), said in the foot.
constexpr uint8_t kLow = 1, kUsb = 2, kMic = 4;

using power::lowBattery;
using power::pwrHeld;

// The NVS record: the next check (the RTC wake the board armed for the agent), the ladder's
// step, when the last full refresh was, and the face as drawn.
uint32_t loadU32(const char *key) { return kv::loadU32(kNvs, key); }
void saveU32(const char *key, uint32_t v) { kv::saveU32(kNvs, key, v); }
bool loadRecord(FaceRecord &r) { return kv::load(kNvs, "face", r) && r.version == kRecordVersion; }
void saveRecord(const FaceRecord &r) { kv::save(kNvs, "face", r); }

// When the face checks in next, on the ladder (5, 15, 30 minutes) or sooner at check_at, and
// kept for the wake. 0: no check-ins (no agent, a low battery).
time_t nextCheck(time_t now) {
  if (!agent::configured() || lowBattery()) {
    saveU32("next", 0);
    return 0;
  }
  const uint32_t step = std::min<uint32_t>(loadU32("step"), 2);
  time_t at = now + kLadderMin[step] * 60;
  const agent::Glance g = agent::glance();
  if (g.checkAt > static_cast<uint32_t>(now)) {
    at = std::min<time_t>(at, std::max<time_t>(g.checkAt, now + kFloorS));
  }
  saveU32("next", static_cast<uint32_t>(at));
  return at;
}

std::string rightText(const FaceRecord &r) {
  if (r.flags & kLow) return "CHECK-INS PAUSED " + kDot + " BATTERY LOW";
  const std::string mic = r.flags & kMic ? "MIC ON " + kDot + " " : "";
  if (r.flags & kUsb) {
    return mic + (r.checked ? "LIVE " + kDot + " " + clockText(r.checked) : "LIVE");
  }
  std::string out;
  if (r.checked) out = "CHECKED " + clockText(r.checked);
  if (r.next) out += (out.empty() ? "" : " " + kDot + " ") + std::string("NEXT ") + clockText(r.next);
  return out;
}

// --- Drawing ----------------------------------------------------------------------------------

void drawFace(ui::Canvas &c, const FaceRecord &r) {
  using namespace ui;
  const agent::Summary &m = r.sum;
  Rect body{0, 0, c.width(), c.height()};
  drawResumeBand(c, body.takeBottom(kBandH), rightText(r));

  // The head: what this is, and when.
  const time_t now = r.checked ? static_cast<time_t>(r.checked) : time(nullptr);
  tm local;
  localtime_r(&now, &local);
  char date[24];
  strftime(date, sizeof date, "%a %d %b", &local);
  drawSectionHeader(c, body.takeTop(kSectionH), "AGENT", upper(date));

  if (!m.content) {
    // Counts and times only: nothing that says what the sessions are.
    // The value 14 px over the band's bottom: 11 over the cells', which stop at its rule.
    agentui::drawCounts(c, body.takeTop(132), m.asks, m.running, m.planned, true,
                        style::kReadout, 14, 11);
    int y = body.y + 24;
    auto line = [&](const std::string &text, const TextStyle &s) {
      drawText(c, body.x + kPadX, y + capHeight(s), text, s);
      y += lineHeight(s) + 10;
    };
    if (m.asks > 0) line(m.asks == 1 ? "A QUESTION WAITS" : std::to_string(m.asks) + " QUESTIONS WAIT", style::kRowSelected);
    if (m.proposed > 0) line(std::to_string(m.proposed) + " DRAFT" + (m.proposed > 1 ? "S" : "") + " TO START", style::kRow);
    if (m.unread > 0) line(std::to_string(m.unread) + " NOT SEEN YET", style::kRow);
    if (m.nextAt) line("NEXT RUN " + whenText(m.nextAt), style::kRow);
    if (m.asks == 0 && m.running == 0 && m.unread == 0 && m.proposed == 0) {
      line("NOTHING RUNNING", style::kRowSelected);
    }
    paragraph(c, body, y + 8,
              m.locked ? "inkharness keeps the rest behind its PIN."
                       : "The PIN keeps the rest off this face.",
              style::kBody, 2);
    return;
  }

  // The question, and its choices.
  if (m.asks > 0) {
    drawSectionHeader(c, body.takeTop(kSectionH), "NEEDS YOU", std::to_string(m.asks), {true});
    int y = body.y + 8;
    drawText(c, {body.x + kPadX, y, body.w - 2 * kPadX, 18}, upper(m.askTitle), style::kLabel);
    y += 24;
    for (const std::string &l : wrap(style::kReading, upper(m.askQuestion), body.w - 2 * kPadX, 3)) {
      drawText(c, body.x + kPadX, y + capHeight(style::kReading), l, style::kReading);
      y += lineHeight(style::kReading);
    }
    body.takeTop(y + 6 - body.y);
    std::vector<agent::AskOption> opts;
    for (int i = 0; i < m.optionCount; ++i) opts.push_back({m.options[i], "", ""});
    drawChoices(c, body, opts, m.recommend, -1, false);
    body.takeTop(8);
  }
  // What runs, and what finished unseen.
  int running = 0, finishedRows = 0;
  for (int i = 0; i < m.rowCount; ++i) {
    const std::string st = m.rows[i].state;
    (st == "working" || st == "watch" ? running : finishedRows)++;
  }
  auto rows = [&](bool live) {
    for (int i = 0; i < m.rowCount && body.h > 56; ++i) {
      const agent::SummaryRow &row = m.rows[i];
      const std::string st = row.state;
      const bool isLive = st == "working" || st == "watch";
      if (isLive != live) continue;
      const Rect line = body.takeTop(isLive ? 62 : kRowTallH);
      drawText(c, {line.x + kPadX, line.y + 4, line.w - 2 * kPadX - 110, 22}, upper(row.title),
               style::kRowSmallSelected);
      std::string right;
      if (st == "watch") {
        right = row.at ? "NEXT " + clockText(row.at) : "WATCH";
      } else if (st == "working") {
        right = row.at ? "BY " + whenText(row.at)
                       : row.step > 0 && row.steps > 0
                             ? std::to_string(row.step) + "/" + std::to_string(row.steps)
                             : "RUNNING";
      } else {
        right = stateWord(st);
      }
      const bool failed = st == "fail";
      const int tw = tagWidth(right, style::kTag, 5);
      drawTag(c, line.right() - kPadX - tw, line.y + 15, 18, right, style::kTag,
              failed || !isLive ? TagKind::Inverted : TagKind::Outlined, 5);
      if (isLive) {
        const Rect meter{line.x + kPadX, line.y + 32, line.w - 2 * kPadX, 22};
        if (row.sparkN > 0) {
          float values[16];
          for (int k = 0; k < row.sparkN; ++k) {
            values[k] = row.sparkMin + (row.sparkMax - row.sparkMin) * row.spark[k] / 255.0f;
          }
          drawSparkline(c, meter, values, row.sparkN);
        } else if (row.steps > 0) {
          drawSegments(c, meter.inset(0, 5), std::min<int>(row.steps, 20),
                       std::min<int>(std::max(0, row.step - 1), 20));
        } else {
          dither25(c, meter.inset(0, 8));
        }
      }
      dashedH(c, line.x, line.bottom() - 1, line.w);
    }
  };
  if (running > 0 && body.h > kSectionH + 56) {
    drawSectionHeader(c, body.takeTop(kSectionH), "RUNNING", std::to_string(running));
    rows(true);
  }
  if (finishedRows > 0 && body.h > kSectionH + 56) {
    drawSectionHeader(c, body.takeTop(kSectionH), "SINCE YOU LEFT", std::to_string(finishedRows));
    rows(false);
  }
  if (m.asks == 0 && running == 0 && finishedRows == 0 && body.h > 120) {
    drawText(c, body.x + kPadX, body.y + 40 + capHeight(style::kTitle), "NOTHING RUNNING",
             style::kTitle);
    body.takeTop(80);
  }
  // The next schedule.
  if (m.nextAt && body.h >= kSectionH + 30) {
    drawSectionHeader(c, body.takeTop(kSectionH), "NEXT", whenText(m.nextAt));
    drawText(c, {body.x + kPadX, body.y + 6, body.w - 2 * kPadX, 24},
             m.nextTitle[0] ? upper(m.nextTitle) : "A SCHEDULE", style::kRow);
  }
}

FaceRecord recordNow(bool usb, uint32_t next) {
  FaceRecord r = {};
  r.version = kRecordVersion;
  r.flags = static_cast<uint8_t>((lowBattery() ? kLow : 0) | (usb ? kUsb : 0) |
                                 (usb && ui::nav::micMark() != ui::MicMark::None ? kMic : 0));
  r.checked = agent::checkedAt();
  r.next = usb ? 0 : next;
  r.sum = agent::summary();
  return r;
}

bool sameDrawing(const FaceRecord &a, const FaceRecord &b) {
  return a.flags == b.flags && rightText(a) == rightText(b) &&
         memcmp(&a.sum, &b.sum, sizeof a.sum) == 0;
}

// --- The face, awake --------------------------------------------------------------------------

class AgentFaceScreen;
AgentFaceScreen *face = nullptr;

class AgentFaceScreen : public OffFaceScreen {
 public:
  AgentFaceScreen(bool usb, bool devWake) : usb_(usb), devWake_(devWake) {
    face = this;
    plan();
  }
  ~AgentFaceScreen() override {
    if (face == this) face = nullptr;
  }

  std::string tag() const override { return "AGENT FACE"; }
  bool chirps() const override { return true; }

  bool onTick() override {
    if (usb_) {
      const pmic::Status p = pmic::status();
      if (p.ok && !p.vbus) {  // unplugged: off behind it, as on battery
        usb_ = false;
        plan();
        return true;
      }
      // Live: redraw as the list changes, a full refresh now and then.
      const FaceRecord now = recordNow(true, 0);
      if (sameDrawing(now, drawn_)) return false;
      if (time(nullptr) - lastFull_ > kFullEveryS) ui::nav::invalidate(ui::Refresh::Full);
      return true;
    }
    // On battery: the second tick after it's drawn, off.
    return offTick();
  }

  void powerOff() override {
    saveRecord(drawn_);
    saveU32("full", static_cast<uint32_t>(time(nullptr)));
    const time_t now = time(nullptr);
    const time_t alarm = rtc::timeValid() ? alarms::next(now) : 0;
    const time_t wake = devWake_ ? now + 60 : wakeAt_;
    if (devWake_) {
      // The dev tool's: one check-in, then the board stays on, so the USB console comes back.
      saveU32("next", static_cast<uint32_t>(wake));
      saveU32("dev", 1);
    }
    power::off(power::Mode::Agent, power::earliest(alarm, wake));
  }

  void draw(ui::Canvas &c, const Rect &) override {
    drawn_ = recordNow(usb_, static_cast<uint32_t>(wakeAt_));
    if (devWake_ && !usb_) drawn_.next = static_cast<uint32_t>(time(nullptr) + 60);
    drawFace(c, drawn_);
    drawn();
    if (lastFull_ == 0) lastFull_ = time(nullptr);
  }

 private:
  void plan() {
    wakeAt_ = usb_ ? 0 : nextCheck(time(nullptr));
    restartTicks();
  }

  bool usb_;
  bool devWake_;
  time_t wakeAt_ = 0;
  FaceRecord drawn_ = {};
  time_t lastFull_ = 0;
};

// --- Check-ins from power-off ------------------------------------------------------------------


// The result a schedule asked to show, on the panel: a page of it as a note, under a band.
void showNoteNow(const std::string &title, const std::string &markdown) {
  using namespace ui;
  NoteDoc doc;
  doc.parse(title, markdown);
  Canvas &c = screen::canvas();
  screen::lock();
  screen::setLandscape(false);
  screen::clear();
  const int pad = kPadX + 6;
  doc.layout(::fonts::kReader[1], c.width() - 2 * pad, c.height() - kBandH - 2 * pad);
  doc.drawPage(c, 0, pad, pad);
  drawResumeBand(c, {0, c.height() - kBandH, c.width(), kBandH},
                 doc.pageCount() > 1 ? "1/" + std::to_string(doc.pageCount()) : std::string());
  screen::present(true);
  screen::unlock();
}

// After a check-in: a schedule's run to show as a note (WAKE THE BOARD). True if shown.
bool showWokenNote(uint32_t seenDone) {
  const agent::Glance g = agent::glance();
  if (g.noteId.empty() || g.latestDone <= seenDone) return false;
  const agent::Sessions a = agent::sessions();
  if (!a.hasOpen || a.open.id != g.noteId || a.open.result.empty()) return false;
  log_i("agent face: showing %s's run as a note", g.noteId.c_str());
  showNoteNow(a.open.title, a.open.result);
  return true;
}

// Joins and syncs (15 s at most, PWR ends it). True if a list came.
bool checkIn() {
  pinMode(board::PWR_KEY_SENSE, INPUT);
  net::begin();
  return agent::checkInNow(kCheckInMs, pwrHeld);
}

}  // namespace

bool agentFaceWanted() {
  if (!agent::configured()) return false;
  // An Agent screen open, under a sheet or menu too.
  const ui::Screen *base = ui::nav::base();
  if (base && base->tag() == moduleTag("AGENT")) return true;
  const agent::Glance g = agent::glance();
  return g.known && (g.asks > 0 || g.running > 0);
}

void showAgentFace(bool usb, bool devWake) {
  const ui::Screen *top = ui::nav::top();
  if ((top && top->offFace()) || deskShowing() || face) return;
  if (audio::status().state == audio::State::Playing) audio::togglePause();  // "off" is quiet
  lockNow();  // under the face: waking it lands on the lock (power_on)
  ui::nav::push(std::make_unique<AgentFaceScreen>(usb, devWake));
  ui::nav::invalidate(ui::Refresh::Full);
}

bool wakeAgentFace() {
  if (!face || ui::nav::base() != face) return false;
  while (ui::nav::top() != face) ui::nav::pop();
  ui::nav::pop();
  return true;
}

time_t withAgentCheck(time_t wake) { return power::earliest(wake, agentCheckWake()); }

time_t agentCheckWake() {
  const time_t now = time(nullptr);
  if (!agent::configured() || lowBattery() || !rtc::timeValid()) {
    saveU32("next", 0);
    return 0;
  }
  const agent::Glance g = agent::glance();
  const time_t at = g.checkAt > static_cast<uint32_t>(now)
                        ? std::max<time_t>(g.checkAt, now + kFloorS)
                        : 0;
  saveU32("next", static_cast<uint32_t>(at));
  return at;
}

bool agentWake() {
  if (power::wake() != power::Wake::Alarm || !rtc::timeValid()) return false;
  const power::Mode mode = power::lastMode();
  if (mode != power::Mode::Agent && mode != power::Mode::Sleep && mode != power::Mode::Book &&
      mode != power::Mode::Edition) {
    return false;
  }
  const time_t now = time(nullptr);
  const time_t ring = alarms::nextAtWake(now);
  if (ring && ring <= now + 1) return false;  // an alarm: boot, and it rings
  const uint32_t due = loadU32("next");
  if (due == 0 || now + 30 < static_cast<time_t>(due)) {
    // Woken for something else (an alarm snoozed or deleted since, the edition): Sleep and a
    // book boot as they always have, the edition's wake draws it (apps/news_edition.h); the face
    // goes back off.
    if (mode != power::Mode::Agent) return false;
  }
  if (lowBattery()) {
    if (mode == power::Mode::Agent) {
      FaceRecord r = {};
      loadRecord(r);
      r.flags |= kLow;
      r.next = 0;
      screen::lock();
      screen::setLandscape(false);
      screen::clear();
      drawFace(screen::canvas(), r);
      screen::present(true);
      screen::unlock();
      saveRecord(r);
    }
    saveU32("next", 0);
    power::off(mode, ring);
    return false;
  }
  uint32_t seenAsk = 0, seenDone = 0;
  agent::seen(seenAsk, seenDone);
  const bool ok = checkIn();
  if (pwrHeld()) return false;  // PWR during it: a normal boot (it finishes in the background)
  const agent::Glance g = agent::glance();
  if (ok && showWokenNote(seenDone)) {
    agent::markSeen(std::max(seenAsk, g.latestAsk), std::max(seenDone, g.latestDone));
    saveU32("next", 0);
    power::off(power::Mode::Sleep, ring);
    return false;
  }
  if (mode != power::Mode::Agent) {
    // Sleep, a book and the edition: no chirp, no redraw (the result waits on Home). The
    // edition keeps its own wake.
    agent::markSeen(std::max(seenAsk, g.latestAsk), std::max(seenDone, g.latestDone));
    const time_t edition = mode == power::Mode::Edition ? editionWakeAt() : 0;
    power::off(mode, power::earliest(withAgentCheck(ring), edition));
    return false;
  }
  // The face: a chirp for what's new, a redraw for what changed, the ladder for the next.
  const bool newAsk = g.latestAsk > seenAsk, newDone = g.latestDone > seenDone;
  if ((newAsk || newDone) && seenAsk + seenDone > 0) {
    audio::begin();
    audio::chirp(newAsk);
    delay(800);  // it plays out before power::off() stops the audio
  }
  agent::markSeen(std::max(seenAsk, g.latestAsk), std::max(seenDone, g.latestDone));
  FaceRecord old = {};
  const bool haveOld = loadRecord(old);
  const agent::Summary now_sum = agent::summary();
  const bool newsChanged = !haveOld || memcmp(&old.sum, &now_sum, sizeof now_sum) != 0;
  uint32_t step = std::min<uint32_t>(loadU32("step"), 2);
  step = newsChanged ? 0 : std::min<uint32_t>(step + 1, 2);
  saveU32("step", step);
  const time_t next = nextCheck(now);
  FaceRecord fresh = recordNow(false, static_cast<uint32_t>(next));
  if (!ok) fresh.checked = old.checked;  // not checked this time: say when it last was
  if (!haveOld || !sameDrawing(old, fresh)) {
    const bool full = !haveOld || now - static_cast<time_t>(loadU32("full")) > kFullEveryS;
    ui::Canvas &c = screen::canvas();
    screen::presentOver(false, full, [&] { drawFace(c, old); }, [&] { drawFace(c, fresh); },
                        pmic::status().vbus);
    if (full) saveU32("full", static_cast<uint32_t>(now));
    saveRecord(fresh);
  }
  log_i("agent face: checked in (%s), next %ld", ok ? "a list" : "nothing", static_cast<long>(next));
  if (loadU32("dev")) {  // the dev tool's check-in: stay on now
    saveU32("dev", 0);
    log_i("agent face: dev wake done, staying on");
    return false;
  }
  power::off(power::Mode::Agent, power::earliest(ring, next));
  return false;
}

bool agentStandbyCheck() {
  const uint32_t due = loadU32("next");
  const time_t now = time(nullptr);
  if (due == 0 || now + 30 < static_cast<time_t>(due) || lowBattery()) return false;
  uint32_t seenAsk = 0, seenDone = 0;
  agent::seen(seenAsk, seenDone);
  const bool ok = checkIn();
  const agent::Glance g = agent::glance();
  const bool shown = ok && showWokenNote(seenDone);
  agent::markSeen(std::max(seenAsk, g.latestAsk), std::max(seenDone, g.latestDone));
  agentCheckWake();  // the next one, if the daemon wants one
  return shown;
}

void requestAgentFace() { faceRequest = true; }

void pollAgentFace() {
  if (faceRequest.exchange(false)) showAgentFace(false, true);
}

}  // namespace apps

#endif  // INKDECK_AGENT
