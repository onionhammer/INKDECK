#include "ui/packed_fonts.h"

#include <esp32-hal-log.h>
#include <esp_heap_caps.h>
#include <miniz.h>  // ESP32-S3 ROM tinfl
#include <stdlib.h>

#include <memory>
#include <type_traits>

#include "ui/fonts/glyphs.h"

namespace ui::fonts {
namespace {

static_assert(std::is_standard_layout_v<PackedFont>, "drawable() casts a font to its PackedFont");

// The fonts inflated so far, newest first. There are about ten packed fonts, and a screen draws
// with one or two of them.
struct Inflated {
  const GFXfont *packed;
  GFXfont font;
  Inflated *next;
};
Inflated *inflated = nullptr;

uint8_t *inflate(const PackedFont &p) {
  // About 11 KB of decoder state, only while inflating. tinfl_decompress_mem_to_mem() would put
  // it on the stack, which the loop task (8 KB) doesn't have.
  std::unique_ptr<tinfl_decompressor, void (*)(void *)> state(
      static_cast<tinfl_decompressor *>(malloc(sizeof(tinfl_decompressor))), free);
  auto *bits = static_cast<uint8_t *>(heap_caps_malloc(p.size, MALLOC_CAP_SPIRAM));
  if (!state || !bits) {
    free(bits);
    return nullptr;
  }
  tinfl_init(state.get());
  size_t in = p.deflatedSize;
  size_t out = p.size;
  const tinfl_status status = tinfl_decompress(state.get(), p.deflated, &in, bits, bits, &out,
                                               TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
  if (status != TINFL_STATUS_DONE || out != p.size) {
    free(bits);
    return nullptr;
  }
  return bits;
}

}  // namespace

const GFXfont *drawable(const GFXfont *font) {
  if (!font || font->bitmap) return font;
  for (const Inflated *i = inflated; i; i = i->next) {
    if (i->packed == font) return &i->font;
  }
  const auto &packed = *reinterpret_cast<const PackedFont *>(font);
  uint8_t *bits = inflate(packed);
  if (!bits) {
    log_e("can't inflate %u bytes of font bitmaps", packed.size);
    return nullptr;
  }
  inflated = new Inflated{font, *font, inflated};
  inflated->font.bitmap = bits;
  return &inflated->font;
}

}  // namespace ui::fonts
