#include "ui/components.h"

#include "input.h"

#include <stdio.h>

#include <algorithm>

namespace ui {
namespace {

constexpr TextStyle kMarker{&fonts::kMono13Medium, 0};  // ▶ in the selected row
constexpr TextStyle kSubtitle{&fonts::kMono13Medium, 0};

}  // namespace

// --- Status bar ---------------------------------------------------------------------------

int drawBattery(Canvas &c, int x, int centerY, int percent) {
  constexpr int kW = 26, kH = 13, kBorder = 2, kPad = 1;
  const Rect body{x, centerY - kH / 2, kW, kH};
  frame(c, body, kBorder);
  const int inner = kW - 2 * (kBorder + kPad);
  const int level = std::clamp(percent, 0, 100) * inner / 100;
  fill(c, {body.x + kBorder + kPad, body.y + kBorder + kPad, level, kH - 2 * (kBorder + kPad)},
       kInk);
  fill(c, {body.right(), centerY - 2, 2, 5}, kInk);  // terminal nub
  return kW + 2;
}

int drawPlaybackMark(Canvas &c, int x, int centerY, Playback playback) {
  constexpr int kW = 22, kH = 20;
  if (playback == Playback::None) return 0;
  // Inverted, like a pill: a play triangle or two pause bars in paper.
  const Rect box{x, centerY - kH / 2, kW, kH};
  fill(c, box, kInk);
  const int cy = box.centerY();
  if (playback == Playback::Playing) {
    c.fillTriangle(box.x + 7, cy - 6, box.x + 7, cy + 6, box.x + 16, cy, kPaper);
  } else {
    fill(c, {box.x + 6, cy - 6, 4, 12}, kPaper);
    fill(c, {box.x + 12, cy - 6, 4, 12}, kPaper);
  }
  return kW;
}

void drawStatusBar(Canvas &c, const Rect &r, const StatusInfo &info) {
  const Rect band{r.x, r.y, r.w, r.h - kRule};
  hrule(c, r.x, band.bottom(), r.w);

  // Module tag, inverted.
  const int tagW = textWidth(style::kStatusTag, info.tag) + 24;
  fill(c, {band.x, band.y, tagW, band.h}, kInk);
  drawText(c, {band.x + 12, band.y, tagW - 24, band.h}, info.tag, style::kStatusTag, Align::Left,
           kPaper);

  // Right block: [▶/‖] [WIFI] [temp] [battery glyph + %] [clock], after a 3 px rule.
  constexpr int kGap = 14;
  constexpr int kBatteryGlyphW = 28 + 6;
  constexpr int kPlaybackW = 22;
  std::string batteryText;
  if (info.battery >= 0) {
    batteryText = std::to_string(info.battery) + "%";
  } else if (info.usb) {
    batteryText = "USB";
  }
  const std::string texts[] = {info.wifi ? "WIFI" : "", info.temp, batteryText, info.clock};
  int blockW = -kGap;
  for (const std::string &t : texts) {
    if (!t.empty()) blockW += textWidth(style::kStatus, t) + kGap;
  }
  if (info.battery >= 0) blockW += kBatteryGlyphW;
  if (info.playback != Playback::None) blockW += kPlaybackW + kGap;
  const int blockX = band.right() - std::max(blockW, 0) - 12;
  vrule(c, blockX - 12 - kRule, band.y, band.h);
  const int baseline = baselineCentered(style::kStatus, band.y, band.h);
  int x = blockX;
  if (info.playback != Playback::None) {
    x += drawPlaybackMark(c, x, band.centerY(), info.playback) + kGap;
  }
  for (int i = 0; i < 4; ++i) {
    if (texts[i].empty()) continue;
    if (i == 2 && info.battery >= 0) x += drawBattery(c, x, band.centerY(), info.battery) + 6;
    x = drawText(c, x, baseline, texts[i], style::kStatus) + kGap;
  }

  // Context fills what's left between the tag and the right block.
  const int ctxX = band.x + tagW + 12;
  const int ctxW = blockX - 12 - kRule - 12 - ctxX;
  const std::string ctx = info.contextIsPath
                              ? ellipsizeFront(style::kStatusContext, info.context, ctxW)
                              : info.context;
  drawText(c, {ctxX, band.y, ctxW, band.h}, ctx, style::kStatusContext);
}

// --- Section header ----------------------------------------------------------------------

void drawSectionHeader(Canvas &c, const Rect &r, const std::string &label,
                       const std::string &right, SectionOptions options) {
  const Rect band{r.x + kPadX, r.y, r.w - 2 * kPadX, r.h - kRule};
  hrule(c, r.x, band.bottom(), r.w);
  const uint16_t ink = options.inverted ? kPaper : kInk;
  if (options.inverted) fill(c, {r.x, r.y, r.w, r.h - kRule}, kInk);
  const int rightW = right.empty() ? 0 : textWidth(style::kSection, right) + kPadX;
  drawText(c, {band.x, band.y, band.w - rightW, band.h}, (options.prefix ? "// " : "") + label,
           style::kSection, Align::Left, ink);
  if (!right.empty()) drawText(c, band, right, style::kSection, Align::Right, ink);
}

void drawNotice(Canvas &c, const Rect &r, const std::string &text) {
  fill(c, r, kInk);
  drawText(c, {r.x + kPadX, r.y, r.w - 2 * kPadX, r.h - kRule}, "! " + text, style::kSection,
           Align::Left, kPaper);
}

// --- Key-hint bar ------------------------------------------------------------------------

int keycapWidth(const char *cap) { return textWidth(style::kCap, cap) + 2 * 5 + 2 * 2; }

int drawKeycap(Canvas &c, int x, int centerY, const char *cap) {
  constexpr int kH = 20;
  const int w = keycapWidth(cap);
  const Rect box{x, centerY - kH / 2, w, kH};
  frame(c, box, 2);
  drawText(c, x + 7, baselineCentered(style::kCap, box.y, box.h), cap, style::kCap);
  return w;
}

void drawHintBar(Canvas &c, const Rect &r, const Hint *hints, int count) {
  hrule(c, r.x, r.y, r.w);
  if (count <= 0) return;
  const Rect band{r.x + kPadX, r.y + kRule, r.w - 2 * kPadX, r.h - kRule};
  constexpr int kCapGap = 6;
  int widths[kMaxHints];
  int total = 0;
  count = std::min(count, kMaxHints);
  for (int i = 0; i < count; ++i) {
    widths[i] = keycapWidth(hints[i].cap) + kCapGap + textWidth(style::kHint, hints[i].verb);
    total += widths[i];
  }
  // Spread like CSS justify-content: space-between.
  const int gap = count > 1 ? (band.w - total) / (count - 1) : 0;
  const int baseline = baselineCentered(style::kHint, band.y, band.h);
  int x = band.x;
  for (int i = 0; i < count; ++i) {
    const int capW = drawKeycap(c, x, band.centerY(), hints[i].cap);
    drawText(c, x + capW + kCapGap, baseline, hints[i].verb, style::kHint);
    x += widths[i] + gap;
  }
}

// --- Tags --------------------------------------------------------------------------------

int tagWidth(const std::string &text, const TextStyle &s, int padX) {
  return textWidth(s, text) + 2 * padX + 4;
}

int drawTag(Canvas &c, int x, int centerY, int h, const std::string &text, const TextStyle &s,
            TagKind kind, int padX, uint16_t color) {
  const int w = tagWidth(text, s, padX);
  const Rect box{x, centerY - h / 2, w, h};
  const uint16_t other = color == kInk ? kPaper : kInk;
  if (kind == TagKind::Inverted) {
    fill(c, box, color);
  } else {
    frame(c, box, 2, color);
  }
  drawText(c, x + padX + 2, baselineCentered(s, box.y, box.h), text, s,
           kind == TagKind::Inverted ? other : color);
  return w;
}

// --- List rows ---------------------------------------------------------------------------

void drawRow(Canvas &c, const Rect &r, const Row &row, bool selected) {
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
  if (!row.index.empty()) {
    drawText(c, x, baselineCentered(style::kRowIndex, band.y, band.h), row.index,
             style::kRowIndex, ink);
    x += textWidth(style::kRowIndex, row.index) + kGap;
  }

  // Right side: trailing text, then the boxed tag left of it.
  int right = r.right() - kPadX;
  if (!row.trailing.empty()) {
    const int w = textWidth(style::kStatus, row.trailing);
    drawText(c, right - w, baselineCentered(style::kStatus, band.y, band.h), row.trailing,
             style::kStatus, ink);
    right -= w + kGap;
  }
  if (!row.tag.empty()) {
    const int w = tagWidth(row.tag, style::kTag, 5);
    drawTag(c, right - w, band.centerY(), 18, row.tag, style::kTag, TagKind::Outlined, 5, ink);
    right -= w + kGap;
  }

  const int titleW = right - x;
  if (row.subtitle.empty()) {
    const TextStyle &s = selected ? style::kRowSelected : style::kRow;
    drawText(c, {x, band.y, titleW, band.h}, row.title, s, Align::Left, ink);
    return;
  }
  const TextStyle &s = selected ? style::kRowSmallSelected : style::kRowSmall;
  constexpr int kLineGap = 7;
  const int block = capHeight(s) + kLineGap + capHeight(kSubtitle);
  const int titleBase = band.y + (band.h - block) / 2 + capHeight(s);
  drawText(c, x, titleBase, ellipsize(s, row.title, titleW), s, ink);
  drawText(c, x, titleBase + kLineGap + capHeight(kSubtitle),
           ellipsize(kSubtitle, row.subtitle, titleW), kSubtitle, ink);
}

void ListCursor::reset(int count, int cursor) {
  count_ = std::max(0, count);
  set(cursor);
}

bool ListCursor::move(int delta, bool wrap) {
  const int before = cursor_;
  if (input::lastKeyFast() && pageRows_ > 1 && count_ > pageRows_ && delta != 0) {
    // The dial is held: whole pages, so a long folder takes seconds rather than minutes.
    const int page = cursor_ / pageRows_ * pageRows_;
    int target = page + (delta > 0 ? pageRows_ : -pageRows_);
    if (wrap) {
      if (target >= count_) target = 0;
      if (target < 0) target = (count_ - 1) / pageRows_ * pageRows_;
    }
    set(target);
    return cursor_ != before;
  }
  if (wrap && count_ > 0) {
    cursor_ = ((cursor_ + delta) % count_ + count_) % count_;
  } else {
    set(cursor_ + delta);
  }
  return cursor_ != before;
}

void ListCursor::set(int cursor) { cursor_ = count_ ? std::clamp(cursor, 0, count_ - 1) : 0; }

int ListCursor::pageStart(int rowsPerPage) const {
  pageRows_ = rowsPerPage;
  return rowsPerPage > 0 ? cursor_ / rowsPerPage * rowsPerPage : 0;
}

std::string ListCursor::label() const {
  char buf[16];
  snprintf(buf, sizeof buf, "%02d/%02d", count_ ? cursor_ + 1 : 0, count_);
  return buf;
}

// --- Meters ------------------------------------------------------------------------------

void drawProgress(Canvas &c, const Rect &r, float fraction) {
  frame(c, r, 2);
  const Rect inner = r.inset(4, 4);
  const int done = static_cast<int>(std::clamp(fraction, 0.0f, 1.0f) * inner.w + 0.5f);
  fill(c, {inner.x, inner.y, done, inner.h}, kInk);
  dither50(c, {inner.x + done, inner.y, inner.w - done, inner.h});
}

void drawSegments(Canvas &c, const Rect &r, int segments, int filled, int gap) {
  if (segments <= 0) return;
  const int w = (r.w - gap * (segments - 1)) / segments;
  for (int i = 0; i < segments; ++i) {
    const Rect seg{r.x + i * (w + gap), r.y, w, r.h};
    if (i < filled) {
      fill(c, seg, kInk);
    } else {
      frame(c, seg, 2);
    }
  }
}

// --- Button ------------------------------------------------------------------------------

Rect drawButton(Canvas &c, int right, int top, const std::string &glyph,
                const std::string &label) {
  constexpr int kH = 44, kPad = 18, kGap = 10, kShadow = 6;
  const TextStyle &glyphStyle = style::kStatusTag;
  const int glyphW = glyph.empty() ? 0 : textWidth(glyphStyle, glyph) + kGap;
  const int w = 2 * kPad + glyphW + textWidth(style::kButton, label);
  const Rect box{right - w, top, w, kH};
  dither50(c, {box.x + kShadow, box.y + kShadow, box.w, box.h});
  fill(c, box, kInk);
  int x = box.x + kPad;
  if (!glyph.empty()) {
    drawText(c, x, baselineCentered(glyphStyle, box.y, box.h), glyph, glyphStyle, kPaper);
    x += glyphW;
  }
  drawText(c, x, baselineCentered(style::kButton, box.y, box.h), label, style::kButton, kPaper);
  return box;
}

// --- Stat cell ---------------------------------------------------------------------------

void drawStat(Canvas &c, const Rect &r, const std::string &key, const std::string &value) {
  dashedH(c, r.x, r.bottom() - 1, r.w);
  constexpr int kLineGap = 8;
  const int block = capHeight(style::kKey) + kLineGap + capHeight(style::kValue);
  const int keyBase = r.y + (r.h - 1 - block) / 2 + capHeight(style::kKey);
  drawText(c, r.x, keyBase, ellipsize(style::kKey, key, r.w), style::kKey);
  drawText(c, r.x, keyBase + kLineGap + capHeight(style::kValue),
           ellipsize(style::kValue, value, r.w), style::kValue);
}

}  // namespace ui
