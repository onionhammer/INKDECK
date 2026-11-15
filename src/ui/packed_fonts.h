// Drawing the large fonts, whose bitmaps are stored deflated (ui/fonts/glyphs.h: PackedFont).
//
// Pass a font through drawable() before handing it to a canvas's setFont(). It returns most
// fonts unchanged. For a packed one it inflates the bitmaps into PSRAM the first time and
// returns a copy of the font that points at them. The copy is kept, so the cost is paid once
// per boot. Measuring needs only the glyph table and works on the font as it is.
#pragma once
#include <stdint.h>  // before gfxfont.h, which uses its types without including it

#include <gfxfont.h>

namespace ui::fonts {

// The font to draw with. Call it from the task that draws (the cache has no lock). If the
// bitmaps can't be inflated (out of memory), it returns nullptr, which GFX draws in its
// built-in 6x8 font.
const GFXfont *drawable(const GFXfont *font);

}  // namespace ui::fonts
