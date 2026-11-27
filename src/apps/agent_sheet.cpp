#include "features.h"

#if INKDECK_AGENT  // the whole file (features.h)

#include "apps/agent_sheet.h"

#include <Arduino.h>

#include <algorithm>
#include <string>

#include "apps/agent_ui.h"
#include "audio.h"
#include "input.h"
#include "settings.h"
#include "agent_client.h"

namespace apps {
namespace {

using ui::Key;
using ui::Rect;

constexpr int kMeterSegments = 20;   // -60..0 dBFS, 3 dB each

// The sheet: half the body, but never so short that the reply gets no room (landscape).
constexpr int kSheetMinH = 300;
constexpr int kSheetStateH = 52;  // the state word beside the mic meter
constexpr int kSheetHeardLines = 2;
constexpr uint32_t kSheetCloseMs = 30000;  // after the last reply, and as long since a key
constexpr uint32_t kSentCloseMs = 3000;    // after words went to a session with no reply

// The sheet while on top: HOLD BOOT there talks without opening another.
const ui::Screen *onTop = nullptr;

const char *stateWord(agent::State s) {
  switch (s) {
    case agent::State::Off: return "OFF";
    case agent::State::Connecting: return "CONNECTING";
    case agent::State::Ready: return "READY";
    case agent::State::Listening: return "LISTENING";
    case agent::State::Thinking: return "THINKING";
    case agent::State::Speaking: return "SPEAKING";
    case agent::State::Error: return "OFFLINE";
  }
  return "";
}

const char *notSetUp() {
  return settings::hasAgent() ? "NO WI-FI NETWORK SET" : "NO AGENT HOST SET";
}


int meterFill(const agent::Status &s) {
  return s.mic ? std::clamp(static_cast<int>((s.levelDb + 60) / 3), 0, kMeterSegments) : 0;
}

// Word-wrapped body text from the top of `area`.
void paragraph(ui::Canvas &c, const Rect &area, const std::string &text, int maxLines) {
  agentui::paragraph(c, area, area.y + 12, text, ui::style::kBody, maxLines);
}

// The agent sheet (HOLD BOOT): an overlay over the lower half of whatever is on screen. The hold
// that opens it has already started the session (apps/hold_boot.cpp); closing it ends it.
std::string titleOf(const agent::Sessions &a, const std::string &session) {
  for (const agent::Row &row : a.rows) {
    if (row.id == session && !row.title.empty()) return row.title;
  }
  return "the agent";
}

// Words for a herdr agent this turn made, waiting for ● (an empty id: none).
agent::Draft draftFor(const agent::Status &s) {
  const agent::Sessions a = agent::sessions();
  return a.draft.turn == s.turn && !s.mic ? a.draft : agent::Draft{};
}

// An agent turn that ended without a spoken reply (inkharness's [agent] reply = "none"): the
// words went straight to a session. Its title then, or "" while that isn't so.
std::string sentTo(const agent::Status &s) {
  if (!s.agentTurn || !s.reply.empty() || s.mic || s.state != agent::State::Ready) return {};
  const agent::Sessions a = agent::sessions();
  if (a.started.empty() || a.startedTurn != s.turn || a.draft.turn == s.turn) return {};
  return titleOf(a, a.started);
}

class AgentSheet : public ui::Screen {
 public:
  std::string tag() const override { return {}; }  // the screen below supplies the chrome
  bool overlay() const override { return true; }

  int hints(ui::Hint *out) const override {
    int n = 0;
    if (!draftFor(agent::status()).id.empty()) {
      out[n++] = {ui::glyph::kPress, "TYPE IT"};
      out[n++] = {"HOLD BOOT", "SAY AGAIN"};
      out[n++] = {"BOOT", "DROP"};
      return n;
    }
    if (agent::configured()) {
      const agent::State s = agent::status().state;
      const bool replying = s == agent::State::Thinking || s == agent::State::Speaking;
      out[n++] = {"HOLD BOOT", replying ? "INTERRUPT" : "TALK"};
      out[n++] = {ui::glyph::kDial, "VOL"};
    }
    out[n++] = {"BOOT", "CLOSE"};
    return n;
  }

  void onEnter() override { onTop = this; }

  void onLeave() override {
    onTop = nullptr;
    agent::dropDraft();  // words not typed by now never will be (does nothing without one)
    agent::close();  // covered (the lock, an alarm, PWR) or closed: either way, done
  }

  bool onKey(Key key) override {
    const agent::Status s = agent::status();
    const agent::Draft draft = draftFor(s);
    if (!draft.id.empty()) {
      switch (key) {
        case Key::Select:
          typedInto_ = titleOf(agent::sessions(), draft.session);
          refusalsAtSend_ = agent::sessions().refusals;
          agent::sendDraft();
          return true;
        case Key::Back: agent::dropDraft(); return false;  // and the navigator pops
        default: return true;
      }
    }
    switch (key) {
      case Key::Up: audio::changeVolume(+5); return true;
      case Key::Down: audio::changeVolume(-5); return true;
      default: return false;  // BOOT closes (the navigator pops)
    }
  }

  bool onTick() override {
    const agent::Status s = agent::status();
    const uint32_t now = millis();
    if (s.generation != seen_ || s.mic || agent::busy()) {
      seen_ = s.generation;
      activeMs_ = now;
    }
    if (now - activeMs_ > kSheetCloseMs && input::idleMs() > kSheetCloseMs) {
      ui::nav::pop();
      return false;
    }
    // Talking again replaces the words: a draft from an earlier utterance is dropped, and what
    // was typed from here is old news.
    if (s.turn != turn_) {
      turn_ = s.turn;
      typedInto_.clear();
      notTyped_.clear();
      const agent::Sessions a = agent::sessions();
      if (!a.draft.id.empty() && a.draft.turn != s.turn) agent::dropDraft();
    }
    // inkharness wouldn't type the draft (the program got busy, it ran out): say why, and stay.
    if (!typedInto_.empty()) {
      const agent::Sessions a = agent::sessions();
      if (a.refusals != refusalsAtSend_ && a.refusalRequest == "send_draft") {
        notTyped_ = "Not typed: " + a.refusal + ". HOLD BOOT to say it again.";
        typedInto_.clear();
      }
    }
    // Words that went straight to a session (or were typed from here): nothing to listen to, so
    // the sheet goes once it has said where they went, back to the Agent with the session on it.
    const std::string to = typedInto_.empty() ? sentTo(s) : typedInto_;
    if (to.empty()) {
      sentMs_ = 0;
    } else if (sentMs_ == 0) {
      sentMs_ = now;
    } else if (now - sentMs_ > kSentCloseMs) {
      ui::nav::pop();
      return false;
    }
    return s.generation != drawn_ || s.mic || to != drawnSent_ || notTyped_ != drawnNotTyped_;
  }

  void draw(ui::Canvas &c, const Rect &body) override {
    using namespace ui;
    const agent::Status s = agent::status();
    drawn_ = s.generation;

    // Over a screen without chrome (the desk face), the sheet shows its keys itself.
    Hint keys[kMaxHints];
    const Screen *under = nav::base();
    const int keyCount = under && !under->chrome() ? hints(keys) : 0;
    Rect r = drawSheet(c, body, kSheetMinH, keys, keyCount);

    const Rect head = r.takeTop(kSectionH);
    if (!agent::configured()) {
      drawNotice(c, head, notSetUp());
      paragraph(c, r, agentui::kSetUpHelp, std::max(1, (r.h - 20) / lineHeight(style::kBody)));
      return;
    }
    if (!s.error.empty()) {
      drawNotice(c, head, upper(s.error));
    } else {
      const std::string right = s.state == agent::State::Connecting
                                    ? "CONNECTING"
                                    : "VOL " + std::to_string(audio::status().volume);
      drawSectionHeader(c, head, s.agentTurn ? "AGENT" : "ASSISTANT", right, {s.mic});
    }

    // The state word, and beside it the mic's level, or what the reply waits on (the tool call
    // the daemon is making: its `status`).
    const Rect row = r.takeTop(kSheetStateH);
    const std::string word = s.mic ? "LISTENING" : stateWord(s.state);
    const int wordW = textWidth(style::kReading, "CONNECTING") + 24;
    drawText(c, row.x + kPadX, baselineCentered(style::kReading, row.y, row.h), word,
             style::kReading);
    const Rect meter{row.x + kPadX + wordW, row.y + (row.h - 18) / 2,
                     row.w - 2 * kPadX - wordW, 18};
    if (!s.mic && !s.doing.empty()) {
      drawText(c, Rect{meter.x, row.y, meter.w, row.h}, s.doing + ui::glyph::kEllipsis,
               style::kBody);
    } else {
      drawSegments(c, meter, kMeterSegments, meterFill(s));
    }

    drawSectionHeader(c, r.takeTop(kSectionH), "YOU");
    const Rect heard = r.takeTop(12 + kSheetHeardLines * lineHeight(style::kBody) + 6);
    paragraph(c, heard, s.heard.empty() ? (s.mic ? ui::glyph::kEllipsis : "") : s.heard,
              kSheetHeardLines);

    const agent::Draft draft = draftFor(s);
    if (!draft.id.empty()) {
      // Words for a herdr agent's terminal: shown, and typed only on ●.
      drawSectionHeader(c, r.takeTop(kSectionH), "TYPE IT?",
                        upper(titleOf(agent::sessions(), draft.session)), {true});
      paragraph(c, r, draft.text, std::max(1, (r.h - 16) / lineHeight(style::kBody)));
      drawnSent_.clear();
      return;
    }
    drawnSent_ = typedInto_.empty() ? sentTo(s) : typedInto_;
    drawnNotTyped_ = notTyped_;
    drawSectionHeader(c, r.takeTop(kSectionH), "REPLY");
    paragraph(c, r,
              !notTyped_.empty()   ? notTyped_
              : drawnSent_.empty() ? s.reply
              : typedInto_.empty() ? "Sent to " + drawnSent_ + "."
                                   : "Typed into " + drawnSent_ + ".",
              std::max(1, (r.h - 16) / lineHeight(style::kBody)));
  }

 private:
  uint32_t drawn_ = 0;
  std::string drawnSent_;  // sentTo() as last drawn
  std::string typedInto_;  // a draft typed from here: its session's title
  uint32_t turn_ = 0;      // the utterance typedInto_ belongs to
  uint32_t refusalsAtSend_ = 0;  // agent::sessions().refusals when the draft went
  std::string notTyped_;         // why inkharness wouldn't type it
  std::string drawnNotTyped_;
  uint32_t sentMs_ = 0;    // when sentTo() first said so; 0: it doesn't
  uint32_t seen_ = 0;
  uint32_t activeMs_ = millis();
};

}  // namespace

bool agentSheetOnTop() {
  const ui::Screen *top = ui::nav::top();
  return top && top == onTop;
}

void raiseAgentSheet() { ui::nav::push(std::make_unique<AgentSheet>()); }

}  // namespace apps

#endif  // INKDECK_AGENT
