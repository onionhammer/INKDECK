// Book pictures (phase 5b): JPEG (bitbank2/JPEGDEC) and PNG (bitbank2/PNGdec) from memory,
// decoded to 8-bit grey, scaled to the size the layout wants, contrast-stretched with a little
// gamma lift (dithered pictures read dark on the panel), then Atkinson-dithered to 1 bit.
// Atkinson diffuses only 3/4 of the error, which keeps line art crisp and photos from going
// muddy. GIF and SVG files aren't decoded; the Reader shows a placeholder for them.
#pragma once
#include <stdint.h>

#include <string>
#include <vector>

namespace picture {

enum class Format : uint8_t { Unknown, Jpeg, Png, Gif, Svg };
Format sniff(const std::string &data);

// Pixel size from the file header, without decoding. False if the format isn't one we decode
// or the header is broken.
bool size(const std::string &data, int &w, int &h);

// 1 bpp, rows padded to whole bytes, MSB = leftmost pixel, set bit = ink: the layout of
// Adafruit GFX's drawBitmap(), drawn with ink on paper. Its buffer lands in PSRAM.
struct Bitmap {
  int w = 0;
  int h = 0;
  std::vector<uint8_t> bits;
};

// Decodes, scales and dithers to exactly w x h. False if it can't: an unsupported format, a
// broken file, a picture over about 4 megapixels (PNG), or no memory.
bool render(const std::string &data, int w, int h, Bitmap &out);

}  // namespace picture
