#include "apps/closed_book.h"

#include <Arduino.h>
#include <time.h>

#include <string>
#include <vector>

#include "alarms.h"
#include "apps/home.h"
#include "apps/lock.h"
#include "covers.h"
#include "picture.h"
#include "power.h"
#include "readstate.h"
#include "rtc.h"
#include "screen.h"
#include "ui/nav.h"

namespace apps {
namespace {

using ui::Key;
using ui::Rect;
using ui::TextStyle;

constexpr int kBandH = 44;  // the Sleep screen's bottom strip, including its 3 px rule
constexpr TextStyle kBand{&ui::fonts::kMono14Bold, 1};       // Sleep's "HOLD [PWR] 1 s TO BOOT"
constexpr TextStyle kBandCap{&ui::fonts::kMono14ExtraBold, 0};
constexpr int kCapH = 24;
// A book without a cover picture: its title, set like one.
constexpr int kPlainInset = 24;
constexpr int kPlainPad = 28;
constexpr int kTitleLines = 9;

class ClosedBookScreen;
const ClosedBookScreen *showing = nullptr;  // while it's on the stack

class ClosedBookScreen : public ui::Screen {
 public:
  explicit ClosedBookScreen(bool devWake) {
    showing = this;
    const time_t now = time(nullptr);
    if (rtc::timeValid()) {
      wakeAt_ = alarms::next(now);
      if (devWake && (!wakeAt_ || wakeAt_ > now + 60)) wakeAt_ = now + 60;
    }
    std::string path;
    readstate::Position pos;
    if (readstate::last(path, title_, pos)) {
      percent_ = pos.percent;
      covers::full(path, screen::kWidth, screen::kHeight - kBandH, cover_);
    }
  }
  ~ClosedBookScreen() override {
    if (showing == this) showing = nullptr;
  }

  std::string tag() const override { return moduleTag("READER"); }
  bool chrome() const override { return false; }
  bool offFace() const override { return true; }
  bool onKey(Key) override {  // a key in the second before power-off: back to the page
    ui::nav::pop();
    return true;
  }
  bool onTick() override {
    // The first tick can come straight after the refresh; wait for the next, so a key has a
    // second to cancel.
    if (!drawn_ || ++ticks_ < 2) return false;
    power::off(power::Mode::Book, wakeAt_);
    ui::nav::pop();  // still here: the power-off failed
    return true;
  }

  void draw(ui::Canvas &c, const Rect &body) override {
    Rect r = body;
    drawBand(c, r.takeBottom(kBandH));
    if (cover_.w > 0) {
      // Centred; a cover that fills the area is a little larger than it and gets clipped.
      picture::draw(c, r.x + (r.w - cover_.w) / 2, r.y + (r.h - cover_.h) / 2, cover_, r.y,
                    r.bottom());
    } else {
      drawPlainCover(c, r);
    }
    drawn_ = true;
  }

 private:
  // "HOLD [PWR] 1 s TO RESUME" on ink, and the place in the book on the right.
  void drawBand(ui::Canvas &c, const Rect &band) const {
    using namespace ui;
    // A paper rule first, so a dark cover doesn't run into the band.
    const Rect ink{band.x, band.y + kRule, band.w, band.h - kRule};
    fill(c, ink, kInk);
    const Rect line{ink.x + kPadX, ink.y + (ink.h - kCapH) / 2, ink.w - 2 * kPadX, kCapH};
    const int base = baselineCentered(kBand, line.y, line.h);
    int x = drawText(c, line.x, base, "HOLD", kBand, kPaper) + 8;
    const int capW = textWidth(kBandCap, "PWR") + 14 + 4;
    frame(c, {x, line.y, capW, line.h}, 2, kPaper);
    drawText(c, x + 9, baselineCentered(kBandCap, line.y, line.h), "PWR", kBandCap, kPaper);
    x += capW + 8;
    x = drawText(c, x, base, "1 s TO RESUME", kBand, kPaper);
    if (percent_ >= 0) {
      drawText(c, {x + 10, line.y, line.right() - x - 10, line.h}, std::to_string(percent_) + "%",
               kBand, Align::Right, kPaper);
    }
  }

  // No cover picture: a frame with a dithered shadow, "§ BOOK", and the title in the largest
  // type that holds letters.
  void drawPlainCover(ui::Canvas &c, const Rect &area) const {
    using namespace ui;
    const int shadow = 6;
    const Rect box{area.x + kPlainInset, area.y + kPlainInset, area.w - 2 * kPlainInset - shadow,
                   area.h - 2 * kPlainInset - shadow};
    dither50(c, {box.x + shadow, box.bottom(), box.w, shadow});
    dither50(c, {box.right(), box.y + shadow, shadow, box.h});
    frame(c, box, kRule);
    const Rect p = box.inset(kPlainPad, kPlainPad);
    drawText(c, {p.x, p.y, p.w, 17}, std::string(glyph::kSection) + " BOOK", style::kLabel);
    dashedH(c, p.x, p.y + 17 + 10, p.w);
    const int pitch = lineHeight(style::kTitle) + 6;
    int base = p.y + 17 + 10 + 34 + capHeight(style::kTitle);
    const std::string title = title_.empty() ? "UNTITLED" : upper(title_);
    for (const std::string &text : wrap(style::kTitle, title, p.w, kTitleLines)) {
      drawText(c, p.x, base, text, style::kTitle);
      base += pitch;
    }
    hrule(c, p.x, base - pitch + 22, 96);
  }

  time_t wakeAt_ = 0;
  std::string title_;
  int percent_ = -1;
  picture::Bitmap cover_;
  bool drawn_ = false;
  int ticks_ = 0;  // since drawn
};

}  // namespace

void closeBook(bool devWake) {
  const ui::Screen *top = ui::nav::top();
  if (top && top->offFace()) return;  // Sleep, Standby or the cover already showing
  // The Reader saves its place, which the cover's band and the next start read.
  if (ui::Screen *base = ui::nav::base()) base->onNap();
  lockNow();  // under the cover: a key that cancels the power-off lands on the lock
  ui::nav::push(std::make_unique<ClosedBookScreen>(devWake));
  ui::nav::invalidate(ui::Refresh::Full);
}

bool reopenBook() {
  if (!showing || ui::nav::top() != showing) return false;
  ui::nav::pop();
  return true;
}

}  // namespace apps
