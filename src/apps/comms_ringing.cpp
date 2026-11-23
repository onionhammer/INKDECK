// The dialer's ringing sheet (apps/comms_ui.h): ● RING on the talk screen raises it over that
// screen while the ring goes. RINGING with the seconds and how many boards ring, a bar to the
// 60 s; from 5 s HOLD BOOT records a message instead (RECORDING, the meter), letting go sends it
// (SENT, how many have it whole, then it closes); NO ANSWER at 60 s stays up for a message or
// ● RING AGAIN. BOOT gives up (or closes). When someone answers it closes, and the talk screen
// shows the conversation.
#include "features.h"

#if INKDECK_COMMS  // the whole file (features.h)

#include <Arduino.h>

#include <algorithm>
#include <string>
#include <vector>

#include "apps/comms_ui.h"
#include "input.h"

namespace apps {
namespace {

using namespace commsui;
using ui::Key;
using ui::Rect;

const std::string kDot = std::string(" ") + ui::glyph::kDot + " ";

constexpr int kSheetMinH = 384;  // as the other comms sheets
constexpr uint32_t kSentShowMs = 5000;  // SENT, then it closes
constexpr uint32_t kStartWaitMs = 2000;  // the ring hasn't started by then: why not

class RingingSheet : public ui::Screen {
 public:
  RingingSheet(int group, uint32_t target) : group_(group), target_(target) {
    comms::hold();
    count_ = comms::dial().count;  // its ring is the next one
    startedMs_ = millis();
    load();
  }
  ~RingingSheet() override { comms::release(); }

  std::string tag() const override { return {}; }  // the talk screen's chrome
  bool overlay() const override { return true; }
  // HOLD BOOT records a message for the ring (from 5 s, or after NO ANSWER); before that the
  // engine says it can't yet.
  ui::TalkTo talkTo() const override {
    ui::TalkTo to;
    to.comms = true;
    to.group = group_;
    to.board = target_;
    to.message = true;
    return to;
  }
  int hints(ui::Hint *out) const override {
    int n = 0;
    switch (state()) {
      case comms::DialState::Ringing:
        if (dial_.canLeaveMessage) out[n++] = {"HOLD BOOT", "LEAVE A MESSAGE"};
        out[n++] = {"BOOT", "GIVE UP"};
        return n;
      case comms::DialState::Recording:
        out[n++] = {"HOLD BOOT", "LET GO TO SEND"};
        return n;
      case comms::DialState::NoAnswer:
        out[n++] = {"HOLD BOOT", "LEAVE A MESSAGE"};
        out[n++] = {ui::glyph::kPress, "RING AGAIN"};
        break;
      case comms::DialState::None:
        if (failed()) out[n++] = {ui::glyph::kPress, "RING AGAIN"};
        break;
      default: break;
    }
    out[n++] = {"BOOT", playing() ? "STOP" : "CLOSE"};
    return n;
  }

  bool onKey(Key key) override {
    const comms::DialState st = state();
    switch (key) {
      case Key::Select:
        if (st == comms::DialState::NoAnswer || (st == comms::DialState::None && failed())) {
          count_ = comms::dial().count;  // follow the new ring
          startedMs_ = millis();
          if (!comms::ringStart(group_, target_)) noteAttempt();
          load();
        }
        return true;
      case Key::Back:
        if (stopIfPlaying()) return true;
        if (st == comms::DialState::Recording) return true;  // HOLD BOOT's release sends it
        comms::ringStop();  // gives up a ring going; clears one over (NO ANSWER, SENT)
        ui::nav::pop();
        return true;
      default: return false;
    }
  }

  bool onTick() override {
    load();
    const uint32_t now = millis();
    switch (state()) {
      case comms::DialState::Answered:
        comms::ringStop();  // dial() back to None; the conversation is on
        ui::nav::pop();     // the talk screen shows it
        return false;
      case comms::DialState::Sent:
        if (!sentMs_) sentMs_ = now;
        if (now - sentMs_ > kSentShowMs && input::idleMs() > kSentShowMs) {
          comms::ringStop();
          ui::nav::pop();
          return false;
        }
        break;
      default: sentMs_ = 0; break;
    }
    return comms::generation() != drawn_ || attemptError() != drawnError_ ||
           failed() != drawnFailed_;
  }

  void draw(ui::Canvas &c, const Rect &body) override {
    using namespace ui;
    drawn_ = comms::generation();
    drawnError_ = attemptError();
    drawnFailed_ = failed();
    load();
    const comms::DialState st = state();
    const bool live = st == comms::DialState::Ringing || st == comms::DialState::Recording;
    const bool message = st == comms::DialState::Recording || st == comms::DialState::Sent;
    const std::string group = groupName(group_);
    const std::string to = target_ ? boardName(dial_.targetName, target_) : group;

    Hint keys[kMaxHints];
    const Screen *under = nav::base();
    const int keyCount = under && !under->chrome() ? hints(keys) : 0;
    Rect r = drawSheet(c, body, kSheetMinH, keys, keyCount);
    const Rect head = r.takeTop(kSectionH);
    std::string error = drawnError_;
    if (error.empty() && failed()) error = comms::status().error;
    if (!error.empty()) {
      // Held too soon: the engine has no ring to leave a message for yet.
      if (st == comms::DialState::Ringing && error == "NO RING TO LEAVE A MESSAGE FOR") {
        error = "A MESSAGE CAN GO FROM 0:05";
      }
      drawNotice(c, head, error);
    } else {
      drawSectionHeader(c, head, (message ? "MESSAGE" : "RINGING") + kDot + to,
                        target_ ? "ONE BOARD" : "ALL OF IT", {live});
    }

    // The state in large type with its seconds on the right; a line; the bar or the meter.
    std::string word, right, line, foot;
    float bar = -1;  // the ring's 60 s
    int meter = -1;
    switch (st) {
      case comms::DialState::Ringing:
        word = "RINGING";
        right = lengthText(dial_.ms);
        line = dial_.ringing ? boardsText(dial_.ringing) + " RINGING" : "NOBODY RINGING YET";
        bar = static_cast<float>(dial_.ms) / comms::kRingMs;
        foot = dial_.canLeaveMessage ? "NOBODY YET: HOLD BOOT TO LEAVE A MESSAGE"
                                     : "NO ANSWER AT 1:00";
        break;
      case comms::DialState::Recording:
        word = "RECORDING";
        right = lengthText(dial_.messageMs);
        line = (target_ ? "FOR " + to : "FOR ALL OF " + group) + kDot + "LET GO TO SEND";
        meter = meterFill(comms::status().levelDb);
        foot = "IT STOPS AND GOES AT 1:00";
        break;
      case comms::DialState::Sent:
        word = "SENT";
        right = lengthText(dial_.messageMs);
        line = dial_.delivered == 0  ? "ON ITS WAY: NOBODY HAS IT WHOLE YET"
               : dial_.delivered == 1 ? "1 BOARD HAS IT WHOLE"
                                      : boardsText(dial_.delivered) + " HAVE IT WHOLE";
        foot = "CLOSES IN A FEW SECONDS";
        break;
      case comms::DialState::NoAnswer:
        word = "NO ANSWER";
        right = lengthText(dial_.ms);
        line = "RANG " + lengthText(dial_.ms) + kDot + "NOBODY ANSWERED";
        bar = 1;
        foot = "A MESSAGE CAN STILL BE LEFT";
        break;
      case comms::DialState::Answered:
        word = "ANSWERED";
        line = boardName(dial_.answeredByName, dial_.answeredBy);
        break;
      case comms::DialState::None:
        word = failed() ? "NOT RINGING" : "RINGING";
        right = failed() ? "" : "0:00";
        line = failed() ? "THE RING DIDN'T START" : "STARTING\xE2\x80\xA6";
        break;
    }
    const int x = r.x + kPadX, w = r.w - 2 * kPadX;
    int y = r.y + 16;
    const int base = y + capHeight(style::kTitle);
    drawText(c, x, base, word, style::kTitle);
    if (!right.empty()) {
      drawText(c, x + w - textWidth(style::kTitle, right), base, right, style::kTitle);
    }
    y += 38 + 10;
    drawText(c, {x, y, w, 17}, line, style::kLabel);
    y += 17 + 12;
    if (bar >= 0) {
      drawProgress(c, {x, y, w, 14}, std::min(bar, 1.0f));
    } else if (meter >= 0) {
      drawText(c, {x, y, 22, 16}, "IN", style::kCap);
      drawSegments(c, {x + 28, y, w - 28, 16}, 20, meter, 2);
    }
    y += 16 + 14;
    r.takeTop(y - r.y);

    std::vector<comms::Call> earlier;
    for (const comms::Call &k : comms::calls(group_, 12)) {
      if (!k.live) earlier.push_back(k);
    }
    drawEarlier(c, r, earlier, foot);
  }

 private:
  void load() {
    dial_ = comms::dial();
    ours_ = dial_.count != count_ && dial_.group == group_ && dial_.target == target_;
  }
  // This sheet's ring's state: None until the engine has started it.
  comms::DialState state() const { return ours_ ? dial_.state : comms::DialState::None; }
  // It hasn't started, and the engine says why (the radio won't start...).
  bool failed() const {
    return !ours_ && millis() - startedMs_ > kStartWaitMs && !comms::status().error.empty();
  }

  int group_;
  uint32_t target_;
  uint32_t count_ = 0;  // dial().count before its ring: its ring has another
  uint32_t startedMs_ = 0;
  uint32_t sentMs_ = 0;
  comms::Dial dial_;
  bool ours_ = false;
  uint32_t drawn_ = 0;
  std::string drawnError_;
  bool drawnFailed_ = false;
};

}  // namespace

std::unique_ptr<ui::Screen> commsui::makeRingingSheet(int group, uint32_t target) {
  return std::make_unique<RingingSheet>(group, target);
}

}  // namespace apps

#endif  // INKDECK_COMMS
