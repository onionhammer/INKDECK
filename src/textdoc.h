// A plain-text file for the text reader (apps/textview.h): decoded to flow text (text.h's glyph
// slots, one byte per glyph, '\n' between lines) and broken into lines for a font and width.
//
// Decoding: UTF-8; Windows-1252 when the file isn't valid UTF-8; UTF-16 with a byte-order mark.
// Tabs become spaces to the next multiple of 4 columns, other control characters are dropped.
//
// Reflow: prose that was hard-wrapped at a fixed width (Project Gutenberg's 70-odd columns) is
// joined back into paragraphs, so it wraps to the screen instead of in ragged pairs of lines.
// Only .txt/.text/.md/.markdown files and files without an extension, and only when most lines
// end near the same column; a short line, an indented one, a blank one or a Markdown list item,
// heading, quote or table row keeps its break. Everything else keeps every line break.
#pragma once
#include <Adafruit_GFX.h>
#include <stdint.h>
#include <stdio.h>

#include <string>
#include <vector>

class TextDoc {
 public:
  // A bigger file shows its first 4 MB of text (less when PSRAM is short).
  static constexpr size_t kMaxBytes = 4u << 20;

  enum class Encoding : uint8_t { Utf8, Cp1252, Utf16 };

  // Reads and decodes path. False with err set if it can't be read or isn't text.
  bool open(const std::string &path, std::string &err);

  // True if the start of the file reads as text: no NUL bytes and hardly any control
  // characters (UTF-16 with a byte-order mark counts). An empty file counts.
  static bool looksLikeText(const std::string &path);
  // .txt, .text, .md, .markdown or no extension: prose, as far as the name says (reflow, and
  // the text reader's serif default).
  static bool proseName(const std::string &path);

  // Breaks the text into lines at most width px wide in font, at spaces (or after a hyphen),
  // or anywhere in a word too long for a line. Call again after changing the font.
  void layout(const GFXfont *font, int width);

  int lineCount() const { return static_cast<int>(lines_.size()); }
  bool empty() const { return text_.empty(); }
  // Flow offset of line i's first glyph, and the line holding offset (for keeping the place
  // through a font change, or a saved one).
  uint32_t lineStart(int i) const { return lines_[i].start; }
  int lineAt(uint32_t offset) const;

  // Draws line i with its baseline at y. justify: stretch the spaces of a line that wraps
  // mid-paragraph to the full width (prose; off for monospace).
  void drawLine(GFXcanvas1 &canvas, int i, int x, int baseline, bool justify) const;

  uint64_t fileBytes() const { return fileBytes_; }
  uint64_t bytesShown() const { return bytesUsed_; }  // of the file; less when truncated
  bool truncated() const { return bytesUsed_ < fileBytes_; }
  bool reflowed() const { return reflowed_; }
  Encoding encoding() const { return encoding_; }

 private:
  struct Line {
    uint32_t start;
    uint16_t length;
    bool wraps;  // ends where the text was wrapped, not at a line break
  };

  bool decode(FILE *f);
  void reflow(const std::string &path);
  void wrapParagraph(uint32_t begin, uint32_t end);
  int width(uint32_t from, uint32_t to) const;

  std::string text_;
  std::vector<Line> lines_;
  const GFXfont *font_ = nullptr;
  int width_ = 0;
  uint64_t fileBytes_ = 0;
  uint64_t bytesUsed_ = 0;
  bool reflowed_ = false;
  Encoding encoding_ = Encoding::Utf8;
};
