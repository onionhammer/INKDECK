// The call sheet (apps/comms_ui.h): rises over the screen in use when a call starts playing, and
// apps::pollComms() decides where it may.
#include "features.h"

#if INKDECK_COMMS  // the whole file (features.h)

#include <Arduino.h>

#include <algorithm>
#include <string>
#include <vector>

#include "apps/clock.h"
#include "apps/comms_ui.h"
#include "apps/home.h"
#include "audio.h"
#include "input.h"
#if INKDECK_AGENT
#include "agent_client.h"
#include "apps/agent_sheet.h"
#endif

namespace apps {
namespace {

using namespace commsui;
using ui::Key;
using ui::Rect;

const std::string kDot = std::string(" ") + ui::glyph::kDot + " ";

constexpr int kSheetMinH = 360;
constexpr int kStateH = 52;  // the state word beside the meter
constexpr int kRowH = 30;
constexpr uint32_t kCloseMs = 10000;  // after the last word, and as long since a key

const ui::Screen *onTop = nullptr;

class CallSheet : public ui::Screen {
 public:
  CallSheet() {
    comms::hold();  // while it's up the call counts as heard, and the radio stays on
    follow(comms::status());
    activeMs_ = millis();
  }
  ~CallSheet() override { comms::release(); }

  std::string tag() const override { return {}; }  // the screen below supplies the chrome
  bool overlay() const override { return true; }
  // HOLD BOOT replies the way the call came: to all of the group, or to the caller alone.
  ui::TalkTo talkTo() const override {
    ui::TalkTo to;
    to.comms = true;
    to.group = group_;
    to.board = toAll_ ? 0 : peer_;
    return to;
  }
  int hints(ui::Hint *out) const override {
    out[0] = {"HOLD BOOT", "REPLY"};
    out[1] = {ui::glyph::kDial, "VOL"};
    out[2] = {"BOOT", "CLOSE"};
    return 3;
  }

  void onEnter() override { onTop = this; }
  void onLeave() override { onTop = nullptr; }

  bool onKey(Key key) override {
    switch (key) {
      case Key::Up: audio::changeVolume(+5); return true;
      case Key::Down: audio::changeVolume(-5); return true;
      default: return false;  // BOOT closes (the navigator pops)
    }
  }

  bool onTick() override {
    const comms::Status s = comms::status();
    const uint32_t now = millis();
    // Another call came while it was up: it follows that one.
    if (s.incoming != incoming_ && s.state == comms::State::Hearing && !s.replay) {
      follow(s);
      ui::nav::talkChanged();
    }
    if (s.state == comms::State::Talking || s.state == comms::State::Hearing ||
        comms::talking()) {
      activeMs_ = now;
    }
    if (s.state == comms::State::Talking) replied_ = true;
    if (s.state == comms::State::Hearing && !s.replay && s.peer == peer_) {  // as it comes now
      route_ = s.route;
      hops_ = s.hops;
      relayName_ = s.relayName;
      rssi_ = s.rssi;
    }
    if (now - activeMs_ > kCloseMs && input::idleMs() > kCloseMs) {
      ui::nav::pop();
      return false;
    }
    return comms::generation() != drawn_ || attemptError() != drawnError_;
  }

  void draw(ui::Canvas &c, const Rect &body) override {
    using namespace ui;
    drawn_ = comms::generation();
    drawnError_ = attemptError();
    const comms::Status s = comms::status();
    const bool talking = s.state == comms::State::Talking;
    const std::vector<comms::Call> calls = comms::calls(group_, 12);

    // Over a screen without chrome, the sheet shows its keys itself.
    Hint keys[kMaxHints];
    const Screen *under = nav::base();
    const int keyCount = under && !under->chrome() ? hints(keys) : 0;
    Rect r = drawSheet(c, body, kSheetMinH, keys, keyCount);

    // What the big block shows: the reply while it goes and once it's sent, else the call.
    const bool showReply = talking || replied_;
    int shownAt = -1;  // its record in `calls`, left out of EARLIER
    for (size_t i = 0; i < calls.size() && shownAt < 0; ++i) {
      const comms::Call &k = calls[i];
      if (showReply ? k.outgoing : !k.outgoing && k.from == peer_) shownAt = static_cast<int>(i);
    }
    const comms::Call *shown = shownAt >= 0 ? &calls[shownAt] : nullptr;

    const std::string groupName = groupNameOf(group_);
    const Rect head = r.takeTop(kSectionH);
    if (!drawnError_.empty()) {
      drawNotice(c, head, drawnError_);
    } else {
      std::string to;
      if (showReply) {
        const uint32_t target = talking ? s.peer : shown ? shown->to : 0;
        const std::string name = talking ? s.peerName : shown ? shown->toName : "";
        to = target ? "TO " + boardName(name, target) : "TO ALL";
      } else {
        to = toAll_ ? "TO ALL" : "TO YOU";
      }
      drawSectionHeader(c, head, "COMMS" + kDot + groupName + kDot + to,
                        "VOL " + std::to_string(audio::status().volume), {true});
    }

    // The state, and the meter beside it.
    const Rect row = r.takeTop(kStateH);
    dashedH(c, row.x, row.bottom() - 1, row.w);
    drawText(c, row.x + kPadX, baselineCentered(style::kReading, row.y, row.h - 1),
             stateWord(s.state), style::kReading);
    const bool live = talking || s.state == comms::State::Hearing;
    const int meterX = row.x + kPadX + 120 + 14;
    drawSegments(c, {meterX, row.y + (row.h - 16) / 2, row.right() - kPadX - meterX, 16}, 20,
                 live ? meterFill(s.levelDb) : 0, 2);

    // FROM (or TO) and the name in large type, how it comes and how long.
    const int x = r.x + kPadX, w = r.w - 2 * kPadX;
    int y = r.y + 14;
    std::string label, name, line;
    if (showReply) {
      label = "TO";
      const uint32_t target = talking ? s.peer : shown ? shown->to : 0;
      const std::string tname = talking ? s.peerName : shown ? shown->toName : "";
      name = target ? boardName(tname, target) : "ALL OF " + groupName;
      if (talking) {
        line = (s.channel ? "AIR CH " + std::to_string(s.channel) + kDot : "") +
               lengthText(s.ms) + " SO FAR";
      } else if (shown) {
        line = lengthText(shown->ms) + kDot +
               (shown->heardBy > 0 ? std::to_string(shown->heardBy) + " HEARD IT"
                                   : std::string("SENT"));
      }
    } else {
      label = "FROM";
      name = boardName(peerName_, peer_);
      const bool hearing = s.state == comms::State::Hearing && !s.replay && s.peer == peer_;
      line = route_ == comms::Route::Relayed
                 ? "VIA " + (relayName_.empty() ? std::string("A RELAY") : relayName_) + kDot +
                       std::to_string(hops_) + (hops_ == 1 ? " HOP" : " HOPS")
                 : "ON AIR" + (rssi_ ? kDot + ui::signedNumber(static_cast<float>(rssi_), 0) +
                                           " dBm"
                                     : std::string());
      if (hearing) {
        line += kDot + lengthText(s.ms) + " SO FAR";
      } else if (shown) {
        line += kDot + lengthText(shown->ms);
      }
    }
    drawText(c, {x, y, w, 17}, label, style::kKey);
    y += 17 + 6;
    drawText(c, {x, y, w, 38}, name, style::kTitle);
    y += 38 + 6;
    drawText(c, {x, y, w, 17}, line, style::kLabel);
    y += 17 + 12;
    r.takeTop(y - r.y);

    // EARLIER: the group's other calls, as many as fit over the foot.
    std::vector<comms::Call> earlier;
    for (size_t i = 0; i < calls.size(); ++i) {
      if (static_cast<int>(i) != shownAt && !calls[i].live) earlier.push_back(calls[i]);
    }
    hrule(c, r.x, r.y, r.w);
    r.takeTop(kRule);
    drawSectionHeader(c, r.takeTop(kSectionH), "EARLIER",
                      earlier.empty() ? "NONE" : countText(earlier));
    const Rect foot = r.takeBottom(8 + 17);
    for (const comms::Call &k : earlier) {
      if (r.h < kRowH) break;
      drawCallRow(c, r.takeTop(kRowH), k, false);
    }
    drawText(c, {foot.x + kPadX, foot.y, foot.w - 2 * kPadX, 17},
             "CLOSES 10 S AFTER THE LAST WORD", style::kLabel);
  }

 private:
  void follow(const comms::Status &s) {
    incoming_ = s.incoming;
    group_ = s.group;
    peer_ = s.peer;
    peerName_ = s.peerName;
    toAll_ = s.toAll;
    route_ = s.route;
    hops_ = s.hops;
    relayName_ = s.relayName;
    rssi_ = s.rssi;
    replied_ = false;
  }

  static std::string groupNameOf(int group) {
    const std::vector<comms::Group> gs = comms::groups();
    return group >= 0 && group < static_cast<int>(gs.size()) ? gs[group].name : std::string();
  }

  uint32_t incoming_ = 0;
  int group_ = 0;
  uint32_t peer_ = 0;
  std::string peerName_;
  bool toAll_ = true;
  // How the call came, as last seen while it played (status() forgets once it's over).
  comms::Route route_ = comms::Route::Air;
  int hops_ = 0;
  std::string relayName_;
  int rssi_ = 0;
  bool replied_ = false;  // HOLD BOOT talked from the sheet since this call
  uint32_t activeMs_ = 0;
  uint32_t drawn_ = 0;
  std::string drawnError_;
};

bool callSheetOnTop() {
  const ui::Screen *top = ui::nav::top();
  return top && top == onTop;
}

#if INKDECK_AGENT
// The agent sheet listening, thinking or speaking: a call sheet over it would take its keys.
bool agentTalking() {
  if (!agentSheetOnTop()) return false;
  const agent::Status a = agent::status();
  return a.mic || a.state == agent::State::Connecting || a.state == agent::State::Listening ||
         a.state == agent::State::Thinking || a.state == agent::State::Speaking;
}
#endif

}  // namespace

void pollComms() {
  static bool primed = false;
  static uint32_t seen = 0;
  const comms::Status s = comms::status();
  if (!primed) {
    primed = true;
    seen = s.incoming;
    return;
  }
  if (s.incoming == seen) return;
  seen = s.incoming;
  if (s.state != comms::State::Hearing || s.replay) return;  // over already
  if (callSheetOnTop()) return;  // it follows the new call itself
  const ui::Screen *top = ui::nav::top();
  const ui::Screen *base = ui::nav::base();
  if (!top || !base || ui::nav::locked() || top->offFace()) return;
  if (!top->allowsTalk() || !base->allowsTalk()) return;  // the Recorder, a ringing alarm
  if (deskShowing()) return;
  if (base->tag() == moduleTag("COMMS")) return;  // the Comms screen shows the call in place
#if INKDECK_AGENT
  if (agentTalking()) return;
#endif
  ui::nav::push(std::make_unique<CallSheet>());
  ui::nav::flush();  // now, even over a Reader about to nap again
}

}  // namespace apps

#endif  // INKDECK_COMMS
