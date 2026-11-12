// UI typography: JetBrains Mono bitmap faces (ui/fonts/jbmono.h) wrapped in text styles, and
// UTF-8 text measuring/drawing on top of them.
//
// Text is UTF-8. ASCII, Latin-1 and the UI symbols in kExtraCodepoints (▲▼▶▸●✓−…) draw from
// the font; anything else is transliterated to ASCII (text.h).
#pragma once
#include <string>
#include <vector>

#include "ui/draw.h"
#include "ui/fonts/jbmono.h"

namespace ui {

struct TextStyle {
  const GFXfont *font;
  int8_t tracking;  // extra px between glyphs: the canvas's CSS letter-spacing, rounded
};

// Semantic styles. Sizes, weights and spacing follow the design canvas.
namespace style {
inline constexpr TextStyle kStatusTag{&fonts::kMono14ExtraBold, 1};  // inverted module tag
inline constexpr TextStyle kStatus{&fonts::kMono14Bold, 1};          // battery %, clock
inline constexpr TextStyle kStatusContext{&fonts::kMono14Medium, 1};  // path / title
inline constexpr TextStyle kSection{&fonts::kMono13Bold, 1};         // "// LABEL"
inline constexpr TextStyle kHint{&fonts::kMono13Bold, 1};            // key-hint verbs
inline constexpr TextStyle kCap{&fonts::kMono13ExtraBold, 0};        // key caps
inline constexpr TextStyle kLabel{&fonts::kMono13Bold, 1};           // small caps lines
inline constexpr TextStyle kKey{&fonts::kMono13Medium, 1};           // stat keys
inline constexpr TextStyle kTag{&fonts::kMono12Bold, 1};             // boxed tags in rows
inline constexpr TextStyle kPill{&fonts::kMono13ExtraBold, 2};       // BUS:SD, ▶ PLAYING
inline constexpr TextStyle kRowIndex{&fonts::kMono14Medium, 0};      // "01"
inline constexpr TextStyle kRow{&fonts::kMono17Bold, 0};             // list row label
inline constexpr TextStyle kRowSelected{&fonts::kMono17ExtraBold, 0};
inline constexpr TextStyle kRowSmall{&fonts::kMono15Bold, 0};        // two-line row title
inline constexpr TextStyle kRowSmallSelected{&fonts::kMono15ExtraBold, 0};
inline constexpr TextStyle kBody{&fonts::kMono15Medium, 0};          // blurbs, paragraphs
inline constexpr TextStyle kValue{&fonts::kMono16Bold, 0};           // stat values
inline constexpr TextStyle kButton{&fonts::kMono17ExtraBold, 1};     // "● OPEN"
inline constexpr TextStyle kTitle{&fonts::kMono38ExtraBold, 0};      // ASCII only
inline constexpr TextStyle kHuge{&fonts::kMono104ExtraBold, -6};     // digits and ':' only
}  // namespace style

// UTF-8 for the symbols the UI uses (all in the fonts' extra slots or Latin-1).
namespace glyph {
inline constexpr const char *kDial = "\xE2\x96\xB2\xE2\x96\xBC";  // ▲▼
inline constexpr const char *kUp = "\xE2\x96\xB2";                // ▲
inline constexpr const char *kDown = "\xE2\x96\xBC";              // ▼
inline constexpr const char *kPress = "\xE2\x97\x8F";             // ●
inline constexpr const char *kHoldPress = "HOLD \xE2\x97\x8F";    // HOLD ●
inline constexpr const char *kPlay = "\xE2\x96\xB6";              // ▶
inline constexpr const char *kPointer = "\xE2\x96\xB8";           // ▸
inline constexpr const char *kDot = "\xC2\xB7";                   // ·
inline constexpr const char *kSection = "\xC2\xA7";               // §
inline constexpr const char *kEmDash = "\xE2\x80\x94";            // —
inline constexpr const char *kEllipsis = "\xE2\x80\xA6";          // …
}  // namespace glyph

enum class Align : uint8_t { Left, Center, Right };

int textWidth(const TextStyle &s, const std::string &text);
int capHeight(const TextStyle &s);  // height of 'H' above the baseline
int lineHeight(const TextStyle &s);

// Baseline that visually centres capitals in a band [top, top + h).
int baselineCentered(const TextStyle &s, int top, int h);

// Draws text with its baseline at y; returns the x after the last glyph.
int drawText(Canvas &c, int x, int baseline, const std::string &text, const TextStyle &s,
             uint16_t color = kInk);
// Draws text in a box: vertically centred, aligned horizontally, ellipsized to fit.
void drawText(Canvas &c, const Rect &box, const std::string &text, const TextStyle &s,
              Align align = Align::Left, uint16_t color = kInk);

// Shortens text to maxWidth with a trailing (or leading) "…".
std::string ellipsize(const TextStyle &s, const std::string &text, int maxWidth);
std::string ellipsizeFront(const TextStyle &s, const std::string &text, int maxWidth);

// Greedy word wrap into at most maxLines lines; the last line is ellipsized.
std::vector<std::string> wrap(const TextStyle &s, const std::string &text, int width,
                              int maxLines);

// Upper-cases ASCII and Latin-1 letters (the design sets most labels in capitals).
std::string upper(const std::string &text);

}  // namespace ui
