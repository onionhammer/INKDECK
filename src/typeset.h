// Drawing helpers on top of Adafruit GFX, plus the typesetter for flow text (text.h): greedy
// word wrap, justified lines on a fixed line grid, paragraph indents, bold/italic runs, centred
// headings with space around them, and an optional drop cap. A word that doesn't fit the line
// breaks at the last soft hyphen (drawn as a hyphen), hard hyphen or dash that fits. There's no
// hyphenation dictionary and no kerning.
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

// An inverted square with the paragraph's first letter, text flowing round it.
struct DropCap {
  static constexpr uint32_t kNone = UINT32_MAX;
  uint32_t at = kNone;            // flow offset of the letter (see Typesetter::findDropCap)
  const GFXfont *font = nullptr;  // capitals; must have the letter
  int box = 84;                   // square side, px
  int top = 4;                    // box offset below its first line's top
  int gap = 12;                   // between the box and the text
};

// Pictures in the flow (flow::kImage): the Reader works out their size and draws them.
class ImageSource {
 public:
  virtual ~ImageSource() = default;
  // The size picture `index` takes in a column maxW wide, at most maxH tall. 0 x 0: nothing.
  virtual void layoutSize(int index, int maxW, int maxH, int &w, int &h) = 0;
  // Draws it with its top-left at (x, y), in the size layoutSize() gave.
  virtual void draw(GFXcanvas1 &canvas, int index, int x, int y, int w, int h) = 0;
};

class Typesetter {
 public:
  Typesetter(const FontFamily &family, int width, int height);

  void setDropCap(const DropCap &drop) { drop_ = drop; }
  // Pictures: centred blocks with half a line of space around them, the text after them back
  // on the line grid. One that doesn't fit what's left of the page starts the next.
  void setImages(ImageSource *images) { images_ = images; }

  // Lays out one page from `start`. Draws it if canvas is given, with the text area's top-left
  // at (x, y). Returns where the next page starts (offset == flow.size() at the end).
  PagePos layoutPage(const std::string &flow, PagePos start, GFXcanvas1 *canvas = nullptr,
                     int x = 0, int y = 0) const;

  // Start positions of every page. Always at least one entry.
  std::vector<PagePos> paginate(const std::string &flow) const;

  // True if anything printable remains at or after offset.
  static bool hasContent(const std::string &flow, size_t offset);

  // Offset of the first letter of the first paragraph that suits a drop cap: left-aligned, at
  // least minLength bytes, starting with an ASCII letter. DropCap::kNone if there's none.
  static uint32_t findDropCap(const std::string &flow, size_t minLength = 200);

  int lineHeight() const { return lineHeight_; }

 private:
  struct Word {
    uint32_t start;
    uint32_t end;
    int width;  // a hyphenated word's includes its hyphen
    uint8_t style;
    bool hyphen = false;  // broken at a soft hyphen: draw '-' after it
  };
  // A place a word may break, just after offset `at`.
  struct Break {
    uint32_t at;
    int width;      // of the word up to it (with the dash, without a soft hyphen's '-')
    uint8_t style;  // in effect there
    int glyphs;     // glyphs before it
    bool soft;
  };

  void drawLine(GFXcanvas1 &canvas, const std::string &flow, const std::vector<Word> &words,
                int wordsWidth, bool justify, bool center, int indent, int x, int top) const;
  void drawDropCap(GFXcanvas1 &canvas, char letter, int x, int y) const;

  const FontFamily &family_;
  int width_;
  int height_;
  int lineHeight_;
  int ascent_;
  int spaceWidth_;
  int indent_;
  int headingGap_;  // space before and after centred paragraphs
  DropCap drop_;
  ImageSource *images_ = nullptr;
};
