#include "apps/level.h"

#include <Arduino.h>
#include <Preferences.h>
#include <math.h>
#include <stdio.h>

#include <algorithm>

#include "apps/home.h"
#include "audio.h"
#include "board_pins.h"
#include "sensor_hub.h"

namespace apps {
namespace {

using ui::Key;
using ui::Rect;
using ui::TextStyle;

// Sizes from the Level artboard.
constexpr TextStyle kAngle{&ui::fonts::kMono40ExtraBold, -1};      // −2.1°
constexpr TextStyle kEdgeAngle{&ui::fonts::kMono28ExtraBold, -1};  // +1.2°
constexpr TextStyle kCellValue{&ui::fonts::kMono17ExtraBold, 0};

constexpr int kLeftW = 300;        // the bubble's column, rule included
constexpr int kTopH = 301;         // bubble and readings, above the vial band
constexpr int kBubble = 250;       // the bubble's box
constexpr int kRim = 121;          // outer ring radius (3 px)
constexpr int kBubbleR = 19;
constexpr float kFullScale = 10;      // degrees that put the bubble against the rim
constexpr float kVialFullScale = 5;   // degrees that put the vial's bubble at an end
constexpr float kLevelWithin = 0.3f;  // degrees: "LEVEL", and the tone
constexpr float kRedrawDeadband = 0.15f;
constexpr int kSamples = 8;  // IMU reads averaged per update (the accelerometer runs at 56 Hz)
constexpr char kNamespace[] = "level";
constexpr char kKey[] = "zero2";
// The zero from when landscape had the wheel underneath: the screen turned 180 degrees from
// now, so its pitch and roll have the other sign. The edge angle reads the same either way.
constexpr char kOldKey[] = "zero";

constexpr float kDeg = 180.0f / static_cast<float>(M_PI);

enum class Mode : uint8_t { Surface, Edge };
// The side the board stands on in edge mode, as the landscape screen is drawn.
enum class Side : uint8_t { Bottom, Top, Left, Right };

struct Zero {
  float pitch = 0;
  float roll = 0;
  float edge = 0;  // long sides only
  bool set = false;
};

Zero loadZero() {
  Zero z;
  Preferences prefs;
  if (prefs.begin(kNamespace, false)) {  // read-write: read-only logs an error until it exists
    if (prefs.isKey(kKey) && prefs.getBytesLength(kKey) == sizeof z) {
      prefs.getBytes(kKey, &z, sizeof z);
    } else if (prefs.isKey(kOldKey)) {
      if (prefs.getBytesLength(kOldKey) == sizeof z) {
        prefs.getBytes(kOldKey, &z, sizeof z);
        z.pitch = -z.pitch;
        z.roll = -z.roll;
        prefs.putBytes(kKey, &z, sizeof z);
      }
      prefs.remove(kOldKey);
    }
    prefs.end();
  }
  return z;
}

void saveZero(const Zero &z) {
  Preferences prefs;
  if (!prefs.begin(kNamespace, false)) return;
  prefs.putBytes(kKey, &z, sizeof z);
  prefs.end();
}

struct Reading {
  bool ok = false;
  Mode mode = Mode::Surface;
  float pitch = 0;  // top edge up > 0 (surface)
  float roll = 0;   // right edge up > 0 (surface)
  Side side = Side::Bottom;
  float edge = 0;   // right end (as you look at it) up > 0 (edge)
  // Raw, before the zero: what HOLD ● ZERO stores.
  float rawPitch = 0, rawRoll = 0, rawEdge = 0;
};

std::string angleText(float v) { return ui::signedNumber(v, 1) + "\xC2\xB0"; }

// "RAISE 3.7 mm PER 10 cm": how far to lift the low side.
std::string raiseText(float deg) {
  char buf[48];
  snprintf(buf, sizeof buf, "RAISE %.1f mm PER 10 cm", tanf(fabsf(deg) / kDeg) * 100);
  return buf;
}

class LevelScreen : public ui::Screen {
 public:
  std::string tag() const override { return moduleTag("LEVEL"); }
  bool landscape() const override { return true; }

  std::string context() const override {
    std::string s;
    if (!r_.ok) {
      s = "NO IMU";
    } else if (r_.mode == Mode::Surface) {
      const float tilt = sqrtf(r_.pitch * r_.pitch + r_.roll * r_.roll);
      char buf[32];
      snprintf(buf, sizeof buf, "SURFACE \xC2\xB7 TILT %.1f\xC2\xB0", tilt);
      s = buf;
    } else {
      s = "EDGE \xC2\xB7 " + angleText(r_.edge);
    }
    if (held_) s += " \xC2\xB7 HELD";
    return s;
  }

  int hints(ui::Hint *out) const override {
    out[0] = {ui::glyph::kDial, "TONE"};
    out[1] = {ui::glyph::kPress, held_ ? "RESUME" : "HOLD"};
    out[2] = {ui::glyph::kHoldPress, zero_.set ? "UNZERO" : "ZERO"};
    out[3] = {"BOOT", "BACK"};
    return 4;
  }

  void onEnter() override {
    zero_ = loadZero();
    sample();
    level_ = within(kLevelWithin);
  }

  bool onKey(Key key) override {
    switch (key) {
      case Key::Up:
      case Key::Down:
        tone_ = !tone_;
        return true;
      case Key::Select:
        held_ = !held_;
        if (!held_) sample();
        return true;
      case Key::SelectLong:
        if (zero_.set) {
          zero_ = Zero{};
        } else if (r_.ok) {
          // Zero what's live now; the other mode keeps its previous zero.
          if (r_.mode == Mode::Surface) {
            zero_.pitch = r_.rawPitch;
            zero_.roll = r_.rawRoll;
          } else if (r_.side == Side::Bottom || r_.side == Side::Top) {
            zero_.edge = r_.rawEdge;
          }
          zero_.set = true;
        }
        saveZero(zero_);
        sample();
        level_ = within(kLevelWithin);
        return true;
      default: return false;
    }
  }

  bool onTick() override {
    if (held_) return false;
    const Reading before = r_;
    const bool wasLevel = level_;
    sample();
    // Level within 0.3°, and not level again until past 0.6°: readings that wander across a
    // single line would beep and redraw over and over.
    level_ = within(level_ ? 2 * kLevelWithin : kLevelWithin);
    if (tone_ && level_ && !wasLevel) audio::beep();
    if (level_ != wasLevel) return true;
    if (r_.ok != before.ok || r_.mode != before.mode || r_.side != before.side) return true;
    if (r_.mode == Mode::Surface) {
      return fabsf(r_.pitch - before.pitch) >= kRedrawDeadband ||
             fabsf(r_.roll - before.roll) >= kRedrawDeadband;
    }
    return fabsf(r_.edge - before.edge) >= kRedrawDeadband;
  }

  void draw(ui::Canvas &c, const Rect &body) override {
    using namespace ui;
    Rect r = body;
    Rect top = r.takeTop(kTopH);
    const Rect left = top.takeLeft(kLeftW - kRule);
    vrule(c, top.x, top.y, top.h);
    top.takeLeft(kRule);
    drawSurface(c, left);
    drawReadings(c, top);
    hrule(c, r.x, r.y, r.w);
    r.takeTop(kRule);
    drawEdge(c, r);
  }

 private:
  bool isLevel() const { return level_; }

  bool within(float limit) const {
    if (!r_.ok) return false;
    if (r_.mode == Mode::Surface) return fabsf(r_.pitch) < limit && fabsf(r_.roll) < limit;
    return (r_.side == Side::Bottom || r_.side == Side::Top) && fabsf(r_.edge) < limit;
  }

  // Averages a few accelerometer reads and works out both modes' angles in landscape axes.
  void sample() {
    float a[3] = {};
    int n = 0;
    for (int i = 0; i < kSamples; ++i) {
      const qmi8658::Reading m = sensors::motion();
      if (!m.ok) continue;
      for (int k = 0; k < 3; ++k) a[k] += m.acc[k];
      ++n;
    }
    Reading out;
    out.mode = r_.mode;
    if (n == 0) {
      r_ = out;
      return;
    }
    // Portrait screen axes -> landscape (the board turned a quarter clockwise from portrait,
    // wheel on top): landscape right = portrait up (+Y), landscape up = portrait left (-X).
    qmi8658::Reading l;
    l.ok = true;
    l.acc[0] = a[1] / n;
    l.acc[1] = -a[0] / n;
    l.acc[2] = a[2] / n;
    out.ok = true;
    const float g = sqrtf(l.acc[0] * l.acc[0] + l.acc[1] * l.acc[1] + l.acc[2] * l.acc[2]);
    const float through = g > 0 ? fabsf(l.acc[2]) / g : 1;  // share of gravity through the screen
    if (through >= 0.7f) {
      out.mode = Mode::Surface;
    } else if (through <= 0.5f) {
      out.mode = Mode::Edge;
    }  // in between: keep the mode, so it doesn't flicker

    sensors::tilt(l, out.rawPitch, out.rawRoll);
    out.pitch = out.rawPitch - zero_.pitch;
    out.roll = out.rawRoll - zero_.roll;

    // Edge: where "up" points in the screen's plane; 0 = standing on the bottom (long) side.
    // Turning the board clockwise (seen from the front) drops the right end and swings "up"
    // towards the screen's left, so the deviation from the nearest quarter turn is already
    // "right end up > 0" - upside down too, as seen by whoever reads it.
    const float theta = atan2f(l.acc[0], l.acc[1]) * kDeg;
    const float quarter = roundf(theta / 90) * 90;
    if (quarter == 0) {
      out.side = Side::Bottom;
    } else if (fabsf(quarter) == 180) {
      out.side = Side::Top;
    } else {
      out.side = quarter > 0 ? Side::Left : Side::Right;
    }
    out.rawEdge = theta - quarter;
    out.edge = out.rawEdge - ((out.side == Side::Bottom || out.side == Side::Top) ? zero_.edge : 0);
    r_ = out;
  }

  void drawSurface(ui::Canvas &c, Rect r) const {
    using namespace ui;
    const bool live = r_.ok && r_.mode == Mode::Surface;
    drawSectionHeader(c, r.takeTop(kSectionH), "SURFACE",
                      live ? "\xC2\xB1" "10\xC2\xB0" : "LAY FLAT", {live});
    const int cx = r.x + r.w / 2, cy = r.y + r.h / 2;
    c.fillCircle(cx, cy, kRim, kInk);
    c.fillCircle(cx, cy, kRim - 3, kPaper);
    drawDashedCircle(c, cx, cy, 60);
    c.drawCircle(cx, cy, 24, kInk);
    c.drawCircle(cx, cy, 23, kInk);
    for (int d = 8; d < kBubble - 8; d += 6) {  // crosshair, 2 on / 4 off
      c.drawFastVLine(cx, cy - kBubble / 2 + d, 2, kInk);
      c.drawFastHLine(cx - kBubble / 2 + d, cy, 2, kInk);
    }
    if (!r_.ok) return;
    constexpr float kTravel = kRim - 3 - kBubbleR;
    float dx = 0, dy = 0;
    if (live) {
      dx = r_.roll / kFullScale * kTravel;
      dy = -r_.pitch / kFullScale * kTravel;
      const float len = sqrtf(dx * dx + dy * dy);
      if (len > kTravel) {
        dx *= kTravel / len;
        dy *= kTravel / len;
      }
    }
    const int bx = cx + lroundf(dx), by = cy + lroundf(dy);
    c.fillCircle(bx, by, kBubbleR, kInk);
    if (!live) {  // parked in the middle, dithered: not in use
      c.fillCircle(bx, by, kBubbleR - 3, kPaper);
      dither50Circle(c, bx, by, kBubbleR - 3);
    }
  }

  void drawReadings(ui::Canvas &c, Rect r) const {
    using namespace ui;
    drawSectionHeader(c, r.takeTop(kSectionH), "TILT", "RINGS 2\xC2\xB0 \xC2\xB7 5\xC2\xB0");
    const bool live = r_.ok && r_.mode == Mode::Surface;
    // Pitch and roll, side by side.
    const Rect nums{r.x + 20, r.y + 16, r.w - 40, 64};
    const int colW = (nums.w - 16) / 2;
    const struct {
      const char *key;
      float value;
    } cols[2] = {{"PITCH \xC2\xB7 TOP EDGE", r_.pitch}, {"ROLL \xC2\xB7 RIGHT EDGE", r_.roll}};
    for (int i = 0; i < 2; ++i) {
      int x = nums.x + i * (colW + 16);
      if (i == 1) {
        vrule(c, x, nums.y, nums.h);
        x += 16;
      }
      drawText(c, x, nums.y + capHeight(style::kKey), cols[i].key, style::kKey);
      drawText(c, x, nums.y + 22 + capHeight(kAngle), live ? angleText(cols[i].value) : "--",
               kAngle);
    }
    // The low side, and how far to raise it.
    const Rect band{r.x, nums.bottom() + 14, r.w, 44};
    dashedH(c, band.x, band.y, band.w);
    dashedH(c, band.x, band.bottom() - 1, band.w);
    std::string pill = "LOW", text;
    if (!r_.ok) {
      pill = "--";
      text = "THE IMU ISN'T ANSWERING";
    } else if (isLevel()) {
      pill = "\xE2\x9C\x93 LEVEL";
      text = r_.mode == Mode::Surface ? "WITHIN 0.3\xC2\xB0 BOTH WAYS" : "WITHIN 0.3\xC2\xB0";
    } else if (r_.mode == Mode::Surface) {
      text = lowSide() + " \xC2\xB7 " + raiseText(std::max(fabsf(r_.pitch), fabsf(r_.roll)));
    } else if (r_.side == Side::Left || r_.side == Side::Right) {
      pill = "EDGE";
      text = "SHORT SIDE: THE ANGLE ONLY, NO VIAL";
    } else {
      text = std::string(r_.edge > 0 ? "LEFT END" : "RIGHT END") + " \xC2\xB7 " +
             raiseText(r_.edge);
    }
    const int pillW = drawTag(c, band.x + 20, band.centerY(), 26, pill, style::kPill,
                              TagKind::Inverted, 9);
    drawText(c, {band.x + 20 + pillW + 10, band.y, band.w - 50 - pillW, band.h}, text,
             style::kLabel);

    // Stat cells along the bottom.
    const Rect cells{r.x, r.bottom() - 50, r.w, 50};
    hrule(c, cells.x, cells.y - kRule, cells.w);
    const std::pair<const char *, std::string> stats[4] = {
        {"TONE", tone_ ? "ON" : "OFF"},
        {"ZERO", zero_.set ? "SET" : "NONE"},
        {"RATE", "56 Hz"},
        {"IMU", "0x6B"},
    };
    const int cellW = cells.w / 4;
    for (int i = 0; i < 4; ++i) {
      const int x = cells.x + i * cellW;
      drawText(c, x + kPadX, cells.y + 8 + capHeight(style::kKey), stats[i].first, style::kKey);
      drawText(c, {x + kPadX, cells.y + 26, cellW - 2 * kPadX + 6, 24}, stats[i].second,
               kCellValue);
      if (i < 3) dashedV(c, x + cellW - 1, cells.y, cells.h);
    }
  }

  void drawEdge(ui::Canvas &c, Rect r) const {
    using namespace ui;
    const bool live = r_.ok && r_.mode == Mode::Edge;
    const bool longSide = r_.side == Side::Bottom || r_.side == Side::Top;
    std::string label = "EDGE", right = "STAND IT ON ITS LONG SIDE";
    if (live) {
      label = longSide ? "EDGE \xC2\xB7 STANDING ON THE LONG SIDE" : "EDGE \xC2\xB7 SHORT SIDE";
      right = r_.side == Side::Top ? "UPSIDE DOWN" : "\xC2\xB1" "5\xC2\xB0";
    }
    drawSectionHeader(c, r.takeTop(kSectionH), label, right, {live});
    const int angleW = 96;
    const Rect vial{r.x + 20, r.centerY() - 22, r.w - 40 - angleW - 20, 44};
    drawText(c, {vial.right() + 20, vial.y, angleW, vial.h}, live ? angleText(r_.edge) : "--",
             kEdgeAngle, Align::Right);
    // The tube: 3 px outline with round ends, two marks either side of the centre.
    c.fillRoundRect(vial.x, vial.y, vial.w, vial.h, 22, kInk);
    c.fillRoundRect(vial.x + 3, vial.y + 3, vial.w - 6, vial.h - 6, 19, kPaper);
    const bool active = live && longSide;
    if (!active) dither50(c, {vial.x + 20, vial.y + 6, vial.w - 40, vial.h - 12});
    const int mid = vial.x + vial.w / 2;
    fill(c, {mid - 26, vial.y + 4, 2, vial.h - 8}, kInk);
    fill(c, {mid + 24, vial.y + 4, 2, vial.h - 8}, kInk);
    // The bubble rises to the high end. Upside down, the screen is mirrored for whoever reads it.
    float t = active ? std::clamp(r_.edge / kVialFullScale, -1.0f, 1.0f) : 0;
    if (r_.side == Side::Top) t = -t;
    const int travel = vial.w / 2 - 22 - 8;
    const int bx = mid + lroundf(t * travel);
    const int by = vial.y + (vial.h - 26) / 2;
    c.fillRoundRect(bx - 22, by, 44, 26, 13, kInk);
    if (!active) c.fillRoundRect(bx - 19, by + 3, 38, 20, 10, kPaper);
  }

  // The low edge or corner, for the surface: where the bubble isn't.
  std::string lowSide() const {
    const bool p = fabsf(r_.pitch) >= kLevelWithin, q = fabsf(r_.roll) >= kLevelWithin;
    const char *v = r_.pitch > 0 ? "BOTTOM" : "TOP";
    const char *h = r_.roll > 0 ? "LEFT" : "RIGHT";
    if (p && q) return std::string(v) + "-" + h + " CORNER";
    return std::string(p ? v : h) + " EDGE";
  }

  static void drawDashedCircle(ui::Canvas &c, int cx, int cy, int radius) {
    // 1 px, 3 px on / 4 px off around the circumference
    const int steps = static_cast<int>(2 * M_PI * radius);
    for (int i = 0; i < steps; ++i) {
      if (i % 7 >= 3) continue;
      const float a = static_cast<float>(i) / radius;
      c.drawPixel(cx + lroundf(radius * cosf(a)), cy + lroundf(radius * sinf(a)), ui::kInk);
    }
  }

  static void dither50Circle(ui::Canvas &c, int cx, int cy, int radius) {
    for (int y = -radius; y <= radius; ++y) {
      for (int x = -radius; x <= radius; ++x) {
        if (x * x + y * y > radius * radius) continue;
        if (((cx + x) + (cy + y)) % 2 == 0) c.drawPixel(cx + x, cy + y, ui::kInk);
      }
    }
  }

  Reading r_;
  Zero zero_;
  bool held_ = false;
  bool tone_ = true;
  bool level_ = false;  // with hysteresis (onTick)
};

}  // namespace

std::unique_ptr<ui::Screen> makeLevel() { return std::make_unique<LevelScreen>(); }

}  // namespace apps
