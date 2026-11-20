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
class VoiceSheet : public ui::Screen {
 public:
  std::string tag() const override { return {}; }  // the screen below supplies the chrome
  bool overlay() const override { return true; }

  int hints(ui::Hint *out) const override {
    int n = 0;
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
    voice::close();  // covered (the lock, an alarm, PWR) or closed: either way, done
  }

  bool onKey(Key key) override {
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
    return s.generation != drawn_ || s.mic;  // the meter moves
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

    drawSectionHeader(c, r.takeTop(kSectionH), "REPLY");
    paragraph(c, r, s.reply, std::max(1, (r.h - 16) / lineHeight(style::kBody)));
  }

 private:
  uint32_t drawn_ = 0;
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
