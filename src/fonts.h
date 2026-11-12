// Font selection. Adafruit GFX FreeFonts, ASCII only (see text.h).
//
// The FreeFonts are rasterised at 141 dpi; the panel is ~235 dpi, so "12pt" renders at about
// 7pt physical size. That's why the reader offers the larger sizes too.
#pragma once
#include <Arduino.h>  // gfxfont.h and the font headers need stdint + PROGMEM
#include <gfxfont.h>

struct FontFamily {
  const char *name;
  const GFXfont *face[4];  // indexed by flow style: regular, bold, italic, bold-italic
};

namespace fonts {

constexpr int kReaderSizeCount = 3;
extern const FontFamily kReader[kReaderSizeCount];  // serif 12 / 18 / 24 pt
constexpr int kDefaultReaderSize = 1;

extern const GFXfont *const kUi;        // list rows
extern const GFXfont *const kUiBold;    // headers, folders
extern const GFXfont *const kUiSmall;   // footers, status bar

}  // namespace fonts
