#include "ui/type.h"

#include <math.h>
#include <stdio.h>

#include <algorithm>

#include "text.h"

namespace ui {
namespace {

using fonts::slotFor;
using ::text::appendUtf8;

const GFXglyph *glyphFor(const GFXfont *f, uint8_t slot) {
  if (slot < f->first || slot > f->last) return nullptr;
  const GFXglyph *g = &f->glyph[slot - f->first];
  return g->xAdvance ? g : nullptr;  // subset fonts leave empty entries
}

// Calls fn(slot, glyph) for every glyph text draws as in font f, transliterating code points
// the font lacks.
template <typename Fn>
void forEachGlyph(const GFXfont *f, const std::string &text, Fn fn) {
  size_t i = 0;
  while (i < text.size()) {
    const uint32_t cp = ::text::nextCodepoint(text, i);
    if (cp < 0x20 || cp == 0xAD) continue;  // controls, soft hyphen
    const uint8_t slot = slotFor(cp);
    if (const GFXglyph *g = slot ? glyphFor(f, slot) : nullptr) {
      fn(slot, g);
      continue;
    }
    std::string ascii;  // "?" for unknown symbols, empty for invisible ones
    ::text::appendAscii(ascii, cp);
    for (const char ch : ascii) {
      auto s = static_cast<uint8_t>(ch);
      const GFXglyph *g = glyphFor(f, s);
      if (!g) g = glyphFor(f, s = '?');
      if (g) fn(s, g);
    }
  }
}

// Byte offsets of each code point boundary in text, including 0 and text.size().
std::vector<size_t> boundaries(const std::string &text) {
  std::vector<size_t> b{0};
  size_t i = 0;
  while (i < text.size()) {
    ::text::nextCodepoint(text, i);
    b.push_back(i);
  }
  return b;
}

}  // namespace

int textWidth(const TextStyle &s, const std::string &text) {
  int w = 0;
  int n = 0;
  forEachGlyph(s.font, text, [&](uint8_t, const GFXglyph *g) {
    w += g->xAdvance + s.tracking;
    ++n;
  });
  return n ? w - s.tracking : 0;
}

int capHeight(const TextStyle &s) {
  for (const uint8_t probe : {uint8_t('H'), uint8_t('0')}) {
    if (const GFXglyph *g = glyphFor(s.font, probe)) return -g->yOffset;
  }
  return s.font->yAdvance * 7 / 10;
}

int lineHeight(const TextStyle &s) { return s.font->yAdvance; }

int baselineCentered(const TextStyle &s, int top, int h) {
  const int cap = capHeight(s);
  return top + (h - cap) / 2 + cap;
}

int drawText(Canvas &c, int x, int baseline, const std::string &text, const TextStyle &s,
             uint16_t color) {
  c.setFont(s.font);
  int n = 0;
  forEachGlyph(s.font, text, [&](uint8_t slot, const GFXglyph *g) {
    c.drawChar(x, baseline, slot, color, color, 1);
    x += g->xAdvance + s.tracking;
    ++n;
  });
  return n ? x - s.tracking : x;
}

void drawText(Canvas &c, const Rect &box, const std::string &text, const TextStyle &s,
              Align align, uint16_t color) {
  const std::string fitted = ellipsize(s, text, box.w);
  const int w = textWidth(s, fitted);
  int x = box.x;
  if (align == Align::Center) x += (box.w - w) / 2;
  if (align == Align::Right) x = box.right() - w;
  drawText(c, x, baselineCentered(s, box.y, box.h), fitted, s, color);
}

std::string ellipsize(const TextStyle &s, const std::string &text, int maxWidth) {
  if (textWidth(s, text) <= maxWidth) return text;
  const int room = maxWidth - textWidth(s, glyph::kEllipsis) - s.tracking;
  const std::vector<size_t> b = boundaries(text);
  size_t keep = 0;
  for (size_t k = 1; k < b.size() && textWidth(s, text.substr(0, b[k])) <= room; ++k) keep = b[k];
  return text.substr(0, keep) + glyph::kEllipsis;
}

std::string ellipsizeFront(const TextStyle &s, const std::string &text, int maxWidth) {
  if (textWidth(s, text) <= maxWidth) return text;
  const int room = maxWidth - textWidth(s, glyph::kEllipsis) - s.tracking;
  const std::vector<size_t> b = boundaries(text);
  size_t from = text.size();
  for (size_t k = b.size() - 1; k-- > 0 && textWidth(s, text.substr(b[k])) <= room;) from = b[k];
  return glyph::kEllipsis + text.substr(from);
}

std::vector<std::string> wrap(const TextStyle &s, const std::string &text, int width,
                              int maxLines) {
  std::vector<std::string> lines;
  std::string line;
  size_t i = 0;
  while (i < text.size()) {
    const size_t sp = text.find(' ', i);
    const std::string word = text.substr(i, sp == std::string::npos ? std::string::npos : sp - i);
    i = sp == std::string::npos ? text.size() : sp + 1;
    if (word.empty()) continue;
    const std::string candidate = line.empty() ? word : line + " " + word;
    if (line.empty() || textWidth(s, candidate) <= width) {
      line = candidate;
    } else {
      lines.push_back(line);
      line = word;
    }
  }
  if (!line.empty()) lines.push_back(line);
  if (maxLines > 0 && static_cast<int>(lines.size()) > maxLines) {
    std::string rest = lines[maxLines - 1];
    for (size_t k = maxLines; k < lines.size(); ++k) rest += " " + lines[k];
    lines.resize(maxLines);
    lines.back() = rest;
  }
  for (auto &l : lines) l = ellipsize(s, l, width);
  return lines;
}

std::string signedNumber(float value, int decimals) {
  if (fabsf(value) < 0.5f * powf(10.0f, -decimals)) value = 0;
  char buf[24];
  snprintf(buf, sizeof buf, "%+.*f", decimals, value);
  std::string s = buf;
  if (s[0] == '-') s.replace(0, 1, "\xE2\x88\x92");  // U+2212 minus
  return s;
}

std::string upper(const std::string &text) {
  std::string out;
  out.reserve(text.size());
  size_t i = 0;
  while (i < text.size()) {
    uint32_t cp = ::text::nextCodepoint(text, i);
    if ((cp >= 'a' && cp <= 'z') || (cp >= 0xE0 && cp <= 0xFE && cp != 0xF7)) cp -= 0x20;
    appendUtf8(out, cp);
  }
  return out;
}

}  // namespace ui
