// A board's mic screen (apps/comms_ui.h): another board's open mic, the latched case (a room).
// Named comms_mics.cpp so nothing here can shadow the engine's comms_mic.h.
#include "features.h"

#if INKDECK_COMMS  // the whole file (features.h)

#include <Arduino.h>
#include <time.h>

#include <algorithm>
#include <string>
#include <vector>

#include "apps/comms_ui.h"
#include "apps/home.h"
#include "audio.h"
#include "input.h"
#include "rtc.h"

namespace apps {
namespace {

using namespace commsui;
using ui::Key;
using ui::Rect;

const std::string kDot = std::string(" ") + ui::glyph::kDot + " ";

// --- A board's mic ------------------------------------------------------------------------------

constexpr int kEventRowH = 30;
constexpr int kChartH = 112;
constexpr int kAxisH = 20;

// The board's open mic, one screen: the state with how long, its level, two hours of levels, its
// board, its LOSTs. Redraws when what it shows changes: the meter at most once a second while its
// voice plays here, the chart at the minute (or while it plays or after a key), a latched mic's
// length in whole minutes.
class MicScreen : public ui::Screen {
 public:
  MicScreen(int group, uint32_t id) : group_(group), id_(id) {
    comms::hold();
    load();
    bars_ = mic_.bars;
    barsMinute_ = minuteNow();
  }
  ~MicScreen() override { comms::release(); }

  std::string tag() const override { return moduleTag("COMMS"); }
  std::string context() const override { return groupName(group_); }
  int hints(ui::Hint *out) const override {
    if (micHeld()) {
      out[0] = micHint();  // ● HOLD
      return 1;
    }
    out[0] = {ui::glyph::kDial, "VOL"};
    out[1] = micHint();  // HOLD BOOT TALK, or BOOT STOP latched
    if (micLatched()) return 2;
    out[2] = {"BOOT", playing() ? "STOP" : "BACK"};
    return 3;
  }
  // HOLD BOOT keys the mic to that board (talks into the room).
  ui::TalkTo talkTo() const override { return micTalkTo(group_, id_); }

  bool onKey(Key key) override {
    switch (key) {
      case Key::Up: audio::changeVolume(+5); return true;
      case Key::Down: audio::changeVolume(-5); return true;
      case Key::Select: return true;  // HOLD BOOT talks; ● while it's held latches (latchKey())
      case Key::Back: return stopIfPlaying();
      default: return false;
    }
  }

  bool onTick() override {
    load();
    // The chart: at the minute, or while its voice plays here or a key came lately.
    const uint32_t minute = minuteNow();
    if (minute != barsMinute_ || (on_ && mic_.playing && !mic_.latched) || keyedLately()) {
      bars_ = mic_.bars;
      barsMinute_ = minute;
    }
    return key() != drawn_;
  }

  void draw(ui::Canvas &c, const Rect &body) override {
    using namespace ui;
    drawn_ = key();
    Rect r = body;
    const comms::OpenMic &m = mic_;
    // The head: whose, and to whom.
    std::string to = m.toMe ? "TO YOU" : m.target ? "TO ONE BOARD" : "TO ALL OF " + groupName(group_);
    drawSectionHeader(c, r.takeTop(kSectionH), name() + "'S MIC", to);

    // The state, large, with how long it has been so; inverted while LOST.
    const Rect block = r.takeTop(122);
    const int x = block.x + kPadX, w = block.w - 2 * kPadX;
    const std::string word = stateWord(), len = stateLength();
    const bool inverted = on_ && m.link == comms::Link::Lost;
    if (inverted) fill(c, {block.x, block.y + 6, block.w, 48}, kInk);
    const uint16_t ink = inverted ? kPaper : kInk;
    const int base = block.y + 6 + 24 + capHeight(style::kTitle) / 2;
    drawText(c, x, base, word, style::kTitle, ink);
    if (!len.empty()) drawText(c, x + w - textWidth(style::kTitle, len), base, len, style::kTitle, ink);
    drawText(c, {x, block.y + 62, w, 17}, stateLine(), style::kLabel);
    drawMicMeter(c, x, block.y + 88, w, 14, "LEVEL", meter());

    // Two hours of it.
    hrule(c, r.x, r.y, r.w);
    r.takeTop(kRule);
    drawSectionHeader(c, r.takeTop(kSectionH), "LAST 2 HOURS", "2-MIN BARS" + kDot + "PEAK");
    drawChart(c, r.takeTop(kChartH + kAxisH + 16));

    // Its board.
    hrule(c, r.x, r.y, r.w);
    r.takeTop(kRule);
    drawSectionHeader(c, r.takeTop(kSectionH), "ITS BOARD", "");
    Rect kv = r.takeTop(3 * 24 + 10);
    kv.takeTop(5);
    const std::string rows[3][2] = {{"POWER", powerText()}, {"REACHED", reached()}, {"HERE", hereText()}};
    for (const auto &row : rows) {
      const Rect line = kv.takeTop(24);
      drawText(c, {line.x + kPadX, line.y, 96, line.h}, row[0], style::kKey);
      drawText(c, {line.x + kPadX + 100, line.y, line.w - 2 * kPadX - 100, line.h}, row[1],
               style::kLabel);
    }

    // Its LOSTs, newest first.
    hrule(c, r.x, r.y, r.w);
    r.takeTop(kRule);
    const int n = static_cast<int>(m.events.size());
    drawSectionHeader(c, r.takeTop(kSectionH), "LOSTS", n ? std::to_string(n) : "NONE");
    if (m.events.empty()) {
      drawText(c, {r.x + kPadX, r.y + 8, r.w - 2 * kPadX, 20},
               "NOT LOST SINCE THIS BOARD HEARD IT", style::kLabel);
    }
    for (const comms::MicEvent &e : m.events) {
      if (r.h < kEventRowH) break;
      drawEvent(c, r.takeTop(kEventRowH), e);
    }
  }

 private:
  static uint32_t minuteNow() {
    return rtc::timeValid() ? static_cast<uint32_t>(time(nullptr) / 60) : millis() / 60000;
  }

  void load() {
    const std::vector<comms::OpenMic> ms = comms::openMics(group_);
    const comms::OpenMic *m = findMic(ms, group_, id_);
    on_ = m != nullptr;
    if (m) mic_ = *m;  // gone (its off came): what it was, OFF
    if (mic_.name.empty()) {
      for (const comms::Board &b : comms::boards(group_)) {
        if (b.id == id_) {
          mic_.name = b.name;
          board_ = b;
        }
      }
    } else {
      for (const comms::Board &b : comms::boards(group_)) {
        if (b.id == id_) board_ = b;
      }
    }
  }

  std::string name() const { return boardName(mic_.name, id_); }

  // OFF, LOST, PAUSED, ON (latched: a room) or TALKING (BOOT held there).
  std::string stateWord() const {
    if (!on_) return "OFF";
    if (mic_.link == comms::Link::Lost) return "LOST";
    if (mic_.paused) return "PAUSED";
    return mic_.latched ? "ON" : "TALKING";
  }
  // How long it has been in that state: LOST in 10 s steps, a latched mic in whole minutes (in
  // seconds for 30 s after a key), a held one in seconds.
  std::string stateLength() const {
    if (!on_) return {};
    if (mic_.link == comms::Link::Lost) return lostText(mic_.linkMs);
    if (mic_.latched && !keyedLately()) return minutesText(mic_.linkMs);
    return lengthText(mic_.linkMs);
  }
  std::string stateLine() const {
    const comms::OpenMic &m = mic_;
    if (!on_) return "ITS MIC WENT OFF";
    std::string since;
    if (m.since && rtc::timeValid()) since = kDot + "SINCE " + whenText(m.since, 0);
    if (m.link == comms::Link::Lost) {
      return "NOT HEARD " + lostText(m.linkMs) + kDot + "ITS MIC WAS ON " +
             minutesText(m.onS * 1000);
    }
    return "MIC ON " + minutesText(m.onS * 1000) + since + kDot +
           (m.latched ? "LATCHED" : "BOOT HELD THERE");
  }
  int meter() const {
    if (!on_ || mic_.link == comms::Link::Lost) return 0;
    // A press moves it; a latched mic (a room) only for 30 s after a key.
    return (mic_.playing && !mic_.latched) || keyedLately() ? meterFill(mic_.levelDb) : 0;
  }
  std::string powerText() const {
    if (mic_.usb) return mic_.battery >= 0 ? "USB" + kDot + std::to_string(mic_.battery) + "%" : "USB";
    return mic_.battery >= 0 ? "BATTERY " + std::to_string(mic_.battery) + "%" : "BATTERY";
  }
  std::string reached() const {
    const std::string r = reachText(id_, mic_.route, mic_.hops, board_.relayName, mic_.rssi);
    if (!r.empty()) return r;
    return mic_.route == comms::Route::Away ? "NOT IN REACH" : "-";
  }
  // Whether its voice plays here.
  std::string hereText() const {
    const comms::Conversation conv = comms::conversation(group_);
    if (!on_) return "-";
    if (!mic_.forMe) return "TO ANOTHER BOARD: NOT FOR THIS ONE";
    if (mic_.heard) return mic_.toMe ? "ITS VOICE PLAYS HERE: IT'S TO YOU" : "ITS VOICE PLAYS HERE";
    if (conv.left) return "YOU LEFT: ITS VOICE DOESN'T PLAY HERE";
    return "NOT IN ITS CONVERSATION";
  }

  // 60 bars of 2 minutes, oldest on the left, each its loudest frame; a bar with no level left
  // empty.
  void drawChart(ui::Canvas &c, const Rect &area) const {
    using namespace ui;
    constexpr int kLabelW = 40;
    const Rect ch{area.x + kPadX + kLabelW, area.y + 10, area.w - 2 * kPadX - kLabelW, kChartH};
    const int n = comms::kHistoryBars;
    const int pitch = ch.w / n, barW = std::max(2, pitch - 2);
    const int x0 = ch.right() - pitch * n;
    auto yOf = [&](int db) {
      return ch.bottom() - std::clamp(db + 60, 0, 60) * ch.h / 60;
    };
    for (int i = 0; i < n && i < static_cast<int>(bars_.size()); ++i) {
      const int v = bars_[i];
      if (v == comms::kNoBar) continue;
      const int top = yOf(v / comms::kMeterStepDb * comms::kMeterStepDb);
      if (top < ch.bottom()) fill(c, {x0 + i * pitch, top, barW, ch.bottom() - top}, kInk);
    }
    // The axis: 30-minute ticks, -2 H on the left, NOW on the right.
    hrule(c, x0, ch.bottom(), pitch * n, 2);
    for (int k = 0; k <= 4; ++k) fill(c, {x0 + k * pitch * n / 4 - (k == 4 ? 2 : 0), ch.bottom(), 2, 6}, kInk);
    const int ay = ch.bottom() + 8;
    drawText(c, {x0, ay, 60, 16}, "\xE2\x88\x92" "2 H", style::kTag);
    drawText(c, {x0 + pitch * n / 2 - 30, ay, 60, 16}, "\xE2\x88\x92" "1 H", style::kTag,
             Align::Center);
    drawText(c, {x0 + pitch * n - 60, ay, 60, 16}, "NOW", style::kTag, Align::Right);
    // dBFS on the left.
    drawText(c, {area.x + kPadX, ch.y - 6, kLabelW - 6, 14}, "0", style::kTag, Align::Right);
    drawText(c, {area.x + kPadX, ch.bottom() - 10, kLabelW - 6, 14}, "\xE2\x88\x92" "60",
             style::kTag, Align::Right);
  }

  std::string barsKey() const {
    std::string k;
    for (const int8_t v : bars_) k += std::to_string(v == comms::kNoBar ? 99 : v / 3) + ",";
    return k;
  }

  // A row: when, LOST inverted (NOW while it lasts), its length.
  std::string eventWhat(const comms::MicEvent &e) const {
    return std::string("LOST") + (e.live ? kDot + "NOW" : "");
  }
  std::string eventLength(const comms::MicEvent &e) const {
    return e.live ? lostText(e.ms) : lengthText(e.ms);
  }
  void drawEvent(ui::Canvas &c, const Rect &r, const comms::MicEvent &e) const {
    using namespace ui;
    dashedH(c, r.x, r.bottom() - 1, r.w);
    const Rect band{r.x + kPadX, r.y, r.w - 2 * kPadX, r.h - 1};
    drawText(c, {band.x, band.y, 64, band.h}, whenText(e.at, e.agoS), style::kKey);
    drawTag(c, band.x + 70, band.centerY(), 19, eventWhat(e), style::kCap, TagKind::Inverted, 4);
    drawText(c, {band.right() - 80, band.y, 80, band.h}, eventLength(e), style::kLabel,
             Align::Right);
  }

  std::string key() const {
    std::string k = context() + "|" + (comms::micEnabled() ? "m" : "") + (playing() ? "p" : "") +
                    micHint().verb + "|" + stateWord() + "|" + stateLength() + "|" + stateLine() +
                    "|" + std::to_string(meter()) + "|" + powerText() + "|" + reached() + "|" +
                    hereText() + "|" + barsKey() + "\n";
    for (size_t i = 0; i < mic_.events.size() && i < 12; ++i) {
      const comms::MicEvent &e = mic_.events[i];
      k += whenText(e.at, e.agoS) + eventWhat(e) + eventLength(e) + (e.live ? "*" : "") + "\n";
    }
    return k;
  }

  int group_;
  uint32_t id_;
  bool on_ = false;
  comms::OpenMic mic_;
  comms::Board board_;
  std::vector<int8_t> bars_;  // the chart as shown
  uint32_t barsMinute_ = 0;
  std::string drawn_;
};

}  // namespace


std::unique_ptr<ui::Screen> commsui::makeMicScreen(int group, uint32_t board) {
  return std::make_unique<MicScreen>(group, board);
}

}  // namespace apps

#endif  // INKDECK_COMMS
