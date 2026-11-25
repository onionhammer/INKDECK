// The agent away from its screens (issue #52, "Asking you to choose" and "Chirps"): the ask
// sheet that rises over the screen in use once per question, the chirps, and checking in when a
// key comes while ?n shows (not while a book is read on battery, where check-ins are kept to
// one every 30 minutes: issue #72). apps::pollAgent() runs it from loop().
//
// The ask sheet: half the body, like the agent sheet. Not over the lock, an off face or the
// desk face, another sheet or menu, the Recorder or a ringing alarm, the Agent's own screens,
// nor within a few seconds of a key. Unlocked, and with no PIN wheels in front of the Agent,
// it shows the question and its choices:
//   ▲▼ choose   ● send   HOLD BOOT answer in words   BOOT later (back in an hour)
// Otherwise it only says an agent needs you: ● opens the Agent (behind the wheels), BOOT later.
// Left alone two minutes it goes, as BOOT does.
#include "features.h"

#if INKDECK_AGENT  // the whole file (features.h)

#include <Arduino.h>
#include <time.h>

#include <algorithm>
#include <string>

#include "agent_client.h"
#include "apps/agent.h"
#include "apps/agent_sheet.h"
#include "apps/agent_ui.h"
#include "apps/clock.h"
#include "apps/home.h"
#include "apps/lock.h"
#include "audio.h"
#include "input.h"
#include "pmic.h"
#include "text.h"
#include "ui/components.h"

namespace apps {
namespace {

using namespace agentui;
using ui::Key;
using ui::Rect;

constexpr int kSheetMinH = 400;
constexpr uint32_t kSnoozeS = 3600;        // BOOT: the same question again after an hour
constexpr uint32_t kQuietKeyMs = 3000;     // not straight after a key: it would take the next
constexpr uint32_t kSheetIdleMs = 120000;  // untouched this long: later, as BOOT
constexpr uint32_t kSentCloseMs = 1500;

const std::string kDot = ui::glyph::kDot;

std::string raisedKey;  // the question the sheet last rose for
std::string snoozedKey;
uint32_t snoozedUntil = 0;  // Unix seconds

// Which question: by id and when it was asked (asked again is a new one). A locked list says
// only when.
std::string keyOf(const agent::Glance &g) {
  if (g.locked || g.askId.empty()) return "@" + std::to_string(g.latestAsk);
  return g.askSession + "/" + g.askId + "@" + std::to_string(g.askAt);
}

class AskSheet : public ui::Screen {
 public:
  AskSheet(std::string key, bool open) : key_(std::move(key)), open_(open) {
    const agent::Sessions a = agent::sessions();
    if (open_ && a.hasAsk) {
      ask_ = a.ask;
      cursor_ = std::max(0, std::min(ask_.recommend, static_cast<int>(ask_.options.size()) - 1));
    }
    open_ = open_ && !ask_.options.empty();
    since_ = millis();
  }

  bool overlay() const override { return true; }
  std::string tag() const override { return {}; }
  // Words said over it answer it (unless the PIN keeps it closed).
  ui::TalkTo talkTo() const override {
    return open_ ? ui::TalkTo{true, ask_.session} : ui::TalkTo{};
  }

  int hints(ui::Hint *out) const override {
    int n = 0;
    if (open_) {
      out[n++] = {ui::glyph::kDial, "CHOOSE"};
      out[n++] = {ui::glyph::kPress, "SEND"};
      out[n++] = {"HOLD BOOT", "SAY"};
    } else {
      out[n++] = {ui::glyph::kPress, "OPEN"};
    }
    out[n++] = {"BOOT", "LATER"};
    return n;
  }

  bool onKey(Key key) override {
    since_ = millis();
    switch (key) {
      case Key::Up:
      case Key::Down: {
        if (!open_ || sent_) return true;
        const int n = static_cast<int>(ask_.options.size());
        cursor_ = (cursor_ + (key == Key::Down ? 1 : n - 1)) % n;
        return true;
      }
      case Key::Select:
        if (open_) {
          if (!sent_) {
            agent::answer(ask_.session, ask_.id, cursor_);
            sent_ = true;
            sentMs_ = millis();
          }
        } else {
          const std::string id = ask_.session;
          const std::string title = ask_.title;
          ui::nav::pop();
          openGuarded(Guarded::Agent, [id, title]() -> std::unique_ptr<ui::Screen> {
            return id.empty() ? makeAgent() : makeAgentSession(id, title);
          });
        }
        return true;
      case Key::Back:
        later();
        return true;
      default: return false;
    }
  }

  bool onTick() override {
    if (sent_) {
      if (millis() - sentMs_ > kSentCloseMs) ui::nav::pop();
      return false;
    }
    const agent::Glance g = agent::glance();
    if (g.asks <= 0 || keyOf(g) != key_) {  // answered elsewhere, or a newer question
      ui::nav::pop();
      return false;
    }
    if (millis() - since_ > kSheetIdleMs) {
      later();
      return false;
    }
    return false;
  }

  void draw(ui::Canvas &c, const Rect &body) override {
    using namespace ui;
    // Over a screen without chrome, the sheet shows its keys itself.
    Hint keys[kMaxHints];
    const Screen *under = nav::base();
    const int keyCount = under && !under->chrome() ? hints(keys) : 0;
    Rect r = drawSheet(c, body, kSheetMinH, keys, keyCount);
    const agent::Glance g = agent::glance();
    const std::string count = g.asks > 1 ? std::to_string(g.asks) + " WAITING" : std::string();
    if (!open_) {
      drawSectionHeader(c, r.takeTop(kSectionH), "AN AGENT NEEDS YOU", count, {true});
      int y = r.y + 24;
      drawText(c, r.x + kPadX, y + capHeight(style::kReadout), "?" + std::to_string(g.asks),
               style::kReadout);
      y += lineHeight(style::kReadout);
      paragraph(c, r, y + 8,
                g.locked ? "inkharness wants its PIN before it says more. Press to open the Agent."
                         : "Press to open the Agent with your PIN and see the question.",
                style::kBody, 3);
      return;
    }
    drawSectionHeader(c, r.takeTop(kSectionH), "AN AGENT ASKS", count, {true});
    int y = r.y + 10;
    drawText(c, {r.x + kPadX, y, r.w - 2 * kPadX, 18}, upper(ask_.title), style::kLabel);
    y += 24;
    y = drawQuestion(c, r, y, ask_.question, 3);
    r.takeTop(y + 8 - r.y);
    const Rect foot = r.takeBottom(kSectionH);
    hrule(c, r.x, r.y, r.w);
    r.takeTop(kRule);
    drawChoices(c, r, ask_.options, ask_.recommend, cursor_, false);
    if (sent_) {
      drawSentBand(c, foot, ask_.options[cursor_].title);
      return;
    }
    dashedH(c, foot.x, foot.y, foot.w);
    const std::string deadline = deadlineText(ask_);
    drawText(c, {foot.x + kPadX, foot.y, foot.w - 2 * kPadX, foot.h},
             deadline.empty() ? "HOLD BOOT: SOMETHING ELSE" : deadline, style::kLabel);
  }

 private:
  void later() {
    snoozedKey = key_;
    snoozedUntil = static_cast<uint32_t>(time(nullptr)) + kSnoozeS;
    ui::nav::pop();
  }

  std::string key_;
  bool open_;
  agent::Ask ask_;
  int cursor_ = 0;
  bool sent_ = false;
  uint32_t sentMs_ = 0;
  uint32_t since_ = 0;  // the last key, or when it rose
};

// Chirps are for an awake board in use: not over the Recorder or a ringing alarm, not while
// anything plays, records or listens, not over an off face (those are powered off, but for the
// Agent face, apps/agent_face.h).
bool chirpAllowed() {
  const ui::Screen *top = ui::nav::top();
  const ui::Screen *base = ui::nav::base();
  if (!top || !base) return false;
  if (audio::ringing() || audio::recording() || audio::playing() || audio::capturing() ||
      audio::streaming()) {
    return false;
  }
  if (top->offFace() && !top->chirps()) return false;
  return top->allowsTalk() && base->allowsTalk();
}

void chirps(const agent::Glance &g) {
  uint32_t ask = 0, done = 0;
  agent::seen(ask, done);
  if (ask == 0 && done == 0) {
    // Never heard before (a new board, or one just updated): what is there now is old news.
    agent::markSeen(std::max<uint32_t>(g.latestAsk, 1), std::max<uint32_t>(g.latestDone, 1));
    return;
  }
  const bool newAsk = g.latestAsk > ask;
  const bool newDone = g.latestDone > done;
  if (!newAsk && !newDone) return;
  const bool allowed = chirpAllowed();
  if (allowed) audio::chirp(newAsk);  // a question first: it needs you
  agent::markSeen(std::max(ask, g.latestAsk), std::max(done, g.latestDone));
  // A schedule's run to say aloud, on USB (the desk, at home): the agent sheet plays it.
  const pmic::Status p = pmic::status();
  if (newDone && !newAsk && allowed && !g.sayId.empty() && p.ok && p.vbus && g.live &&
      !ui::nav::locked() && !agentSheetOnTop()) {
    const ui::Screen *top = ui::nav::top();
    if (top && !top->overlay()) {
      raiseAgentSheet();
      agent::speak(g.sayId);
    }
  }
}

void raise(const agent::Glance &g) {
  if (g.asks <= 0) return;
  const std::string key = keyOf(g);
  if (key == raisedKey &&
      (snoozedKey != key || static_cast<uint32_t>(time(nullptr)) < snoozedUntil)) {
    return;  // risen for already (and not snoozed, or not yet back)
  }
  ui::Screen *top = ui::nav::top();
  if (!top || ui::nav::locked() || top->offFace() || top->overlay() || !top->allowsTalk()) return;
  if (deskShowing() || agentSheetOnTop()) return;
  if (top->tag() == moduleTag("AGENT")) return;  // the Agent shows it already
  if (input::idleMs() < kQuietKeyMs) return;
  const bool open = !g.locked && !g.askId.empty() && !guardWouldAsk(Guarded::Agent);
  raisedKey = key;
  snoozedKey.clear();
  ui::nav::push(std::make_unique<AskSheet>(key, open));
  ui::nav::flush();  // now, even over a Reader about to nap again
}

// A reading screen is up and the board isn't on USB: it naps between keys (apps/idle.h), and
// each check-in is its radio on for seconds, over the tailnet for longer (issue #72).
bool readingOnBattery() {
  const ui::Screen *base = ui::nav::base();
  if (!base || !base->reading()) return false;
  const pmic::Status p = pmic::status();
  return !(p.ok && p.vbus);
}

}  // namespace

void pollAgent() {
  // The client checks in every 30 minutes then, whatever its ladder says.
  const bool reading = readingOnBattery();
  agent::setReadingOnBattery(reading);
  const agent::Glance g = agent::glance();
  if (!g.known) return;
  chirps(g);
  // A key while ?n shows: look now (the client keeps it to once a minute). Not from a page
  // turn: with a question waiting, reading would join Wi-Fi every minute.
  if (g.asks > 0 && input::idleMs() < 1500 && !reading) agent::checkSoon();
  raise(g);
}

}  // namespace apps

#endif  // INKDECK_AGENT
