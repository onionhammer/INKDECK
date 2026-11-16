#include "apps/voice.h"

#include <algorithm>
#include <string>

#include "apps/home.h"
#include "audio.h"
#include "input.h"
#include "settings.h"
#include "voice_client.h"

namespace apps {
namespace {

using ui::Key;
using ui::Rect;

constexpr int kPanelH = 132;         // the state word and the mic meter
constexpr int kMeterSegments = 20;   // -60..0 dBFS, 3 dB each
constexpr int kHeardLines = 3;

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

// The talk key's edges, from the input timer: straight to the voice task, so the mic opens
// when the wheel goes down even while the panel is refreshing.
void onEdge(bool down) {
  if (down) {
    voice::talkStart();
  } else {
    voice::talkStop();
  }
}

class VoiceScreen : public ui::Screen {
 public:
  std::string tag() const override { return moduleTag("VOICE"); }
  std::string context() const override {
    return settings::hasVoice() ? settings::voiceHost() : std::string();
  }

  int hints(ui::Hint *out) const override {
    int n = 0;
    const voice::State s = voice::status().state;
    if (!voice::configured()) {
      // nothing to do here but go back
    } else if (s == voice::State::Error) {
      out[n++] = {ui::glyph::kPress, "RETRY"};
    } else if (s == voice::State::Thinking || s == voice::State::Speaking) {
      out[n++] = {ui::glyph::kHoldPress, "INTERRUPT"};
    } else if (s != voice::State::Connecting) {
      out[n++] = {ui::glyph::kHoldPress, "TALK"};
    }
    out[n++] = {ui::glyph::kDial, "VOL"};
    out[n++] = {"BOOT", "BACK"};
    return n;
  }

  void onEnter() override {
    if (!voice::configured()) return;
    voice::open();
    input::onSelectEdge(onEdge);
  }

  void onLeave() override {
    input::onSelectEdge(nullptr);
    voice::close();
  }

  bool onKey(Key key) override {
    switch (key) {
      case Key::Up: audio::changeVolume(+5); return true;
      case Key::Down: audio::changeVolume(-5); return true;
      case Key::SelectDown:
      case Key::SelectUp: return true;  // the voice task has it already; redraw the state
      default: return false;
    }
  }

  bool onTick() override {
    const voice::Status s = voice::status();
    return s.generation != drawn_ || s.state == voice::State::Listening;  // the meter moves
  }

  void draw(ui::Canvas &c, const Rect &body) override {
    using namespace ui;
    const voice::Status s = voice::status();
    drawn_ = s.generation;
    const bool listening = s.state == voice::State::Listening;

    Rect r = body;
    const Rect head = r.takeTop(kSectionH);
    if (!voice::configured()) {
      drawNotice(c, head, settings::hasVoice() ? "NO WI-FI NETWORK SET" : "NO VOICE HOST SET");
    } else if (!s.error.empty()) {
      drawNotice(c, head, upper(s.error));
    } else {
      drawSectionHeader(c, head, "ASSISTANT", "VOL " + std::to_string(audio::status().volume),
                        {listening});
    }

    // The state, and the mic's level under it while listening.
    const Rect panel = r.takeTop(kPanelH);
    const std::string word = voice::configured() ? stateWord(s.state) : "NOT SET UP";
    drawText(c, panel.x + kPadX, panel.y + 26 + capHeight(style::kTitle), word, style::kTitle);
    const Rect meter{panel.x + kPadX, panel.y + 88, panel.w - 2 * kPadX, 20};
    const int filled =
        listening ? std::clamp(static_cast<int>((s.levelDb + 60) / 3), 0, kMeterSegments) : 0;
    drawSegments(c, meter, kMeterSegments, filled);

    if (!voice::configured()) {
      paragraph(c, r, "Name the machine that runs voiced in SD:/inkdeck.ini: voice_host, and "
                      "voice_token if it asks for one. Then restart the board.", 6);
      return;
    }

    drawSectionHeader(c, r.takeTop(kSectionH), "YOU");
    Rect heard = r.takeTop(12 + kHeardLines * lineHeight(style::kBody) + 8);
    paragraph(c, heard, s.heard.empty() ? (listening ? "\xE2\x80\xA6" : "") : s.heard, kHeardLines);

    drawSectionHeader(c, r.takeTop(kSectionH), "REPLY");
    paragraph(c, r, s.reply, std::max(1, (r.h - 20) / lineHeight(style::kBody)));
  }

 private:
  // Word-wrapped body text from the top of `area`.
  static void paragraph(ui::Canvas &c, const Rect &area, const std::string &text, int maxLines) {
    using namespace ui;
    int baseline = area.y + 12 + capHeight(style::kBody);
    for (const std::string &line : wrap(style::kBody, text, area.w - 2 * kPadX, maxLines)) {
      drawText(c, area.x + kPadX, baseline, line, style::kBody);
      baseline += lineHeight(style::kBody);
    }
  }

  uint32_t drawn_ = 0;
};

}  // namespace

std::unique_ptr<ui::Screen> makeVoice() { return std::make_unique<VoiceScreen>(); }

}  // namespace apps
