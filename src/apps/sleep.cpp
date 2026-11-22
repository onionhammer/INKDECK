#include "apps/agent_face.h"
#include "apps/off_face.h"
#include "apps/sleep.h"

#include <Arduino.h>
#include <stdio.h>
#include <time.h>

#include <algorithm>

#include "alarms.h"
#include "apps/lock.h"
#include "covers.h"
#include "picture.h"
#include "pmic.h"
#include "power.h"
#include "readstate.h"
#include "rtc.h"
#include "timeutil.h"

namespace apps {
namespace {

using ui::Key;
using ui::Rect;
using ui::TextStyle;

// Sizes, weights and letter-spacing from the Sleep artboard.
constexpr TextStyle kStripTag{&ui::fonts::kMono13ExtraBold, 1};
constexpr TextStyle kStrip{&ui::fonts::kMono13Bold, 1};
constexpr TextStyle kResumeKey{&ui::fonts::kMono13Medium, 1};
constexpr TextStyle kResume{&ui::fonts::kMono14Bold, 1};
constexpr TextStyle kWakeTime{&ui::fonts::kMono80ExtraBold, -4};
constexpr TextStyle kWakeDate{&ui::fonts::kMono15Bold, 1};
constexpr TextStyle kKey{&ui::fonts::kMono14Medium, 1};
constexpr TextStyle kValue{&ui::fonts::kMono14Bold, 0};
constexpr TextStyle kCardLabel{&ui::fonts::kMono13Medium, 1};
constexpr TextStyle kCardTitle{&ui::fonts::kMono15Bold, 0};
constexpr TextStyle kCardPercent{&ui::fonts::kMono15ExtraBold, 0};

constexpr int kPanelW = 292;  // the inverted wake panel
constexpr int kTopH = 40;     // strips, including their 3 px rules
constexpr int kBottomH = 44;
constexpr int kCols = 36;     // dot-matrix field: 36 x 28 cells of 14 px
constexpr int kRows = 28;
constexpr int kCell = 14;

// The book's card at the foot of the panel: its cover (at most 72 x 108, a box of its own in
// the covers cache) in a 2 px paper frame with a 4 px dithered shadow, Home's READER cover
// inverted; beside it // READING, the title and the progress. Rows from the top: label, gap,
// title lines; from the bottom: bar, gap, READ / percent.
constexpr int kCoverW = 72;
constexpr int kCoverH = 108;
constexpr int kCoverFrame = 2;
constexpr int kCoverShadow = 4;
constexpr int kCardGap = 16;  // shadow to text
constexpr int kCardLabelH = 16;
constexpr int kCardTitleLineH = 19;
constexpr int kCardRowGap = 6;
constexpr int kCardPercentH = 19;
constexpr int kCardBarH = 12;
constexpr int kCardMinH = kCardLabelH + kCardRowGap + kCardTitleLineH + kCardRowGap +
                          kCardPercentH + kCardRowGap + kCardBarH;

// The artboard's sprites: '#' ink, '+' 50 % dither, 'o' paper inside the sprite.
const char *const kFrog[] = {
    "    ######      ######    ", "   #++++++#    #++++++#   ", "  #+#++++#+#  #+#++++#+#  ",
    "  #++####++####++####++#  ", " #++++++++++++++++++++++# ", "#++++++++++#++#++++++++++#",
    "#+#++++++++++++++++++++#+#", "#++####################++#", "#+++oooooooooooooooooo+++#",
    " #++oooooooooooooooooo++# ", "#++++#oooooooooooooo#++++#", "#++++#o#oooooooooo#o#++++#",
    "#++++#o#oooooooooo#o#++++#", " #+++#o#oooooooooo#o#+++# ", "  ######################  ",
    "     # # #      # # #     ",
};
const char *const kPad[] = {
    "   ########################   ",
    " ##oooooooooooooooooooooooo## ",
    "   ########################   ",
};
const char *const kZBig[] = {"#####", "...#.", "..#..", ".#...", "#####"};
const char *const kZSmall[] = {"####", "..#.", ".#..", "####"};

using timeutil::kMonthNames;
using timeutil::kWeekdayShort;

using timeutil::hhmm;

// The frog field: which character each cell holds (0 = empty).
class Field {
 public:
  Field() {
    stamp(kFrog, sizeof kFrog / sizeof *kFrog, 3, 8);
    stamp(kPad, sizeof kPad / sizeof *kPad, 1, 24);
    stamp(kZSmall, sizeof kZSmall / sizeof *kZSmall, 28, 6);
    stamp(kZBig, sizeof kZBig / sizeof *kZBig, 31, 1);
  }
  char at(int x, int y) const {
    return x < 0 || y < 0 || x >= kCols || y >= kRows ? 0 : cells_[y][x];
  }

 private:
  void stamp(const char *const *rows, int n, int x0, int y0) {
    for (int r = 0; r < n; ++r) {
      for (int c = 0; rows[r][c]; ++c) {
        const char ch = rows[r][c];
        if (ch != ' ' && ch != '.') cells_[y0 + r][x0 + c] = ch;
      }
    }
  }
  char cells_[kRows][kCols] = {};
};

// Built at boot, not on first use: a function-local static would need a thread-safe guard.
const Field field;

void drawField(ui::Canvas &c, const Rect &area) {
  ui::drawDotField(c, area.x + (area.w - kCols * kCell) / 2, area.y + (area.h - kRows * kCell) / 2,
                   kCols, kRows, kCell, [](int x, int y) { return field.at(x, y); });
}

// 2 px dashed rule, 3 on / 3 off, in `color`.
void dashed2(ui::Canvas &c, int x, int y, int w, uint16_t color) {
  ui::dashedH(c, x, y, w, color);
  ui::dashedH(c, x, y + 1, w, color);
}

class SleepScreen : public OffFaceScreen {
 public:
  SleepScreen() {
    now_ = time(nullptr);
    wakeAt_ = rtc::timeValid() ? alarms::next(now_, &slot_) : 0;
    int snoozeSlot = -1;
    snoozed_ = wakeAt_ && wakeAt_ == alarms::snoozedUntil(&snoozeSlot) && snoozeSlot == slot_;
    detail_ = pmic::detail();
    std::string path, title;
    readstate::Position pos;
    if (readstate::last(path, title, pos)) {
      resume_ = "Reader \xC2\xB7 " + title + " \xC2\xB7 " + std::to_string(pos.percent) + "%";
      title_ = title;
      percent_ = std::min<int>(pos.percent, 100);
      // The first sleep with a book renders its thumbnail (a few hundred ms), later ones read it
      // from the card. No cover, or no card: the panel as it was.
      hasCover_ = covers::thumbnail(path, kCoverW, kCoverH, cover_);
    } else {
      resume_ = "Home";
    }
  }

  std::string tag() const override { return "SLEEP"; }
  bool landscape() const override { return true; }
  // Also when inkharness asks for a check-in (a schedule that wakes the board).
  void powerOff() override { power::off(power::Mode::Sleep, withAgentCheck(wakeAt_)); }

  void draw(ui::Canvas &c, const Rect &body) override {
    using namespace ui;
    drawn();
    const Rect left{body.x, body.y, body.w - kPanelW, body.h};
    const Rect panel{left.right(), body.y, kPanelW, body.h};
    drawLeft(c, left);
    drawPanel(c, panel);
  }

 private:
  void drawLeft(ui::Canvas &c, const Rect &area) const {
    using namespace ui;
    vrule(c, area.right() - kRule, area.y, area.h);
    const Rect inner{area.x, area.y, area.w - kRule, area.h};

    // Top strip: INKDECK/OS, then when it halted.
    const Rect top{inner.x, inner.y, inner.w, kTopH - kRule};
    hrule(c, inner.x, top.bottom(), inner.w);
    const char *tag = "INKDECK/OS";
    const int tagW = textWidth(kStripTag, tag) + 2 * kPadX;
    fill(c, {top.x, top.y, tagW, top.h}, kInk);
    drawText(c, {top.x + kPadX, top.y, tagW - 2 * kPadX, top.h}, tag, kStripTag, Align::Left,
             kPaper);
    std::string halted = "HALTED";
    if (rtc::timeValid()) {
      tm t;
      localtime_r(&now_, &t);
      char buf[48];
      snprintf(buf, sizeof buf, "HALTED %02d:%02d \xC2\xB7 %s %d %s %d", t.tm_hour, t.tm_min,
               kWeekdayShort[t.tm_wday], t.tm_mday, kMonthNames[t.tm_mon], t.tm_year + 1900);
      halted = buf;
    }
    drawText(c, {top.x + tagW + kPadX, top.y, top.w - tagW - 2 * kPadX, top.h}, halted, kStrip);

    // The field, between the strips.
    drawField(c, {inner.x, inner.y + kTopH, inner.w, inner.h - kTopH - kBottomH});

    // Bottom strip: what to come back to.
    const int by = inner.bottom() - kBottomH;
    hrule(c, inner.x, by, inner.w);
    const Rect strip{inner.x + kPadX, by + kRule, inner.w - 2 * kPadX, kBottomH - kRule};
    const std::string key = std::string("RESUME ") + glyph::kPointer;
    const int keyW = textWidth(kResumeKey, key);
    drawText(c, {strip.x, strip.y, keyW, strip.h}, key, kResumeKey);
    drawText(c, {strip.x + keyW + 10, strip.y, strip.w - keyW - 10, strip.h}, resume_, kResume);
  }

  void drawPanel(ui::Canvas &c, const Rect &area) const {
    using namespace ui;
    fill(c, area, kInk);
    const Rect p{area.x + 24, area.y + 30, area.w - 48, area.h - 30 - 24};

    // The wake time.
    int y = p.y + capHeight(kWakeTime);
    std::string date = "NO WAKE SET";
    if (wakeAt_) {
      tm w;
      localtime_r(&wakeAt_, &w);
      drawText(c, p.x, y, hhmm(w.tm_hour, w.tm_min), kWakeTime, kPaper);
      char buf[40];
      snprintf(buf, sizeof buf, "%s %d %s \xC2\xB7 RTC ALARM", kWeekdayShort[w.tm_wday],
               w.tm_mday, kMonthNames[w.tm_mon]);
      date = buf;
    } else {
      drawText(c, p.x, y, "--:--", kWakeTime, kPaper);
    }
    y += 14 + 6;
    drawText(c, {p.x, y, p.w, 19}, date, kWakeDate, Align::Left, kPaper);
    y += 19 + 18;
    dashed2(c, p.x, y, p.w, kPaper);
    y += 2 + 18;

    // REASON / PATH / BATTERY / ALARM.
    std::string reason = "none: PWR key only";
    std::string alarm = "none set";
    if (wakeAt_ && slot_ >= 0) {
      const alarms::Alarm a = alarms::get(slot_);
      reason = (snoozed_ ? "snooze, alarm " : "alarm ") + std::to_string(slot_ + 1);
      alarm = hhmm(a.hour, a.minute) + " " + alarms::repeatLabel(a.days);
    }
    std::string battery = "no battery";
    if (detail_.status.percent >= 0) {
      char buf[32];
      snprintf(buf, sizeof buf, "%d %% \xC2\xB7 %.2f V", detail_.status.percent,
               detail_.vbatMv / 1000.0);
      battery = buf;
    }
    const std::pair<const char *, std::string> rows[] = {
        {"REASON", reason},
        {"PATH", wakeAt_ ? "RTC INT \xE2\x86\x92 PWRON" : "PWR \xE2\x86\x92 PWRON"},
        {"BATTERY", battery},
        {"ALARM", alarm},
    };
    for (const auto &row : rows) {
      drawText(c, {p.x, y, 76, 18}, row.first, kKey, Align::Left, kPaper);
      drawText(c, {p.x + 76, y, p.w - 76, 18}, row.second, kValue, Align::Left, kPaper);
      y += 18 + 10;
    }

    // HOLD [PWR] TO BOOT, at the bottom, and the book's card on top of it.
    const Rect line{p.x, p.bottom() - 24, p.w, 24};
    if (hasCover_) drawCard(c, {p.x, y, p.w, line.y - 14 - y});
    drawHoldPwr(c, line, "TO BOOT", kPaper);
  }

  // The book's card, at the bottom of `area` (ink).
  void drawCard(ui::Canvas &c, const Rect &area) const {
    using namespace ui;
    const Rect box{area.x, 0, cover_.w + 2 * kCoverFrame, cover_.h + 2 * kCoverFrame};
    const int cardH = std::max(box.h, kCardMinH);
    if (cardH + kCoverShadow > area.h) return;
    const int top = area.bottom() - kCoverShadow - cardH;

    // The cover, framed in paper, its shadow a 50 % dither of paper on the ink.
    const Rect cover{box.x, top, box.w, box.h};
    const Rect shadows[] = {{cover.x + kCoverShadow, cover.bottom(), cover.w, kCoverShadow},
                            {cover.right(), cover.y + kCoverShadow, kCoverShadow, cover.h}};
    for (const Rect &s : shadows) {
      fill(c, s, kPaper);
      dither50(c, s);
    }
    frame(c, cover, kCoverFrame, kPaper);
    picture::draw(c, cover.x + kCoverFrame, cover.y + kCoverFrame, cover_);

    // Beside it: // READING and the title from the top, READ / percent and the bar from the
    // bottom; the title gets the lines in between (two, beside a 2:3 cover).
    const int x = cover.right() + kCoverShadow + kCardGap;
    const int w = area.right() - x;
    drawText(c, {x, top, w, kCardLabelH}, "// READING", kCardLabel, Align::Left, kPaper);
    const int titleY = top + kCardLabelH + kCardRowGap;
    const int barY = top + cardH - kCardBarH;
    const int percentY = barY - kCardRowGap - kCardPercentH;
    const int lines = std::max(1, (percentY - kCardRowGap - titleY) / kCardTitleLineH);
    int ly = titleY;
    for (const std::string &l : wrap(kCardTitle, title_, w, lines)) {
      drawText(c, {x, ly, w, kCardTitleLineH}, l, kCardTitle, Align::Left, kPaper);
      ly += kCardTitleLineH;
    }
    drawText(c, {x, percentY, w, kCardPercentH}, "READ", kCardLabel, Align::Left, kPaper);
    drawText(c, {x, percentY, w, kCardPercentH}, std::to_string(percent_) + "%", kCardPercent,
             Align::Right, kPaper);
    const Rect bar{x, barY, w, kCardBarH};
    frame(c, bar, 2, kPaper);
    fill(c, {bar.x + 4, bar.y + 4, (bar.w - 8) * percent_ / 100, bar.h - 8}, kPaper);
  }

  time_t now_ = 0;
  time_t wakeAt_ = 0;
  int slot_ = -1;
  bool snoozed_ = false;
  pmic::Detail detail_;
  std::string resume_;
  std::string title_;  // the book in progress, for its card
  int percent_ = 0;
  bool hasCover_ = false;
  picture::Bitmap cover_;
};

}  // namespace

void sleepNow() {
  const ui::Screen *top = ui::nav::top();
  if (top && top->offFace()) return;  // Sleep, Standby or a closed book already showing
  lockNow();  // under the frog: a key that cancels the power-off lands on the lock
  ui::nav::push(std::make_unique<SleepScreen>());
  ui::nav::invalidate(ui::Refresh::Full);
}

}  // namespace apps
