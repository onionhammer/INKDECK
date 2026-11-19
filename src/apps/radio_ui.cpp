#include "apps/radio_ui.h"

#include <math.h>

#include <algorithm>

#include "audio.h"

namespace apps {
namespace radio {

using ui::Align;
using ui::Rect;
using ui::TextStyle;

namespace {

constexpr TextStyle kUnit{&ui::fonts::kMono18Bold, 0};     // "dBm" by the reading
constexpr TextStyle kValue{&ui::fonts::kMono14Bold, 0};    // key/value values, dBm in rows
constexpr TextStyle kAddress{&ui::fonts::kMono15Bold, 0};  // the address under the name
constexpr TextStyle kSubtitle{&ui::fonts::kMono13Medium, 0};
constexpr TextStyle kMarker{&ui::fonts::kMono13Medium, 0};

}  // namespace

int barsFor(int rssi) {
  if (rssi >= -55) return 4;
  if (rssi >= -65) return 3;
  if (rssi >= -75) return 2;
  return 1;
}

void drawBars(ui::Canvas &c, int x, int bottom, int rssi, uint16_t color) {
  static const int kHeights[4] = {4, 7, 10, 14};
  const int lit = barsFor(rssi);
  for (int i = 0; i < 4; ++i) {
    const Rect bar{x + i * 5, bottom - kHeights[i], 3, kHeights[i]};
    if (i < lit) {
      ui::fill(c, bar, color);
    } else {
      ui::frame(c, bar, 1, color);
    }
  }
}

const char *quality(int rssi) {
  if (rssi >= -55) return "STRONG";
  if (rssi >= -67) return "GOOD";
  if (rssi >= -75) return "FAIR";
  if (rssi >= -85) return "WEAK";
  return "FAINT";
}

std::string dbm(int rssi) {
  if (rssi >= 0) return std::to_string(rssi);
  return "\xE2\x88\x92" + std::to_string(-rssi);
}

void History::push(int8_t rssi) {
  v_.push_back(rssi);
  if (static_cast<int>(v_.size()) > kSlots) v_.erase(v_.begin());
}

bool History::any() const {
  return std::any_of(v_.begin(), v_.end(), [](int8_t s) { return s != kNone; });
}

int History::min() const {
  int m = 0;
  bool first = true;
  for (const int8_t s : v_) {
    if (s == kNone) continue;
    m = first ? s : std::min<int>(m, s);
    first = false;
  }
  return m;
}

int History::max() const {
  int m = 0;
  bool first = true;
  for (const int8_t s : v_) {
    if (s == kNone) continue;
    m = first ? s : std::max<int>(m, s);
    first = false;
  }
  return m;
}

void drawSignal(ui::Canvas &c, const Rect &r, bool have, int rssi, const std::string &top,
                const std::string &bottom, const History &history) {
  using namespace ui;
  const Rect p = r.inset(kPadX, 12);
  constexpr int kBigH = 51;  // the canvas's 64 px at line-height 0.8
  const int base = p.y + capHeight(style::kReadout) + (kBigH - capHeight(style::kReadout)) / 2;
  const int colW = (p.w - 16) / 2;

  // The 64 px face has digits and "-" only (no U+2212).
  const std::string reading = have ? std::to_string(rssi) : "--";
  const int x = drawText(c, p.x, base, reading, style::kReadout);
  drawText(c, x + 6, base, "dBm", kUnit);

  const int col2 = p.x + colW + 16;
  vrule(c, col2, p.y, kBigH);
  const Rect right{col2 + kRule + 16, p.y, p.right() - col2 - kRule - 16, kBigH};
  if (top.empty()) {
    drawText(c, right.x, right.y + capHeight(style::kReading), have ? quality(rssi) : "LOST",
             style::kReading);
    const int filled = have ? std::clamp((rssi + 100) * 20 / 70, 0, 20) : 0;
    drawSegments(c, {right.x, right.bottom() - 14, right.w, 14}, 20, filled, 2);
  } else {
    drawText(c, {right.x, right.y, right.w, 24}, top, style::kReading);
    drawText(c, {right.x, right.bottom() - 17, right.w, 17}, bottom, style::kLabel);
  }

  const Rect labels{p.x, p.y + kBigH + 12, p.w, 17};
  drawText(c, labels, "RSSI \xC2\xB7 2 MIN", style::kLabel);
  if (history.any()) {
    drawText(c, labels, "MIN " + dbm(history.min()) + " \xC2\xB7 MAX " + dbm(history.max()),
             style::kLabel, Align::Right);
  }

  // The sparkline: kSlots bars, newest on the right, the bottom rule the axis.
  const Rect spark{p.x, labels.bottom() + 8, p.w, 32};
  fill(c, {spark.x, spark.bottom() - 2, spark.w, 2}, kInk);
  if (!history.any()) return;
  float mn = history.min(), mx = history.max();
  if (mx - mn < 6) {  // don't blow a few dB of wander up to full height
    const float mid = (mn + mx) / 2;
    mn = mid - 3;
    mx = mid + 3;
  }
  constexpr int kSlots = History::kSlots;
  constexpr int kGap = 2;
  const std::vector<int8_t> &v = history.samples();
  const int first = kSlots - static_cast<int>(v.size());
  for (int i = std::max(first, 0); i < kSlots; ++i) {
    const int8_t s = v[i - first];
    if (s == History::kNone) continue;
    const int x0 = spark.x + i * (spark.w + kGap) / kSlots;
    const int x1 = spark.x + (i + 1) * (spark.w + kGap) / kSlots - kGap;
    const int h = std::max(2, static_cast<int>(lroundf(4 + (s - mn) / (mx - mn) * 26)));
    fill(c, {x0, spark.bottom() - 2 - h, x1 - x0, h}, kInk);
  }
}

void drawRow(ui::Canvas &c, const Rect &r, const std::string &title, const std::string &subtitle,
             const std::string &tag, int rssi, bool selected) {
  using namespace ui;
  const uint16_t ink = selected ? kPaper : kInk;
  if (selected) {
    fill(c, r, kInk);
  } else {
    dashedH(c, r.x, r.bottom() - 1, r.w);
  }
  const Rect band{r.x, r.y, r.w, r.h - 1};
  constexpr int kGap = 10;
  int x = r.x + kPadX;
  if (selected) drawText(c, {x, band.y, 14, band.h}, glyph::kPlay, kMarker, Align::Left, ink);
  x += 14 + kGap;

  // Right side: the dBm, the bars left of it, then the tag.
  int right = r.right() - kPadX;
  constexpr int kDbmW = 32;
  drawText(c, {right - kDbmW, band.y, kDbmW, band.h}, dbm(rssi), kValue, Align::Right, ink);
  right -= kDbmW + kGap;
  drawBars(c, right - kBarsW, band.centerY() + 7, rssi, ink);
  right -= kBarsW + kGap;
  if (!tag.empty()) {
    const int w = tagWidth(tag, style::kTag, 5);
    drawTag(c, right - w, band.centerY(), 18, tag, style::kTag, TagKind::Outlined, 5, ink);
    right -= w + kGap;
  }

  const TextStyle &s = selected ? style::kRowSmallSelected : style::kRowSmall;
  const int titleW = right - x;
  constexpr int kLineGap = 7;
  const int block = capHeight(s) + kLineGap + capHeight(kSubtitle);
  const int titleBase = band.y + (band.h - block) / 2 + capHeight(s);
  drawText(c, x, titleBase, ellipsize(s, title, titleW), s, ink);
  drawText(c, x, titleBase + kLineGap + capHeight(kSubtitle), ellipsize(kSubtitle, subtitle, titleW),
           kSubtitle, ink);
}

void drawKeyValue(ui::Canvas &c, const Rect &r, const std::string &key, const std::string &value) {
  using namespace ui;
  constexpr int kKeyW = 112;
  drawText(c, {r.x, r.y, kKeyW, r.h}, key, style::kKey);
  drawText(c, {r.x + kKeyW, r.y, r.w - kKeyW, r.h}, value, kValue);
}

void drawIdentity(ui::Canvas &c, Rect r, const std::string &header, const std::string &pill,
                  const std::string &name, const std::string &address, const std::string &tag) {
  using namespace ui;
  const Rect head = r.takeTop(kSectionH);
  drawSectionHeader(c, head, header);
  if (!pill.empty()) {
    const int w = tagWidth(pill, style::kPill, 6);
    drawTag(c, head.right() - kPadX - w, head.y + (kSectionH - kRule) / 2, 19, pill, style::kPill,
            TagKind::Inverted, 6);
  }
  const Rect p = r.inset(kPadX, 0);
  const int nameBase = p.y + 12 + capHeight(style::kReading);
  drawText(c, p.x, nameBase, ellipsize(style::kReading, name, p.w), style::kReading);
  const Rect line{p.x, p.bottom() - 12 - 20, p.w, 20};
  int tagW = 0;
  if (!tag.empty()) {
    tagW = tagWidth(tag, style::kTag, 5);
    drawTag(c, line.right() - tagW, line.centerY(), 18, tag, style::kTag, TagKind::Outlined, 5);
  }
  drawText(c, {line.x, line.y, line.w - tagW - 10, line.h}, address, kAddress);
}

Rect takeSection(ui::Canvas &c, Rect &r, int h, const std::string &label,
                 const std::string &right) {
  using namespace ui;
  Rect s = r.takeTop(h);
  hrule(c, s.x, s.y, s.w);
  s.takeTop(kRule);
  drawSectionHeader(c, s.takeTop(kSectionH), label, right);
  return s;
}

void beep(int rssi) {
  // FAINT (-95 dBm) at 400 Hz up to STRONG (-35 dBm and up) at 2400 Hz.
  const int hz = 400 + std::clamp(rssi + 95, 0, 60) * 2000 / 60;
  audio::beep(50, hz);
}

}  // namespace radio
}  // namespace apps
