#include "apps/viewer.h"

#include <Arduino.h>

#include <algorithm>

#include "apps/home.h"
#include "picture.h"

namespace apps {
namespace {

using ui::Key;
using ui::Rect;

constexpr int kMaxGrow = 2;  // small pictures stay small rather than turning to blocks

std::string baseName(const std::string &path) { return path.substr(path.rfind('/') + 1); }

class ViewerScreen : public ui::Screen {
 public:
  ViewerScreen(std::vector<std::string> paths, int index,
               std::function<void(const std::string &)> shown)
      : paths_(std::move(paths)), shown_(std::move(shown)) {
    show(std::max(0, std::min(index, static_cast<int>(paths_.size()) - 1)));
  }

  std::string tag() const override { return moduleTag("FILES"); }
  std::string context() const override {
    const std::string name = baseName(paths_[index_]);
    if (paths_.size() < 2) return name;
    char count[32];  // room for any two ints, so the compiler can see nothing is cut off
    snprintf(count, sizeof count, "%02d/%02d ", index_ + 1, static_cast<int>(paths_.size()));
    return count + name;
  }
  bool chrome() const override { return chrome_; }
  bool landscape() const override { return landscape_; }
  bool reading() const override { return true; }

  int hints(ui::Hint *out) const override {
    int n = 0;
    if (paths_.size() > 1) {
      out[n++] = {ui::glyph::kUp, "PREV"};
      out[n++] = {ui::glyph::kDown, "NEXT"};
    }
    out[n++] = {ui::glyph::kPress, "HIDE"};
    out[n++] = {"BOOT", "BACK"};
    return n;
  }

  bool onKey(Key key) override {
    switch (key) {
      case Key::Up: return step(-1);
      case Key::Down: return step(+1);
      case Key::Select: chrome_ = !chrome_; return true;
      default: return false;  // BOOT: the navigator goes back to Files
    }
  }

  void draw(ui::Canvas &c, const Rect &body) override {
    // Fitted to the whole screen, so showing or hiding the chrome doesn't change it; the chrome
    // bands cover its edges. Rendered once per picture (and size), then drawn from the bitmap.
    const int cw = c.width(), ch = c.height();
    int w = 0, h = 0;
    if (probed_) {
      picture::fit(w_, h_, std::min(cw, w_ * kMaxGrow), std::min(ch, h_ * kMaxGrow), w, h);
    }
    if (probed_ && !failed_ && (bitmap_.w != w || bitmap_.h != h)) {
      bitmap_ = {};
      failed_ = !picture::renderFile(paths_[index_], w, h, bitmap_);
      if (failed_) log_w("viewer: %s: couldn't decode", paths_[index_].c_str());
    }
    if (!probed_ || failed_) {
      drawError(c, body);
      return;
    }
    picture::draw(c, (cw - w) / 2, (ch - h) / 2, bitmap_, body.y, body.y + body.h);
  }

 private:
  // Reads the picture's header now (the orientation has to be known before the frame is
  // drawn); draw() decodes it, once, however fast the dial turns.
  void show(int index) {
    index_ = index;
    bitmap_ = {};
    failed_ = false;
    probed_ = picture::probeFile(paths_[index_], w_, h_);
    if (probed_) {
      landscape_ = w_ > h_;
    } else {
      log_w("viewer: %s: not a JPEG or PNG we can read", paths_[index_].c_str());
    }
    if (shown_) shown_(paths_[index_]);
  }

  bool step(int delta) {
    const int next = index_ + delta;
    if (next < 0 || next >= static_cast<int>(paths_.size())) return false;
    show(next);
    return true;
  }

  void drawError(ui::Canvas &c, const Rect &body) const {
    const Rect p = body.inset(20, 24);
    int y = p.y + ui::capHeight(ui::style::kTitle);
    ui::drawText(c, p.x, y, "CAN'T SHOW", ui::style::kTitle);
    y += 24;
    const std::string text = baseName(paths_[index_]) +
                             " couldn't be decoded. JPEG and PNG work; a PNG up to 2400 px wide "
                             "and 4 megapixels.";
    for (const auto &line : ui::wrap(ui::style::kBody, text, p.w, 4)) {
      y += 22;
      ui::drawText(c, p.x, y, line, ui::style::kBody);
    }
  }

  std::vector<std::string> paths_;
  std::function<void(const std::string &)> shown_;
  int index_ = 0;
  bool probed_ = false;  // the header was read: w_ x h_ is the size as shown
  bool failed_ = false;  // the header was fine, the picture wasn't
  int w_ = 0;
  int h_ = 0;
  bool landscape_ = false;
  bool chrome_ = true;
  picture::Bitmap bitmap_;
};

}  // namespace

std::unique_ptr<ui::Screen> makeViewer(std::vector<std::string> paths, int index,
                                       std::function<void(const std::string &)> shown) {
  if (paths.empty()) return nullptr;
  return std::make_unique<ViewerScreen>(std::move(paths), index, std::move(shown));
}

}  // namespace apps
