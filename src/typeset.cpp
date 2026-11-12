#include "typeset.h"

#include <algorithm>

#include "text.h"

namespace gfx {
namespace {

const GFXglyph *glyph(const GFXfont *font, char c) {
  auto ch = static_cast<uint8_t>(c);
  if (ch < font->first || ch > font->last) ch = '?';
  return &font->glyph[ch - font->first];
}

}  // namespace

int advance(const GFXfont *font, char c) {
  if (c == flow::kEmDash) return glyph(font, 'M')->xAdvance;
  return glyph(font, c)->xAdvance;
}

int textWidth(const GFXfont *font, const std::string &s) {
  int w = 0;
  for (const char c : s) w += advance(font, c);
  return w;
}

int ascent(const GFXfont *font) {
  int a = 0;
  for (const char c : {'d', 'l', 'T', 'A'}) a = std::max(a, -static_cast<int>(glyph(font, c)->yOffset));
  return a;
}

int drawChar(GFXcanvas1 &canvas, int x, int baseline, char c, const GFXfont *font, uint16_t color) {
  if (c == flow::kEmDash) {
    const int w = advance(font, c);
    const int xHeight = -glyph(font, 'x')->yOffset;
    const int thick = std::max(1, font->yAdvance / 24);
    canvas.fillRect(x + 1, baseline - xHeight / 2 - thick / 2, w - 2, thick, color);
    return x + w;
  }
  auto ch = static_cast<uint8_t>(c);
  if (ch < font->first || ch > font->last) ch = '?';
  canvas.setFont(font);
  canvas.drawChar(x, baseline, ch, color, color, 1);
  return x + font->glyph[ch - font->first].xAdvance;
}

int drawText(GFXcanvas1 &canvas, int x, int baseline, const std::string &s, const GFXfont *font,
             uint16_t color) {
  for (const char c : s) x = drawChar(canvas, x, baseline, c, font, color);
  return x;
}

std::string ellipsize(const GFXfont *font, const std::string &s, int maxWidth) {
  if (textWidth(font, s) <= maxWidth) return s;
  const int dots = textWidth(font, "...");
  std::string out;
  int w = 0;
  for (const char c : s) {
    const int a = advance(font, c);
    if (w + a + dots > maxWidth) break;
    out += c;
    w += a;
  }
  return out + "...";
}

std::string ellipsizeFront(const GFXfont *font, const std::string &s, int maxWidth) {
  if (textWidth(font, s) <= maxWidth) return s;
  const int dots = textWidth(font, "...");
  size_t start = s.size();
  int w = 0;
  while (start > 0) {
    const int a = advance(font, s[start - 1]);
    if (w + a + dots > maxWidth) break;
    w += a;
    --start;
  }
  return "..." + s.substr(start);
}

}  // namespace gfx

namespace {

// True if pos begins a paragraph (only spaces/style bytes between it and a break or the start).
bool isParagraphStart(const std::string &flow, size_t pos) {
  while (pos > 0) {
    const char c = flow[pos - 1];
    if (c == flow::kPara) return true;
    if (c != ' ' && c != flow::kCenter && !flow::isStyle(c)) return false;
    --pos;
  }
  return true;
}

}  // namespace

Typesetter::Typesetter(const FontFamily &family, int width, int height)
    : family_(family), width_(width), height_(height) {
  const GFXfont *regular = family.face[0];
  lineHeight_ = regular->yAdvance;
  ascent_ = gfx::ascent(regular);
  spaceWidth_ = gfx::advance(regular, ' ');
  indent_ = gfx::advance(regular, 'M');
  paraGap_ = lineHeight_ / 4;
}

bool Typesetter::hasContent(const std::string &flow, size_t offset) {
  for (size_t i = offset; i < flow.size(); ++i) {
    const char c = flow[i];
    if ((c > ' ' && c < 0x7F) || c == flow::kEmDash) return true;
  }
  return false;
}

PagePos Typesetter::layoutPage(const std::string &flow, PagePos start, GFXcanvas1 *canvas,
                               int x, int y) const {
  const size_t n = flow.size();
  size_t pos = start.offset;
  uint8_t style = start.style;
  int lineTop = 0;
  bool firstLine = isParagraphStart(flow, pos);
  bool center = false;
  std::vector<Word> words;
  words.reserve(32);
  int wordsWidth = 0;
  PagePos lineStart{static_cast<uint32_t>(pos), style};

  auto flush = [&](bool justify) {
    if (canvas) {
      drawLine(*canvas, flow, words, wordsWidth, justify, center,
               firstLine && !center ? indent_ : 0, x, y + lineTop);
    }
    words.clear();
    wordsWidth = 0;
    lineTop += lineHeight_;
    firstLine = false;
  };
  auto lineFits = [&] { return lineTop + lineHeight_ <= height_; };

  while (true) {
    while (pos < n && flow[pos] == ' ') ++pos;
    if (pos >= n) {
      if (!words.empty()) flush(false);
      return {static_cast<uint32_t>(n), style};
    }
    const char c = flow[pos];
    if (c == flow::kPara) {
      if (!words.empty()) {
        flush(false);
        lineTop += paraGap_;
      }
      ++pos;
      firstLine = true;
      center = false;
      lineStart = {static_cast<uint32_t>(pos), style};
      if (!lineFits()) return lineStart;
      continue;
    }
    if (flow::isStyle(c)) {
      style = c - flow::kStyleBase;
      ++pos;
      continue;
    }
    if (c == flow::kCenter) {
      center = true;
      ++pos;
      continue;
    }

    Word w{static_cast<uint32_t>(pos), 0, 0, style};
    while (pos < n && flow[pos] != ' ' && flow[pos] != flow::kPara) {
      const char d = flow[pos++];
      if (flow::isStyle(d)) {
        style = d - flow::kStyleBase;
      } else if (d != flow::kCenter) {
        w.width += gfx::advance(family_.face[style], d);
      }
    }
    w.end = static_cast<uint32_t>(pos);

    const int indent = firstLine && !center ? indent_ : 0;
    const int needed = indent + wordsWidth + static_cast<int>(words.size()) * spaceWidth_ + w.width;
    if (!words.empty() && needed > width_) {
      flush(true);
      lineStart = {w.start, w.style};
      if (!lineFits()) return lineStart;
    }
    words.push_back(w);
    wordsWidth += w.width;
  }
}

void Typesetter::drawLine(GFXcanvas1 &canvas, const std::string &flow,
                          const std::vector<Word> &words, int wordsWidth, bool justify,
                          bool center, int indent, int x, int top) const {
  if (words.empty()) return;
  const int gaps = static_cast<int>(words.size()) - 1;
  const int natural = indent + wordsWidth + gaps * spaceWidth_;
  int cursor = x + indent;
  int extra = 0, remainder = 0;
  if (center) {
    cursor = x + std::max(0, (width_ - natural) / 2);
  } else if (justify && gaps > 0) {
    const int slack = width_ - natural;
    if (slack > 0 && slack <= gaps * spaceWidth_ * 3) {  // don't stretch sparse lines absurdly
      extra = slack / gaps;
      remainder = slack % gaps;
    }
  }
  const int baseline = top + ascent_;
  for (size_t i = 0; i < words.size(); ++i) {
    const Word &w = words[i];
    uint8_t style = w.style;
    int cx = cursor;
    for (uint32_t p = w.start; p < w.end; ++p) {
      const char d = flow[p];
      if (flow::isStyle(d)) {
        style = d - flow::kStyleBase;
      } else if (d != flow::kCenter) {
        cx = gfx::drawChar(canvas, cx, baseline, d, family_.face[style], gfx::kBlack);
      }
    }
    cursor += w.width + spaceWidth_ + extra + (static_cast<int>(i) < remainder ? 1 : 0);
  }
}

std::vector<PagePos> Typesetter::paginate(const std::string &flow) const {
  std::vector<PagePos> pages;
  PagePos pos;
  while (true) {
    pages.push_back(pos);
    const PagePos next = layoutPage(flow, pos);
    if (next.offset <= pos.offset || !hasContent(flow, next.offset)) break;
    pos = next;
  }
  return pages;
}
