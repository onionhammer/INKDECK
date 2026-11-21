// A note for the Note screen (apps/note.h): the text the agent puts on the screen (a
// recipe, steps, a list), in a small Markdown subset, parsed into blocks and laid out in pages.
//
// What it understands, one construct per line:
//   # Heading                 any level; a first heading that repeats the title is dropped
//   - item  * item  + item    bullets; indented two spaces or more, one level in
//   1. step  1) step          numbered, with the number given
//   - [ ] item  - [x] item    a checklist
//   > quote                   indented behind a bar
//   ---  ***  ___             a rule
//   **bold**, __bold__, *italic*, _italic_ anywhere in a line
// A blank line ends a paragraph or an item; a single line break inside one is kept, as a chat
// would show it. Everything else comes out as plain text, whatever arrives: `code` and ```
// fences lose their marks, [links](url) and ![images](url) keep their text, table rows become
// their cells joined by " · " (the |---| line goes), HTML tags go and the common entities are
// decoded. Characters the fonts lack become ASCII ("⅓" -> "1/3", "℃" -> "°C"), ✓ ☑ become ■,
// ☐ becomes □, and emoji go.
//
// The layout: the title in 28 px ExtraBold capitals (up to three lines) over a 3 px rule, then
// the blocks in the body family (Literata), ragged right, half a line between paragraphs and a
// quarter between list items. Headings are 15 px ExtraBold capitals followed by a dashed rule.
// List items hang from a marker column. A line never splits across pages, and a heading never
// ends one.
#pragma once
#include <Adafruit_GFX.h>
#include <stdint.h>

#include <string>
#include <vector>

#include "fonts.h"

class NoteDoc {
 public:
  // title: UTF-8, plain (may be empty: then a first heading becomes it). markdown: the rest.
  void parse(const std::string &title, const std::string &markdown);

  // A line under the title's rule in the UI's small capitals, `left` on the left and `right` on
  // the right (News: the byline, and how long the story is). Call before layout().
  void setByline(const std::string &left, const std::string &right);

  // Lays the note out in pages width x height px, the text in body's faces. Call again after
  // changing the size.
  void layout(const FontFamily &body, int width, int height);

  bool empty() const { return lines_.empty(); }
  int pageCount() const { return pages_.empty() ? 1 : static_cast<int>(pages_.size()); }
  // Draws page `page` with the column's top-left at (x, y).
  void drawPage(GFXcanvas1 &canvas, int page, int x, int y) const;

  // Where page `page` starts in the text, and the page that holds an offset: for keeping the
  // place through a size change, or a saved one.
  uint32_t pageOffset(int page) const;
  int pageAt(uint32_t offset) const;

  // a and b say the same, ignoring case, spaces at the ends and emphasis marks (* and _).
  static bool sameText(const std::string &a, const std::string &b);

 private:
  enum class Kind : uint8_t { Paragraph, Bullet, Number, Check, Quote, Heading, Rule, Row, Code };
  struct Block {
    Kind kind;
    uint8_t level;        // list nesting: 0, or 1 one level in
    bool checked;         // Check: done
    uint32_t start, end;  // its text in text_: flow (text.h) with '\n' for a line break
    std::string marker;   // Number: "12."
  };
  enum class LineKind : uint8_t { Title, TitleRule, Byline, Text, Heading, Rule };
  struct Line {
    uint32_t start, end;  // its text: in title_ for a title line, else in text_
    int16_t x, y;         // the text's left edge in the column, and the line's top in the page
    uint16_t block;
    uint8_t style;        // the flow style in effect at start
    LineKind kind;
    bool first;           // the block's first line: a list item's marker goes beside it
    bool more;            // the title's last line, with more cut off: an ellipsis after it
  };
  struct Span {
    uint32_t start, end;
    uint8_t style;
  };

  static bool isItem(Kind k) { return k == Kind::Bullet || k == Kind::Number || k == Kind::Check; }
  void addBlock(Kind kind, uint8_t level, bool checked, const std::string &marker,
                const std::string &raw);
  // Breaks src[from, to) into spans at most width px wide, at spaces; '\n' forces a break, and
  // a word too long for a line breaks anywhere. faceFor(style) gives the glyphs' font.
  template <typename FaceFor>
  static void wrap(const std::string &src, uint32_t from, uint32_t to, int width, int tracking,
                   FaceFor faceFor, std::vector<Span> &out);
  void drawMarker(GFXcanvas1 &canvas, const Block &b, int right, int baseline) const;

  std::string title_;  // flow in the title font's slots, capitals
  std::string bylineLeft_;   // UTF-8
  std::string bylineRight_;
  std::string text_;
  std::vector<Block> blocks_;
  std::vector<Line> lines_;
  std::vector<uint32_t> pages_;  // index in lines_ of each page's first line
  const FontFamily *body_ = nullptr;
  int width_ = 0;
  int height_ = 0;
  int markerW_ = 0;    // the marker column, its gap included
  int markerGap_ = 0;  // between a marker's right edge and the item's text
};
