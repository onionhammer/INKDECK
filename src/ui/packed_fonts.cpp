#include "ui/packed_fonts.h"

#include <esp32-hal-log.h>
#include <esp_heap_caps.h>
#include <miniz.h>  // ESP32-S3 ROM tinfl
#include <stdlib.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <type_traits>

#include "ui/fonts/glyphs.h"

namespace ui::fonts {
namespace {

static_assert(std::is_standard_layout_v<PackedFont>, "drawable() casts a font to its PackedFont");

// The fonts unpacked so far, by PackedFont::index. Text is measured and drawn from more than one
// task: reading a font that's there takes no lock, unpacking one does.
std::atomic<const GFXfont *> unpacked[kFontCount];
std::mutex unpacking;

const GFXglyph kNoGlyph = {};

// Inflates p's stream into out (size bytes). About 11 KB of decoder state, only while
// inflating: tinfl_decompress_mem_to_mem() would put it on the stack, which the loop task (8 KB)
// doesn't have.
bool inflate(const PackedFont &p, uint8_t *out, size_t size) {
  std::unique_ptr<tinfl_decompressor, void (*)(void *)> state(
      static_cast<tinfl_decompressor *>(malloc(sizeof(tinfl_decompressor))), free);
  if (!state) return false;
  tinfl_init(state.get());
  size_t in = p.packedSize;
  size_t got = size;
  const tinfl_status status = tinfl_decompress(state.get(), p.packed, &in, out, out, &got,
                                               TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
  return status == TINFL_STATUS_DONE && got == size;
}

// One PSRAM block per font: the GFXfont, its glyph table, then the stream as it inflates (five
// columns of a byte a glyph: width, height, xAdvance, xOffset, yOffset; then the bitmaps, which
// GFX reads where they land). The glyph table is built from the columns; a glyph's bitmap
// starts where the one before ends, so the offsets are a running sum.
const GFXfont *unpack(const GFXfont *font, const PackedFont &p) {
  const size_t n = font->last - font->first + 1;
  const size_t streamSize = 5 * n + p.bitmapSize;
  auto *block = static_cast<uint8_t *>(heap_caps_malloc(
      sizeof(GFXfont) + n * sizeof(GFXglyph) + streamSize, MALLOC_CAP_SPIRAM));
  if (!block) return nullptr;
  auto *glyphs = reinterpret_cast<GFXglyph *>(block + sizeof(GFXfont));
  uint8_t *column = reinterpret_cast<uint8_t *>(glyphs + n);
  if (!inflate(p, column, streamSize)) {
    free(block);
    return nullptr;
  }
  size_t offset = 0;
  for (size_t i = 0; i < n; ++i) {
    GFXglyph &g = glyphs[i];
    g.bitmapOffset = static_cast<uint16_t>(offset);
    g.width = column[i];
    g.height = column[n + i];
    g.xAdvance = column[2 * n + i];
    g.xOffset = static_cast<int8_t>(column[3 * n + i]);
    g.yOffset = static_cast<int8_t>(column[4 * n + i]);
    offset += (g.width * g.height + 7) / 8;
  }
  auto *out = reinterpret_cast<GFXfont *>(block);
  *out = *font;
  out->bitmap = column + 5 * n;
  out->glyph = glyphs;
  return out;
}

}  // namespace

const GFXfont *drawable(const GFXfont *font) {
  if (!font || font->bitmap) return font;  // none, or one already unpacked
  const auto &packed = *reinterpret_cast<const PackedFont *>(font);
  std::atomic<const GFXfont *> &slot = unpacked[packed.index];
  if (const GFXfont *have = slot.load()) return have;
  std::lock_guard<std::mutex> lock(unpacking);
  if (const GFXfont *have = slot.load()) return have;  // another task got there first
  const GFXfont *out = unpack(font, packed);
  if (!out) {
    static bool said = false;  // once: every glyph measured or drawn would say it again
    if (!said) log_e("can't unpack a font (%u bytes of bitmaps): out of memory", packed.bitmapSize);
    said = true;
    return nullptr;
  }
  slot.store(out);
  return out;
}

const GFXglyph &glyph(const GFXfont *font, uint8_t slot) {
  const GFXfont *f = drawable(font);
  if (!f || slot < f->first || slot > f->last) return kNoGlyph;
  return f->glyph[slot - f->first];
}

}  // namespace ui::fonts
