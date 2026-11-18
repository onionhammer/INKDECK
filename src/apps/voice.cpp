#include "apps/voice.h"

#include <Arduino.h>

#include <algorithm>
#include <atomic>
#include <string>

#include "apps/home.h"
#include "apps/music.h"
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

// The sheet: half the body, but never so short that the reply gets no room (landscape).
constexpr int kSheetMinH = 300;
constexpr int kSheetStateH = 52;  // the state word beside the mic meter
constexpr int kSheetHeardLines = 2;
constexpr uint32_t kSheetCloseMs = 30000;  // after the last reply, and as long since a key

// The Voice screen or the sheet while on top: HOLD BOOT there talks without opening another.
const ui::Screen *voiceOnTop = nullptr;

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
    "Name the machine that runs voiced in SD:/inkdeck.ini: voice_host, and voice_token if it "
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

// The wheel press on the Voice screen, from the input timer: straight to the voice task, so the
// mic opens when the wheel goes down even while the panel is refreshing.
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
    voiceOnTop = this;
    if (!voice::configured()) return;
    voice::open();
    input::onSelectEdge(onEdge);
  }

  void onLeave() override {
    voiceOnTop = nullptr;
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
    return s.generation != drawn_ || s.mic;  // the meter moves
  }

  void draw(ui::Canvas &c, const Rect &body) override {
    using namespace ui;
    const voice::Status s = voice::status();
    drawn_ = s.generation;

    Rect r = body;
    const Rect head = r.takeTop(kSectionH);
    if (!voice::configured()) {
      drawNotice(c, head, notSetUp());
    } else if (!s.error.empty()) {
      drawNotice(c, head, upper(s.error));
    } else {
      drawSectionHeader(c, head, "ASSISTANT", "VOL " + std::to_string(audio::status().volume),
                        {s.mic});
    }

    // The state, and the mic's level under it while listening.
    const Rect panel = r.takeTop(kPanelH);
    const std::string word = !voice::configured() ? "NOT SET UP"
                             : s.mic              ? "LISTENING"
                                                  : stateWord(s.state);
    drawText(c, panel.x + kPadX, panel.y + 26 + capHeight(style::kTitle), word, style::kTitle);
    const Rect meter{panel.x + kPadX, panel.y + 88, panel.w - 2 * kPadX, 20};
    drawSegments(c, meter, kMeterSegments, meterFill(s));

    if (!voice::configured()) {
      paragraph(c, r, kSetUpHelp, 6);
      return;
    }

    drawSectionHeader(c, r.takeTop(kSectionH), "YOU");
    Rect heard = r.takeTop(12 + kHeardLines * lineHeight(style::kBody) + 8);
    paragraph(c, heard, s.heard.empty() ? (s.mic ? ui::glyph::kEllipsis : "") : s.heard,
              kHeardLines);

    drawSectionHeader(c, r.takeTop(kSectionH), "REPLY");
    paragraph(c, r, s.reply, std::max(1, (r.h - 20) / lineHeight(style::kBody)));
  }

 private:
  uint32_t drawn_ = 0;
};

// The voice sheet (HOLD BOOT): an overlay over the lower half of whatever is on screen. The hold
// that opens it has already started the session (talkEdge()); closing it ends the session.
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

  void onEnter() override { voiceOnTop = this; }

  void onLeave() override {
    voiceOnTop = nullptr;
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
      drawSectionHeader(c, head, "ASSISTANT", right, {s.mic});
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

// What the last HOLD BOOT's edge chose: talk, or (music or a podcast playing, or talking not
// allowed) not. talkKey() follows it, so the two can't disagree if playback ends in between.
std::atomic<bool> holdTalks{false};

// HOLD BOOT's two ends, from the input timer: straight to the voice task, so the mic opens on
// time even while the panel is refreshing. Playback comes first: while it plays, HOLD BOOT is the
// player's shortcut. The release always goes (a stray one does nothing).
void talkEdge(bool down) {
  if (!down) {
    voice::talkStop();
    return;
  }
  const bool talks = ui::nav::talkAllowed() && !audio::playing();
  holdTalks = talks;
  if (talks) voice::talkStart();  // with no session, opens one
}

// HOLD BOOT in the UI task, after talkEdge(): the player's shortcut while something plays,
// otherwise the sheet, unless the Voice screen or the sheet is on top already.
void talkKey(bool held) {
  if (!held) return;  // the release went to the voice task already; the navigator redraws
  if (!holdTalks) {
    openPlayer();  // does nothing if nothing plays (talking wasn't allowed)
    return;
  }
  ui::Screen *top = ui::nav::top();
  if (top && top == voiceOnTop) return;
  if (!ui::nav::talkAllowed()) {
    // The stack changed between the hold and now (the lock came up): undo what the hold began.
    voice::close();
    return;
  }
  ui::nav::push(std::make_unique<VoiceSheet>());
}

}  // namespace

std::unique_ptr<ui::Screen> makeVoice() { return std::make_unique<VoiceScreen>(); }

void beginTalkKey() {
  voice::begin();
  input::onBackHold(talkEdge);
  ui::nav::onTalkKey(talkKey);
}

}  // namespace apps
