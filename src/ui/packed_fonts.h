// Measuring and drawing with the fonts, which are stored packed (ui/fonts/glyphs.h: PackedFont):
// a font's glyph table and bitmaps are deflated together in flash, and its GFXfont points at
// neither.
//
// drawable() gives the font unpacked. It inflates one into PSRAM the first time (a millisecond
// or two; the block stays for the boot) and after that only looks it up. Pass every font through
// it before handing it to a canvas's setFont(), and measure through glyph(): a packed font set
// straight on a canvas, or its glyph table read as it is, is a null pointer.
#pragma once
#include <stdint.h>  // before gfxfont.h, which uses its types without including it

#include <gfxfont.h>

namespace ui::fonts {

// The font to draw with: its glyph table and bitmaps are there. From any task. If it can't be
// unpacked (out of memory) it returns nullptr, which GFX draws in its built-in 6x8 font.
const GFXfont *drawable(const GFXfont *font);

// The glyph in a slot, for measuring. An empty one (no advance, no ink) for a slot outside the
// font's range, or if the font can't be unpacked.
const GFXglyph &glyph(const GFXfont *font, uint8_t slot);

}  // namespace ui::fonts
