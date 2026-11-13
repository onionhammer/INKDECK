// Text helpers shared by the EPUB parser, typesetter, UI and audio tags.
#pragma once
#include <stdint.h>

#include <string>

// "Flow" text: what the EPUB parser produces and the typesetter consumes. One byte per glyph,
// in the fonts' slot scheme (ui/fonts/glyphs.h: ASCII, typographic symbols at 0x80-0x9F,
// Latin-1 at 0xA0-0xFF), plus these control bytes. Runs of whitespace are already collapsed to
// a single ' '.
namespace flow {
constexpr char kPara = '\n';        // paragraph break
constexpr char kStyleBase = 0x01;   // 0x01..0x04 set style: kStyleBase + (bold | italic << 1)
constexpr char kCenter = 0x05;      // at paragraph start: centre this paragraph
// A picture: kImage, then one byte (kImageBase + its index in the chapter's picture list), as a
// paragraph of its own. The index byte is never a control byte, a space or a paragraph break.
constexpr char kImage = 0x06;
constexpr uint8_t kImageBase = 0x21;
constexpr int kMaxImages = 0xFF - kImageBase;
constexpr uint8_t kBold = 1;
constexpr uint8_t kItalic = 2;

inline bool isStyle(char c) { return c >= kStyleBase && c < kStyleBase + 4; }
// True for bytes that draw a glyph (not a space or a control byte).
inline bool isGlyph(char c) { return static_cast<uint8_t>(c) > ' ' && c != 0x7F; }
}  // namespace flow

namespace text {

// Decodes one UTF-8 sequence at s[i], advancing i. Malformed bytes decode as U+FFFD.
uint32_t nextCodepoint(const std::string &s, size_t &i);

void appendUtf8(std::string &out, uint32_t cp);

// Appends an ASCII approximation of cp (possibly empty, possibly several chars).
void appendAscii(std::string &out, uint32_t cp);

// Appends cp as flow text: its glyph slot if the fonts have one, else an ASCII approximation.
// Whitespace becomes ' '; soft hyphens and zero-width characters vanish.
void appendSlots(std::string &out, uint32_t cp);

// UTF-8 string -> ASCII approximation.
std::string toAscii(const std::string &utf8);

}  // namespace text
