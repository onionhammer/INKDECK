// The mic's two screens (apps/comms_ui.h): the MIC LEVEL sheet (this board's threshold, against
// its mic's level) and a board's mic screen (another board's open mic, the long-open case: a
// room). Named comms_mics.cpp so nothing here can shadow the engine's comms_mic.h.
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

std::string dbfs(int db) { return ui::signedNumber(static_cast<float>(db), 0) + " dBFS"; }

// --- MIC LEVEL ----------------------------------------------------------------------------------

// The threshold: this board's mic meter with the ▲ where it is, ▲▼ moving it 3 dB a step. The
// meter moves while the mic is on (● switches it, to the talk screen's row); it follows the mic
// for 30 s after a key, then only while a sound goes, so a sheet left up doesn't refresh the panel
// every second.
class MicLevelSheet : public ui::Screen {
 public:
  MicLevelSheet(int group, uint32_t target) : group_(group), target_(target) {}

  std::string tag() const override { return {}; }  // the Comms screen's chrome
  bool overlay() const override { return true; }
  int hints(ui::Hint *out) const override {
    out[0] = {ui::glyph::kDial, "LEVEL"};
    out[1] = {ui::glyph::kPress, comms::micEnabled() ? "MIC OFF" : "MIC ON"};
    out[2] = {"BOOT", "DONE"};
    return 3;
  }

  bool onKey(Key key) override {
    switch (key) {
      case Key::Up:
      case Key::Down: {
        const int step = key == Key::Up ? comms::kMicStepDb : -comms::kMicStepDb;
        const int db = std::clamp(comms::micThreshold() + step, comms::kMicMinDb, comms::kMicMaxDb);
        comms::setMicThreshold(db);
        shownDb_ = db;  // the engine keeps it from its task: shown at once
        return true;
      }
      case Key::Select: toggleMic(group_, target_); return true;
      default: return false;  // BOOT: done
    }
  }

  bool onTick() override { return key() != drawn_; }

  void draw(ui::Canvas &c, const Rect &body) override {
    using namespace ui;
    drawn_ = key();
    const comms::Mic m = comms::mic();
    const int db = threshold();
    constexpr int kH = kRule + kSectionH + 132;
    Rect r{body.x, body.bottom() - kH, body.w, kH};
    fill(c, {r.x, r.y - 6, r.w, 6}, kPaper);
    dither50(c, {r.x, r.y - 6, r.w, 6});
    fill(c, r, kPaper);
    hrule(c, r.x, r.y, r.w);
    r.takeTop(kRule);
    const std::string err = attemptError();
    if (!err.empty()) {
      drawNotice(c, r.takeTop(kSectionH), err);
    } else {
      drawSectionHeader(c, r.takeTop(kSectionH), "MIC LEVEL", dbfs(db));
    }
    const int x = r.x + kPadX, w = r.w - 2 * kPadX;
    int y = r.y + 16;
    drawMicMeter(c, x, y, w, 18, "MIC", meter(m), db);
    y += 18 + 12 + 14;
    drawText(c, {x, y, w, 17}, "SENDS ABOVE " + std::string(glyph::kUp) + " " + dbfs(db),
             style::kLabel);
    y += 22;
    std::string now;
    if (!comms::micEnabled()) {
      now = "YOUR MIC IS OFF: \xE2\x97\x8F SWITCHES IT ON TO SEE IT";
    } else if (!m.paused.empty() && m.paused != "STARTING") {
      now = "PAUSED" + kDot + pausedText(m.paused);
    } else if (m.sending) {
      now = "SENDING" + kDot + "LOUDER THAN THE LEVEL";
    } else {
      now = "YOUR MIC" + kDot + "QUIET: NOTHING GOES";
    }
    drawText(c, {x, y, w, 17}, now, style::kLabel);
    y += 22;
    drawText(c, {x, y, w, 17}, "HIGHER SENDS LESS OF THE ROOM; 3 dB A STEP", style::kLabel);
  }

 private:
  int threshold() const { return shownDb_ > -100 ? shownDb_ : comms::micThreshold(); }
  int meter(const comms::Mic &m) const {
    return m.on && (m.sending || keyedLately()) ? meterFill(m.levelDb) : 0;
  }
  std::string key() const {
    const comms::Mic m = comms::mic();
    return std::to_string(threshold()) + "|" + std::to_string(meter(m)) + "|" +
           (comms::micEnabled() ? "m" : "") + (m.sending ? "s" : "") + m.paused + "|" +
           attemptError();
  }

  int group_;
  uint32_t target_;
  int shownDb_ = -1000;
  std::string drawn_;
};

// --- A board's mic ------------------------------------------------------------------------------

constexpr int kEventRowH = 30;
constexpr int kChartH = 112;
constexpr int kAxisH = 20;

// The board's open mic, one screen: the state with how long, its level against its threshold, two
// hours of levels, its board, its events. Redraws when what it shows changes: the meter at most
// once a second while a sound comes, the chart at the minute (or with a sound or a key), lengths
// in whole minutes while it's quiet.
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
    out[0] = {ui::glyph::kDial, "VOL"};
    out[1] = {ui::glyph::kPress, comms::micEnabled() ? "MIC OFF" : "MIC ON"};
    out[2] = {"BOOT", playing() ? "STOP" : "BACK"};
    return 3;
  }

  bool onKey(Key key) override {
    switch (key) {
      case Key::Up: audio::changeVolume(+5); return true;
      case Key::Down: audio::changeVolume(-5); return true;
      case Key::Select: toggleMic(group_, id_); return true;  // to it: talks into the room
      case Key::Back: return stopIfPlaying();
      default: return false;
    }
  }

  bool onTick() override {
    load();
    // The chart: at the minute, or while a sound comes or a key came lately.
    const uint32_t minute = minuteNow();
    if (minute != barsMinute_ || (on_ && mic_.link == comms::Link::Sound) || keyedLately()) {
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
    drawMicMeter(c, x, block.y + 88, w, 14, "LEVEL", meter(), m.thresholdDb);

    // Two hours of it.
    hrule(c, r.x, r.y, r.w);
    r.takeTop(kRule);
    drawSectionHeader(c, r.takeTop(kSectionH), "LAST 2 HOURS",
                      "2-MIN BARS" + kDot + std::string(glyph::kUp) + " " + dbfs(m.thresholdDb));
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

    // Its sounds and LOSTs, newest first.
    hrule(c, r.x, r.y, r.w);
    r.takeTop(kRule);
    const int n = static_cast<int>(m.events.size());
    drawSectionHeader(c, r.takeTop(kSectionH), "EVENTS", n ? std::to_string(n) : "NONE YET");
    if (m.events.empty()) {
      drawText(c, {r.x + kPadX, r.y + 8, r.w - 2 * kPadX, 20},
               "SOUNDS OVER ITS LEVEL, AND LOSTS, SINCE THIS BOARD HEARD IT", style::kLabel);
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

  std::string stateWord() const {
    if (!on_) return "OFF";
    switch (mic_.link) {
      case comms::Link::Quiet: return mic_.paused ? "PAUSED" : "QUIET";
      case comms::Link::Sound: return "SOUND";
      case comms::Link::Lost: return "LOST";
    }
    return "";
  }
  // How long it has been in that state: seconds while a sound comes, LOST in 10 s steps, whole
  // minutes while quiet.
  std::string stateLength() const {
    if (!on_) return {};
    switch (mic_.link) {
      case comms::Link::Sound: return lengthText(mic_.linkMs);
      case comms::Link::Lost: return lostText(mic_.linkMs);
      case comms::Link::Quiet: break;
    }
    return minutesText(mic_.linkMs);
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
    return "MIC ON " + minutesText(m.onS * 1000) + since + kDot + "SENDS ABOVE " +
           dbfs(m.thresholdDb);
  }
  int meter() const {
    if (!on_ || mic_.link == comms::Link::Lost) return 0;
    return mic_.link == comms::Link::Sound || keyedLately() ? meterFill(mic_.levelDb) : 0;
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

  // 60 bars of 2 minutes, oldest on the left: the part over the threshold solid, under it
  // dithered; the threshold dashed across; a bar with no level left empty.
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
    const int yt = yOf(mic_.thresholdDb);
    for (int i = 0; i < n && i < static_cast<int>(bars_.size()); ++i) {
      const int v = bars_[i];
      if (v == comms::kNoBar) continue;
      const int top = yOf(v / comms::kMicStepDb * comms::kMicStepDb);
      const int bx = x0 + i * pitch;
      if (top < yt) fill(c, {bx, top, barW, yt - top}, kInk);
      const int dTop = std::max(top, yt);
      if (dTop < ch.bottom()) dither50(c, {bx, dTop, barW, ch.bottom() - dTop});
    }
    // The threshold, dashed, 2 px.
    dashedH(c, x0, yt, pitch * n);
    dashedH(c, x0, yt + 1, pitch * n);
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
    drawText(c, {area.x + kPadX, yt - 7, kLabelW - 6, 14}, glyph::kUp, style::kTag, Align::Right);
  }

  std::string barsKey() const {
    std::string k;
    for (const int8_t v : bars_) k += std::to_string(v == comms::kNoBar ? 99 : v / 3) + ",";
    return k;
  }

  // A row: when, SOUND · LOUD (or SOFT) or LOST inverted, its length.
  std::string eventWhat(const comms::MicEvent &e) const {
    if (e.lost) return "LOST";
    return std::string("SOUND") + kDot + (e.loud ? "LOUD" : "SOFT");
  }
  std::string eventLength(const comms::MicEvent &e) const {
    if (!e.live) return lengthText(e.ms);
    return e.lost ? lostText(e.ms) : lengthText(e.ms);
  }
  void drawEvent(ui::Canvas &c, const Rect &r, const comms::MicEvent &e) const {
    using namespace ui;
    dashedH(c, r.x, r.bottom() - 1, r.w);
    const Rect band{r.x + kPadX, r.y, r.w - 2 * kPadX, r.h - 1};
    drawText(c, {band.x, band.y, 64, band.h}, whenText(e.at, e.agoS), style::kKey);
    const std::string what = eventWhat(e) + (e.live ? kDot + "NOW" : "");
    if (e.lost) {
      drawTag(c, band.x + 70, band.centerY(), 19, what, style::kCap, TagKind::Inverted, 4);
    } else {
      drawText(c, {band.x + 70, band.y, band.w - 150, band.h}, what, style::kLabel);
    }
    drawText(c, {band.right() - 80, band.y, 80, band.h}, eventLength(e), style::kLabel,
             Align::Right);
  }

  std::string key() const {
    std::string k = context() + "|" + (comms::micEnabled() ? "m" : "") + (playing() ? "p" : "") +
                    "|" + stateWord() + "|" + stateLength() + "|" + stateLine() + "|" +
                    std::to_string(meter()) + "|" + std::to_string(mic_.thresholdDb) + "|" +
                    powerText() + "|" + reached() + "|" + hereText() + "|" + barsKey() + "\n";
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

std::unique_ptr<ui::Screen> commsui::makeMicLevelSheet(int group, uint32_t target) {
  return std::make_unique<MicLevelSheet>(group, target);
}

std::unique_ptr<ui::Screen> commsui::makeMicScreen(int group, uint32_t board) {
  return std::make_unique<MicScreen>(group, board);
}

}  // namespace apps

#endif  // INKDECK_COMMS
