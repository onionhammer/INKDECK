#include "typeset.h"

#include <algorithm>

#include "text.h"
#include "ui/fonts/glyphs.h"
#include "ui/packed_fonts.h"

namespace gfx {
namespace {

const GFXglyph *glyph(const GFXfont *font, char c) {
  auto ch = static_cast<uint8_t>(c);
  if (ch < font->first || ch > font->last) ch = '?';
  return &font->glyph[ch - font->first];
}

}  // namespace

int advance(const GFXfont *font, char c) { return glyph(font, c)->xAdvance; }

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
  auto ch = static_cast<uint8_t>(c);
  if (ch < font->first || ch > font->last) ch = '?';
  canvas.setFont(ui::fonts::drawable(font));
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

bool isAsciiLetter(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }

// A line may break after these: a hyphen, an en dash, an em dash.
bool isDash(char c) {
  constexpr char kEnDash = static_cast<char>(ui::fonts::slotFor(0x2013));
  constexpr char kEmDash = static_cast<char>(ui::fonts::slotFor(0x2014));
  return c == '-' || (c != 0 && (c == kEnDash || c == kEmDash));
}

}  // namespace

Typesetter::Typesetter(const FontFamily &family, int width, int height)
    : family_(family), width_(width), height_(height) {
  const GFXfont *regular = family.face[0];
  lineHeight_ = family.lineHeight ? family.lineHeight : regular->yAdvance;
  // Centre capitals in the line, as CSS's half-leading roughly does.
  const int cap = -gfx::glyph(regular, 'H')->yOffset;
  ascent_ = (lineHeight_ + cap) / 2;
  spaceWidth_ = gfx::advance(regular, ' ');
  indent_ = gfx::advance(regular, 'M');
  headingGap_ = lineHeight_ / 2;
}

bool Typesetter::hasContent(const std::string &flow, size_t offset) {
  for (size_t i = offset; i < flow.size(); ++i) {
    if (flow::isGlyph(flow[i]) || flow[i] == flow::kImage) return true;
  }
  return false;
}

uint32_t Typesetter::findDropCap(const std::string &flow, size_t minLength) {
  size_t start = 0;
  while (start < flow.size()) {
    size_t end = flow.find(flow::kPara, start);
    if (end == std::string::npos) end = flow.size();
    size_t i = start;
    while (i < end && flow::isStyle(flow[i])) ++i;
    if (i < end && flow[i] != flow::kCenter && end - start >= minLength) {
      return isAsciiLetter(flow[i]) ? static_cast<uint32_t>(i) : DropCap::kNone;
    }
    start = end + 1;
  }
  return DropCap::kNone;
}

PagePos Typesetter::layoutPage(const std::string &flow, PagePos start, GFXcanvas1 *canvas,
                               int x, int y) const {
  const size_t n = flow.size();
  size_t pos = start.offset;
  uint8_t style = start.style;
  int lineTop = 0;
  bool firstLine = isParagraphStart(flow, pos);
  bool center = false;
  bool afterHeading = false;  // no indent on the paragraph after a centred one
  bool afterPicture = false;  // nor after a picture
  int dropBottom = 0;         // lines above this y make room for the drop cap
  std::vector<Word> words;
  words.reserve(32);
  std::vector<Break> breaks;
  int wordsWidth = 0;
  PagePos lineStart{static_cast<uint32_t>(pos), style};

  auto indentNow = [&] {
    if (lineTop < dropBottom) return drop_.box + drop_.gap;
    return firstLine && !center && !afterHeading ? indent_ : 0;
  };
  auto flush = [&](bool justify) {
    if (canvas) {
      drawLine(*canvas, flow, words, wordsWidth, justify, center, indentNow(), x, y + lineTop);
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
      if (!words.empty()) flush(false);
      if (center) lineTop += headingGap_;
      afterHeading = center || afterPicture;
      afterPicture = false;
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
    if (c == flow::kPageBreak) {
      if (!words.empty()) flush(false);
      ++pos;
      if (lineTop > 0) return {static_cast<uint32_t>(pos), style};
      continue;
    }
    if (c == flow::kImage) {
      if (!words.empty()) flush(false);
      const int index =
          pos + 1 < n ? static_cast<uint8_t>(flow[pos + 1]) - flow::kImageBase : -1;
      int w = 0, h = 0;
      if (images_ && index >= 0) images_->layoutSize(index, width_, height_, w, h);
      if (h > 0) {
        const int gap = lineTop > 0 ? headingGap_ : 0;
        if (lineTop > 0 && lineTop + gap + h > height_) {
          // Shrink it into what's left of the page if it keeps at least half its height (a
          // cover under its heading); otherwise it starts the next page.
          int sw = 0, sh = 0;
          images_->layoutSize(index, width_, height_ - lineTop - gap, sw, sh);
          if (sh * 2 < h || sh <= 0) return {static_cast<uint32_t>(pos), style};
          w = sw;
          h = sh;
        }
        if (canvas) images_->draw(*canvas, index, x + (width_ - w) / 2, y + lineTop + gap, w, h);
        lineTop += gap + h + headingGap_;
        lineTop = (lineTop + lineHeight_ - 1) / lineHeight_ * lineHeight_;  // back on the grid
        afterPicture = true;
      }
      pos += 2;
      continue;
    }
    if (c == flow::kCenter) {
      if (lineTop > 0 && words.empty() && !afterHeading) {
        lineTop += headingGap_;
        if (!lineFits()) return lineStart;
      }
      center = true;
      ++pos;
      continue;
    }

    Word w{static_cast<uint32_t>(pos), 0, 0, style};
    if (pos == drop_.at && drop_.font && words.empty()) {
      // The paragraph that gets the drop cap. Start a new page if the box wouldn't fit.
      if (lineTop > 0 && lineTop + drop_.top + drop_.box > height_) return lineStart;
      if (canvas) drawDropCap(*canvas, flow[pos], x, y + lineTop + drop_.top);
      dropBottom = lineTop + drop_.top + drop_.box;
      w.start = static_cast<uint32_t>(++pos);  // the letter is in the box, not the line
    }
    // Measure the word, noting where it may break: after a soft hyphen (which then draws as a
    // hyphen), or after a hyphen or dash.
    breaks.clear();
    int glyphs = 0;
    while (pos < n && flow[pos] != ' ' && flow[pos] != flow::kPara) {
      const char d = flow[pos++];
      if (flow::isStyle(d)) {
        style = d - flow::kStyleBase;
      } else if (d == flow::kSoftHyphen) {
        if (!center) breaks.push_back({static_cast<uint32_t>(pos), w.width, style, glyphs, true});
      } else if (d != flow::kCenter) {
        w.width += gfx::advance(family_.face[style], d);
        ++glyphs;
        if (isDash(d)) breaks.push_back({static_cast<uint32_t>(pos), w.width, style, glyphs, false});
      }
    }
    w.end = static_cast<uint32_t>(pos);
    if (w.end == w.start) continue;  // a one-letter word that went into the drop cap

    const int used = indentNow() + wordsWidth + static_cast<int>(words.size()) * spaceWidth_;
    if (used + w.width > width_) {
      // Too long for what's left of the line: break it at the last place that fits. A soft
      // hyphen needs a letter on each side; a hard hyphen or dash, two.
      const Break *fit = nullptr;
      for (const Break &b : breaks) {
        const int minSide = b.soft ? 1 : 2;
        if (b.glyphs < minSide || glyphs - b.glyphs < minSide) continue;
        const int hyphen = b.soft ? gfx::advance(family_.face[b.style], '-') : 0;
        if (used + b.width + hyphen <= width_) fit = &b;
      }
      if (fit) {
        Word head = w;
        head.end = fit->at;
        head.hyphen = fit->soft;
        head.width = fit->width + (fit->soft ? gfx::advance(family_.face[fit->style], '-') : 0);
        words.push_back(head);
        wordsWidth += head.width;
        flush(true);
        pos = fit->at;  // the rest of the word starts the next line
        style = fit->style;
        lineStart = {fit->at, fit->style};
        if (!lineFits()) return lineStart;
        continue;
      }
      if (!words.empty()) {
        flush(true);
        lineStart = {w.start, w.style};
        if (!lineFits()) return lineStart;
        // Measure it again on the fresh line, where it may still need breaking.
        pos = w.start;
        style = w.style;
        continue;
      }
      // Alone on the line and still too wide, with nowhere to break: it runs over, as before.
    }
    words.push_back(w);
    wordsWidth += w.width;
  }
}

void Typesetter::drawDropCap(GFXcanvas1 &canvas, char letter, int x, int y) const {
  canvas.fillRect(x, y, drop_.box, drop_.box, gfx::kBlack);
  if (letter >= 'a' && letter <= 'z') letter = static_cast<char>(letter - 'a' + 'A');
  const auto slot = static_cast<uint8_t>(letter);
  const GFXfont *f = drop_.font;
  if (slot < f->first || slot > f->last) return;
  const GFXglyph &g = f->glyph[slot - f->first];
  // Centre the glyph's ink in the box.
  const int gx = x + (drop_.box - g.width) / 2 - g.xOffset;
  const int baseline = y + (drop_.box - g.height) / 2 - g.yOffset;
  canvas.setFont(ui::fonts::drawable(f));
  canvas.drawChar(gx, baseline, slot, gfx::kWhite, gfx::kWhite, 1);
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
    cursor = x + indent + std::max(0, (width_ - natural) / 2);
  } else if (justify && gaps > 0) {
    // Without hyphenation a long next word can leave a lot of slack. Lines with a few gaps
    // still justify (like a printed page); a line of two or three words only within reason.
    const int slack = width_ - natural;
    if (slack > 0 && (gaps >= 3 || slack <= gaps * spaceWidth_ * 4)) {
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
      } else if (d != flow::kCenter && d != flow::kSoftHyphen) {
        cx = gfx::drawChar(canvas, cx, baseline, d, family_.face[style], gfx::kBlack);
      }
    }
    if (w.hyphen) gfx::drawChar(canvas, cx, baseline, '-', family_.face[style], gfx::kBlack);
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
