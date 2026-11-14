// Font families for the typesetter (book text). UI fonts are in ui/type.h.
#pragma once
#include <Arduino.h>  // gfxfont.h needs stdint + PROGMEM
#include <gfxfont.h>

struct FontFamily {
  const char *name;
  const GFXfont *face[4];  // indexed by flow style: regular, bold, italic, bold-italic
  int lineHeight;          // px; 0 = the regular face's own line height
};

namespace fonts {

// Literata 16/26, 19/30 (docs/ui-design.md's reader setting) and 23/36.
constexpr int kReaderSizeCount = 3;
extern const FontFamily kReader[kReaderSizeCount];
constexpr int kDefaultReaderSize = 1;

// The text reader's monospace sizes (JetBrains Mono Medium), the counterparts of kReader's.
extern const FontFamily kTextMono[kReaderSizeCount];

extern const GFXfont *const kDropCap;  // JetBrains Mono 68 px ExtraBold, capitals

}  // namespace fonts
