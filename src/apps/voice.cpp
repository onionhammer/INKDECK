#include "features.h"

#if INKDECK_VOICE  // the whole file (features.h)

#include "apps/voice.h"

#include <Arduino.h>

#include <algorithm>
#include <string>

#include "audio.h"
#include "input.h"
#include "settings.h"
#include "voice_client.h"

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

const char *stateWord(voice::State s) {
  switch (s) {
    case voice::State::Off: return "OFF";
    case voice::State::Connecting: return "CONNECTING";
    case voice::State::Ready: return "READY";
    case voice::State::Listening: return "LISTENING";
    case voice::State::Thinking: return "THINKING";
    case voice::State::Speaking: return "SPEAKING";
    case voice::State::Error: return "OFFLINE";
  }
  return "";
}

const char *notSetUp() {
  return settings::hasVoice() ? "NO WI-FI NETWORK SET" : "NO VOICE HOST SET";
}

constexpr const char *kSetUpHelp =
    "Name the machine that runs inkharness in SD:/inkdeck.ini: voice_host, and voice_token if it "
    "asks for one. Then restart the board.";

int meterFill(const voice::Status &s) {
  return s.mic ? std::clamp(static_cast<int>((s.levelDb + 60) / 3), 0, kMeterSegments) : 0;
}

// Word-wrapped body text from the top of `area`.
void paragraph(ui::Canvas &c, const Rect &area, const std::string &text, int maxLines) {
  using namespace ui;
  int baseline = area.y + 12 + capHeight(style::kBody);
  for (const std::string &line : wrap(style::kBody, text, area.w - 2 * kPadX, maxLines)) {
    drawText(c, area.x + kPadX, baseline, line, style::kBody);
    baseline += lineHeight(style::kBody);
  }
}

// The voice sheet (HOLD BOOT): an overlay over the lower half of whatever is on screen. The hold
// that opens it has already started the session (apps/hold_boot.cpp); closing it ends it.
std::string titleOf(const voice::Agent &a, const std::string &session) {
  for (const voice::AgentRow &row : a.rows) {
    if (row.id == session && !row.title.empty()) return row.title;
  }
  return "the agent";
}

// Words for a herdr agent this turn made, waiting for ● (an empty id: none).
voice::Draft draftFor(const voice::Status &s) {
  const voice::Agent a = voice::agent();
  return a.draft.turn == s.turn && !s.mic ? a.draft : voice::Draft{};
}

// An agent turn that ended without a spoken reply (inkharness's [agent] reply = "none"): the
// words went straight to a session. Its title then, or "" while that isn't so.
std::string sentTo(const voice::Status &s) {
  if (!s.agentTurn || !s.reply.empty() || s.mic || s.state != voice::State::Ready) return {};
  const voice::Agent a = voice::agent();
  if (a.started.empty() || a.startedTurn != s.turn || a.draft.turn == s.turn) return {};
  return titleOf(a, a.started);
}

class VoiceSheet : public ui::Screen {
 public:
  std::string tag() const override { return {}; }  // the screen below supplies the chrome
  bool overlay() const override { return true; }

  int hints(ui::Hint *out) const override {
    int n = 0;
    if (!draftFor(voice::status()).id.empty()) {
      out[n++] = {ui::glyph::kPress, "TYPE IT"};
      out[n++] = {"HOLD BOOT", "SAY AGAIN"};
      out[n++] = {"BOOT", "DROP"};
      return n;
    }
    if (voice::configured()) {
      const voice::State s = voice::status().state;
      const bool replying = s == voice::State::Thinking || s == voice::State::Speaking;
      out[n++] = {"HOLD BOOT", replying ? "INTERRUPT" : "TALK"};
      out[n++] = {ui::glyph::kDial, "VOL"};
    }
    out[n++] = {"BOOT", "CLOSE"};
    return n;
  }

  void onEnter() override { onTop = this; }

  void onLeave() override {
    onTop = nullptr;
    voice::agentDropDraft();  // words not typed by now never will be (does nothing without one)
    voice::close();  // covered (the lock, an alarm, PWR) or closed: either way, done
  }

  bool onKey(Key key) override {
    const voice::Status s = voice::status();
    const voice::Draft draft = draftFor(s);
    if (!draft.id.empty()) {
      switch (key) {
        case Key::Select:
          typedInto_ = titleOf(voice::agent(), draft.session);
          refusalsAtSend_ = voice::agent().refusals;
          voice::agentSendDraft();
          return true;
        case Key::Back: voice::agentDropDraft(); return false;  // and the navigator pops
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
    const voice::Status s = voice::status();
    const uint32_t now = millis();
    if (s.generation != seen_ || s.mic || voice::busy()) {
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
      const voice::Agent a = voice::agent();
      if (!a.draft.id.empty() && a.draft.turn != s.turn) voice::agentDropDraft();
    }
    // inkharness wouldn't type the draft (the program got busy, it ran out): say why, and stay.
    if (!typedInto_.empty()) {
      const voice::Agent a = voice::agent();
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
    const voice::Status s = voice::status();
    drawn_ = s.generation;

    // Over a screen without chrome (the desk face), the sheet shows its keys itself.
    Hint keys[kMaxHints];
    const Screen *under = nav::base();
    const int keyCount = under && !under->chrome() ? hints(keys) : 0;
    Rect r = drawSheet(c, body, kSheetMinH, keys, keyCount);

    const Rect head = r.takeTop(kSectionH);
    if (!voice::configured()) {
      drawNotice(c, head, notSetUp());
      paragraph(c, r, kSetUpHelp, std::max(1, (r.h - 20) / lineHeight(style::kBody)));
      return;
    }
    if (!s.error.empty()) {
      drawNotice(c, head, upper(s.error));
    } else {
      const std::string right = s.state == voice::State::Connecting
                                    ? "CONNECTING"
                                    : "VOL " + std::to_string(audio::status().volume);
      drawSectionHeader(c, head, s.agentTurn ? "AGENT" : "ASSISTANT", right, {s.mic});
    }

    // The state word, and the mic's level beside it.
    const Rect row = r.takeTop(kSheetStateH);
    const std::string word = s.mic ? "LISTENING" : stateWord(s.state);
    const int wordW = textWidth(style::kReading, "CONNECTING") + 24;
    drawText(c, row.x + kPadX, baselineCentered(style::kReading, row.y, row.h), word,
             style::kReading);
    const Rect meter{row.x + kPadX + wordW, row.y + (row.h - 18) / 2,
                     row.w - 2 * kPadX - wordW, 18};
    drawSegments(c, meter, kMeterSegments, meterFill(s));

    drawSectionHeader(c, r.takeTop(kSectionH), "YOU");
    const Rect heard = r.takeTop(12 + kSheetHeardLines * lineHeight(style::kBody) + 6);
    paragraph(c, heard, s.heard.empty() ? (s.mic ? ui::glyph::kEllipsis : "") : s.heard,
              kSheetHeardLines);

    const voice::Draft draft = draftFor(s);
    if (!draft.id.empty()) {
      // Words for a herdr agent's terminal: shown, and typed only on ●.
      drawSectionHeader(c, r.takeTop(kSectionH), "TYPE IT?",
                        upper(titleOf(voice::agent(), draft.session)), {true});
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
  uint32_t refusalsAtSend_ = 0;  // voice::agent().refusals when the draft went
  std::string notTyped_;         // why inkharness wouldn't type it
  std::string drawnNotTyped_;
  uint32_t sentMs_ = 0;    // when sentTo() first said so; 0: it doesn't
  uint32_t seen_ = 0;
  uint32_t activeMs_ = millis();
};

}  // namespace

bool voiceOnTop() {
  const ui::Screen *top = ui::nav::top();
  return top && top == onTop;
}

void raiseVoiceSheet() { ui::nav::push(std::make_unique<VoiceSheet>()); }

}  // namespace apps

#endif  // INKDECK_VOICE
