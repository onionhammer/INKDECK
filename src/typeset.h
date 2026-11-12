// Drawing helpers on top of Adafruit GFX, plus a simple typesetter for flow text (text.h):
// greedy word wrap, justified lines, paragraph indent, bold/italic runs. No hyphenation.
#pragma once
#include <Adafruit_GFX.h>

#include <string>
#include <vector>

#include "fonts.h"

namespace gfx {

constexpr uint16_t kBlack = 0;  // framebuffer bit 0 = black on this panel
constexpr uint16_t kWhite = 1;

int advance(const GFXfont *font, char c);
int textWidth(const GFXfont *font, const std::string &s);
int ascent(const GFXfont *font);  // baseline offset from the top of a line

// Draws one character with its baseline at y; returns the x for the next character.
int drawChar(GFXcanvas1 &canvas, int x, int baseline, char c, const GFXfont *font, uint16_t color);
int drawText(GFXcanvas1 &canvas, int x, int baseline, const std::string &s, const GFXfont *font,
             uint16_t color = kBlack);

// Shortens s to fit maxWidth, replacing the tail (or head) with "...".
std::string ellipsize(const GFXfont *font, const std::string &s, int maxWidth);
std::string ellipsizeFront(const GFXfont *font, const std::string &s, int maxWidth);

}  // namespace gfx

// Where a page starts in the flow text, and the style in effect there.
struct PagePos {
  uint32_t offset = 0;
  uint8_t style = 0;
};

class Typesetter {
 public:
  Typesetter(const FontFamily &family, int width, int height);

  // Lays out one page from `start`. Draws it if canvas is given, with the text area's top-left
  // at (x, y). Returns where the next page starts (offset == flow.size() at the end).
  PagePos layoutPage(const std::string &flow, PagePos start, GFXcanvas1 *canvas = nullptr,
                     int x = 0, int y = 0) const;

  // Start positions of every page. Always at least one entry.
  std::vector<PagePos> paginate(const std::string &flow) const;

  // True if anything printable remains at or after offset.
  static bool hasContent(const std::string &flow, size_t offset);

 private:
  struct Word {
    uint32_t start;
    uint32_t end;
    int width;
    uint8_t style;
  };

  void drawLine(GFXcanvas1 &canvas, const std::string &flow, const std::vector<Word> &words,
                int wordsWidth, bool justify, bool center, int indent, int x, int top) const;

  const FontFamily &family_;
  int width_;
  int height_;
  int lineHeight_;
  int ascent_;
  int spaceWidth_;
  int indent_;
  int paraGap_;
};
