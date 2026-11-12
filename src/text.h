// Text helpers shared by the EPUB parser, typesetter and file browser.
//
// The bundled Adafruit GFX fonts only cover printable ASCII (0x20-0x7E), so all text is
// transliterated to ASCII on the way in ("é" -> "e", curly quotes -> straight quotes, ...).
// Swap in fonts with wider coverage when this stops being a prototype.
#pragma once
#include <stdint.h>

#include <string>

// "Flow" text: what the EPUB parser produces and the typesetter consumes. Printable ASCII
// plus these control bytes. Runs of whitespace are already collapsed to a single ' '.
namespace flow {
constexpr char kPara = '\n';        // paragraph break
constexpr char kStyleBase = 0x01;   // 0x01..0x04 set style: kStyleBase + (bold | italic << 1)
constexpr char kCenter = 0x05;      // at paragraph start: centre this paragraph
constexpr char kEmDash = 0x7F;      // drawn as a rule; not in the GFX fonts
constexpr uint8_t kBold = 1;
constexpr uint8_t kItalic = 2;

inline bool isStyle(char c) { return c >= kStyleBase && c < kStyleBase + 4; }
}  // namespace flow

namespace text {

// Decodes one UTF-8 sequence at s[i], advancing i. Malformed bytes decode as U+FFFD.
uint32_t nextCodepoint(const std::string &s, size_t &i);

// Appends an ASCII approximation of cp (possibly empty, possibly several chars).
void appendAscii(std::string &out, uint32_t cp);

// UTF-8 string -> ASCII approximation.
std::string toAscii(const std::string &utf8);

}  // namespace text
