#include "apps/viewer.h"

#include <Arduino.h>
#include <math.h>

#include <algorithm>

#include "apps/home.h"
#include "paths.h"
#include "picture.h"
#include "sensor_hub.h"

namespace apps {
namespace {

using ui::Key;
using ui::Rect;

constexpr int kMaxGrow = 2;  // small pictures stay small rather than turning to blocks
constexpr int kSettleTicks = 2;  // a new pose seen on this many ticks in a row (1 s apart) counts


// How the board is held.
enum class Pose : uint8_t {
  Unclear,    // being moved, part way between the two, upside down, or no IMU
  Flat,       // lying within 30° of flat: the screen stays as it is
  Portrait,   // upright
  Landscape,  // turned a quarter clockwise, the wheel on top (screen.h's landscape)
};

const char *poseName(Pose p) {
  switch (p) {
    case Pose::Flat: return "flat";
    case Pose::Portrait: return "portrait";
    case Pose::Landscape: return "landscape";
    default: return "unclear";
  }
}

// From gravity in screen axes (sensors::motion(); portrait X right, Y up, Z out of the
// screen). At rest the accelerometer reads 1 g straight up, so the raised edge reads positive:
// +Y with the portrait top up, -X with the portrait left edge up, which is landscape's top.
Pose sensePose() {
  const qmi8658::Reading m = sensors::motion();
  if (!m.ok) return Pose::Unclear;
  const float x = m.acc[0], y = m.acc[1], z = m.acc[2];
  const float g = sqrtf(x * x + y * y + z * z);
  // Far from 1 g: moving, or nothing new yet from an IMU just woken from wake on motion.
  if (g < 0.8f || g > 1.2f) return Pose::Unclear;
  if (sqrtf(x * x + y * y) < 0.5f * g) return Pose::Flat;  // sin 30°
  // 0° portrait, 90° landscape. 30° to 60° between them is neither, so a board held near 45°
  // doesn't flip back and forth; so are the two ways upside down, which the panel can't show.
  const float angle = atan2f(-x, y) * (180.0f / static_cast<float>(M_PI));
  if (fabsf(angle) <= 30.0f) return Pose::Portrait;
  if (fabsf(angle - 90.0f) <= 30.0f) return Pose::Landscape;
  return Pose::Unclear;
}

class ViewerScreen : public ui::Screen {
 public:
  ViewerScreen(std::vector<std::string> paths, int index,
               std::function<void(const std::string &)> shown)
      : paths_(std::move(paths)), shown_(std::move(shown)), held_(sensePose()) {
    if (held_ == Pose::Unclear) held_ = Pose::Flat;  // till the ticks know better: portrait
    show(std::max(0, std::min(index, static_cast<int>(paths_.size()) - 1)));
  }

  std::string tag() const override { return moduleTag("FILES"); }
  std::string context() const override {
    const std::string name = paths::nameOf(paths_[index_]);
    if (paths_.size() < 2) return name;
    char count[32];  // room for any two ints, so the compiler can see nothing is cut off
    snprintf(count, sizeof count, "%02d/%02d ", index_ + 1, static_cast<int>(paths_.size()));
    return count + name;
  }
  bool chrome() const override { return chrome_; }
  bool landscape() const override { return landscape_; }
  // Turned with the board, so the wheel keeps its sense: the roll that steps forward upright
  // steps forward turned too.
  bool swapsDial() const override { return false; }
  bool reading() const override { return true; }
  bool wakeOnMotion() const override { return true; }

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

  // Follows the board as it's turned: a new pose seen on kSettleTicks ticks in a row is how it's
  // held now. Upright, the screen turns with it (a full refresh, the picture decoded again at
  // the new size). Laid down, it stays as it is, for the next pictures too.
  bool onTick() override {
    const Pose p = sensePose();
    if (p == Pose::Unclear || p == held_) {
      seenTicks_ = 0;
      return false;
    }
    if (p != seen_) {
      seen_ = p;
      seenTicks_ = 0;
    }
    if (++seenTicks_ < kSettleTicks) return false;
    seenTicks_ = 0;
    held_ = p;
    log_i("viewer: held %s", poseName(p));
    if (p == Pose::Flat || (p == Pose::Landscape) == landscape_) return false;
    landscape_ = p == Pose::Landscape;
    return true;
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
    if (!probed_) log_w("viewer: %s: not a JPEG or PNG we can read", paths_[index_].c_str());
    // As the board is held, whatever the picture's shape; lying flat, as it was (portrait when
    // the viewer opens flat).
    if (held_ == Pose::Portrait || held_ == Pose::Landscape) landscape_ = held_ == Pose::Landscape;
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
    const std::string text = paths::nameOf(paths_[index_]) +
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
  Pose held_ = Pose::Flat;     // how the board is held, as last settled
  Pose seen_ = Pose::Unclear;  // a different pose on the latest ticks, and how many in a row
  int seenTicks_ = 0;
  picture::Bitmap bitmap_;
};

}  // namespace

std::unique_ptr<ui::Screen> makeViewer(std::vector<std::string> paths, int index,
                                       std::function<void(const std::string &)> shown) {
  if (paths.empty()) return nullptr;
  return std::make_unique<ViewerScreen>(std::move(paths), index, std::move(shown));
}

}  // namespace apps
