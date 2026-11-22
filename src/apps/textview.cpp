#include "apps/textview.h"

#include <Preferences.h>

#include <algorithm>

#include "apps/home.h"
#include "flashfs.h"
#include "fonts.h"
#include "paths.h"
#include "readstate.h"
#include "screen.h"
#include "sdcard.h"
#include "textdoc.h"
#include "ui/menu.h"

namespace apps {
namespace {

using ui::Key;
using ui::Rect;

// The Reader's layout (apps/reader.cpp), with the chapter line naming the file.
constexpr int kStripH = 30;  // progress strip, including its 3 px rule
constexpr int kPadTop = 22;
constexpr int kPadX = 30;
constexpr int kPadBottom = 8;
constexpr int kHeadLineH = 17;
constexpr int kHeadGap = 14;
constexpr ui::TextStyle kHeadStyle{&ui::fonts::kMono13Bold, 2};
constexpr ui::TextStyle kStripStyle{&ui::fonts::kMono13Bold, 1};
constexpr int kThumbW = 5;  // scrollbar thumb, centred in the right margin
constexpr int kMinThumbH = 16;

constexpr int kBodyH = screen::kHeight - ui::kStatusBarH - ui::kHintBarH;
constexpr int kTextW = screen::kWidth - 2 * kPadX;
constexpr int kTextH = kBodyH - kStripH - kPadTop - kHeadLineH - kHeadGap - kPadBottom;

// How far one step of the dial scrolls; a setting for all files.
enum class Step : uint8_t { Line, Quarter, Half };
constexpr int kStepCount = 3;
constexpr const char *kPrefs = "textview";

Step loadStep() {
  Preferences prefs;
  if (!prefs.begin(kPrefs, false)) return Step::Quarter;  // read-only fails until it exists
  const uint8_t v = prefs.isKey("step") ? prefs.getUChar("step") : 1;
  prefs.end();
  return static_cast<Step>(v % kStepCount);
}

void saveStep(Step step) {
  Preferences prefs;
  if (!prefs.begin(kPrefs, false)) return;
  prefs.putUChar("step", static_cast<uint8_t>(step));
  prefs.end();
}

using paths::folderOf;
using paths::nameOf;

// The menu's FOLDER value, named as Files names it.
std::string folderLabel(const std::string &path) {
  const std::string folder = folderOf(path);
  if (folder == sdcard::kMountPoint) return "SD CARD";
  if (folder == flashfs::kMountPoint) return "ONBOARD FLASH";
  return ui::upper(nameOf(folder));
}

std::string bytesText(uint64_t n) {
  char buf[16];
  if (n < 1024) {
    snprintf(buf, sizeof buf, "%u B", static_cast<unsigned>(n));
  } else if (n < 1024 * 1024) {
    snprintf(buf, sizeof buf, "%u KB", static_cast<unsigned>((n + 512) / 1024));
  } else {
    snprintf(buf, sizeof buf, "%.1f MB", n / (1024.0 * 1024.0));
  }
  return buf;
}

class TextScreen : public ui::Screen {
 public:
  explicit TextScreen(std::string path) : path_(std::move(path)), step_(loadStep()) {}

  bool open(std::string &err) {
    if (!doc_.open(path_, err)) return false;
    readstate::Position pos;
    if (readstate::load(path_, pos)) {
      size_ = pos.fontSize % fonts::kReaderSizeCount;
      mono_ = pos.chapter & 1;
    } else {
      mono_ = !TextDoc::proseName(path_);  // code, data and logs line up in monospace
      if (mono_) size_ = 0;                // Mono 14: about 50 columns
    }
    layout(pos.offset);
    return true;
  }

  std::string tag() const override { return moduleTag("FILES"); }
  std::string context() const override { return ui::upper(nameOf(path_)); }
  int hints(ui::Hint *out) const override {
    out[0] = {ui::glyph::kDial, "SCROLL"};
    out[1] = {ui::glyph::kPress, "MENU"};
    out[2] = {"BOOT", "BACK"};
    return 3;
  }

  bool reading() const override { return true; }  // light sleep between scrolls
  void onNap() override { save(); }
  void onLeave() override { save(); }  // covered by the menu, or closed

  bool onKey(Key key) override {
    switch (key) {
      case Key::Up: return scrollBy(-stepLines());
      case Key::Down: return scrollBy(+stepLines());
      case Key::Select: openMenu(); return true;
      default: return false;
    }
  }

  void draw(ui::Canvas &c, const Rect &body) override {
    Rect r = body;
    drawStrip(c, r.takeBottom(kStripH));
    drawHeadLine(c, {r.x + kPadX, r.y + kPadTop, r.w - 2 * kPadX, kHeadLineH});
    const int textY = r.y + kPadTop + kHeadLineH + kHeadGap;
    if (doc_.empty()) {
      ui::drawText(c, {r.x + kPadX, textY, kTextW, 40}, "EMPTY FILE", ui::style::kLabel,
                   ui::Align::Center);
      return;
    }
    const GFXfont *f = font();
    // Capitals centred in the line, as the Reader's typesetter does.
    const int baseline = (lineH() - f->glyph['H' - f->first].yOffset) / 2;
    const int last = std::min(doc_.lineCount(), top_ + visible());
    for (int i = top_; i < last; ++i) {
      doc_.drawLine(c, i, r.x + kPadX, textY + (i - top_) * lineH() + baseline, !mono_);
    }
    drawScrollbar(c, {r.right() - kPadX / 2 - kThumbW / 2, textY, kThumbW, visible() * lineH()});
  }

 private:
  const FontFamily &family() const {
    return (mono_ ? fonts::kTextMono : fonts::kReader)[size_];
  }
  const GFXfont *font() const { return family().face[0]; }
  int lineH() const { return family().lineHeight; }
  int visible() const { return std::max(1, kTextH / lineH()); }
  int maxTop() const { return std::max(0, doc_.lineCount() - visible()); }
  float progress() const { return maxTop() ? static_cast<float>(top_) / maxTop() : 1.0f; }
  uint32_t offset() const { return doc_.lineCount() ? doc_.lineStart(top_) : 0; }

  int stepLines() const {
    int n = step_ == Step::Line ? 1 : step_ == Step::Quarter ? visible() / 4 : visible() / 2;
    if (input::lastKeyFast()) n = std::max(n, visible() / 2);  // the dial held a while
    return std::max(1, n);
  }

  bool scrollBy(int delta) {
    const int top = std::max(0, std::min(top_ + delta, maxTop()));
    if (top == top_) return false;
    top_ = top;
    return true;
  }

  // Lays the text out in the current font, with the line holding `at` on top.
  void layout(uint32_t at) {
    doc_.layout(font(), kTextW);
    top_ = std::min(doc_.lineAt(at), maxTop());
  }

  void save() const {
    readstate::Position pos;
    pos.chapter = mono_ ? 1 : 0;
    pos.offset = offset();
    pos.fontSize = static_cast<uint8_t>(size_);
    pos.percent = static_cast<uint8_t>(progress() * 100);
    readstate::save(path_, nameOf(path_), pos, false);  // not the Reader's last book
  }

  // "§ NAME.TXT - - - - 12 KB", like the Reader's chapter line.
  void drawHeadLine(ui::Canvas &c, const Rect &line) const {
    const std::string right =
        doc_.truncated() ? "FIRST " + bytesText(doc_.bytesShown()) + " OF " +
                               bytesText(doc_.fileBytes())
                         : bytesText(doc_.fileBytes());
    const int rightW = ui::textWidth(kHeadStyle, right);
    const std::string left = ui::ellipsize(
        kHeadStyle, std::string(ui::glyph::kSection) + " " + ui::upper(nameOf(path_)),
        line.w - rightW - 36);
    const int base = ui::baselineCentered(kHeadStyle, line.y, line.h);
    const int dashFrom = ui::drawText(c, line.x, base, left, kHeadStyle) + 12;
    const int dashTo = line.right() - rightW - 12;
    ui::drawText(c, line.right() - rightW, base, right, kHeadStyle);
    if (dashTo > dashFrom) ui::dashedH(c, dashFrom, line.centerY(), dashTo - dashFrom);
  }

  // "LN 120/2400 [=====:::::] 5%": the top line, how far down.
  void drawStrip(ui::Canvas &c, const Rect &strip) const {
    ui::hrule(c, strip.x, strip.y, strip.w);
    const Rect band{strip.x + ui::kPadX, strip.y + ui::kRule, strip.w - 2 * ui::kPadX,
                    strip.h - ui::kRule};
    const std::string left = "LN " + std::to_string(doc_.lineCount() ? top_ + 1 : 0) + "/" +
                             std::to_string(doc_.lineCount());
    const std::string right = std::to_string(static_cast<int>(progress() * 100)) + "%";
    const int leftW = ui::textWidth(kStripStyle, left);
    const int rightW = ui::textWidth(kStripStyle, right);
    const int base = ui::baselineCentered(kStripStyle, band.y, band.h);
    ui::drawText(c, band.x, base, left, kStripStyle);
    ui::drawText(c, band.right() - rightW, base, right, kStripStyle);
    ui::drawProgress(c, {band.x + leftW + 10, band.centerY() - 7, band.w - leftW - rightW - 20, 14},
                     progress());
  }

  // A dashed track with a solid thumb the size of the screen's share of the text. None when
  // it all fits.
  void drawScrollbar(ui::Canvas &c, const Rect &track) const {
    if (doc_.lineCount() <= visible()) return;
    ui::dashedV(c, track.x + track.w / 2, track.y, track.h);
    const int thumbH = std::max(kMinThumbH, track.h * visible() / doc_.lineCount());
    const int y = track.y + static_cast<int>((track.h - thumbH) * progress());
    ui::fill(c, {track.x, y, track.w, thumbH}, ui::kInk);
  }

  void openMenu() {
    TextScreen *t = this;  // below the menu, so it outlives it
    std::vector<ui::MenuItem> items;
    items.push_back({"RESUME", nullptr, [] { ui::nav::pop(); }});
    items.push_back({"TEXT SIZE", [t] { return ui::upper(t->family().name); },
                     [t] {
                       const uint32_t at = t->offset();
                       t->size_ = (t->size_ + 1) % fonts::kReaderSizeCount;
                       t->layout(at);
                     }});
    items.push_back({"FONT", [t] { return std::string(t->mono_ ? "MONO" : "SERIF"); },
                     [t] {
                       const uint32_t at = t->offset();
                       t->mono_ = !t->mono_;
                       t->layout(at);
                     }});
    items.push_back({"SCROLL",
                     [t] {
                       return std::string(t->step_ == Step::Line      ? "1 LINE"
                                          : t->step_ == Step::Quarter ? "1/4 SCREEN"
                                                                      : "1/2 SCREEN");
                     },
                     [t] {
                       t->step_ = static_cast<Step>((static_cast<int>(t->step_) + 1) % kStepCount);
                       saveStep(t->step_);
                     }});
    items.push_back({"TO START", nullptr, [t] {
                       t->top_ = 0;
                       ui::nav::pop();
                     }});
    const std::string path = path_;
    items.push_back({"FOLDER", [path] { return folderLabel(path); },
                     [] {
                       ui::nav::pop();  // the menu
                       ui::nav::pop();  // back to the Files screen that opened the file
                     }});
    ui::nav::push(std::make_unique<ui::MenuScreen>("TEXT", std::move(items)));
  }

  std::string path_;
  Step step_;
  TextDoc doc_;
  int size_ = fonts::kDefaultReaderSize;
  bool mono_ = false;
  int top_ = 0;  // first line on screen
};

}  // namespace

std::unique_ptr<ui::Screen> makeTextReader(const std::string &path, std::string &err) {
  auto screen = std::make_unique<TextScreen>(path);
  if (!screen->open(err)) return nullptr;
  return screen;
}

}  // namespace apps
