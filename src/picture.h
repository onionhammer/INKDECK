// Pictures: JPEG (bitbank2/JPEGDEC) and PNG (bitbank2/PNGdec), from memory (a book's pictures)
// or streamed from a file (Files' picture viewer), decoded to 8-bit grey, scaled to the size the
// layout wants, contrast-stretched with a little gamma lift (dithered pictures read dark on the
// panel), then Atkinson-dithered to 1 bit. Atkinson diffuses only 3/4 of the error, which keeps
// line art crisp and photos from going muddy. GIF and SVG files aren't decoded; the Reader shows
// a placeholder for them.
#pragma once
#include <Adafruit_GFX.h>
#include <limits.h>
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

// Pixel size that fits a w x h picture into a box (aspect kept, never larger than the box).
void fit(int w, int h, int boxW, int boxH, int &outW, int &outH);

// Copies a bitmap into the canvas with its top-left at (x, y): ink black, the rest white. Writes
// the canvas buffer directly (honouring its rotation), much faster than drawBitmap(). Only canvas
// rows top..bottom-1 are written (a picture that runs under the chrome bands).
void draw(GFXcanvas1 &canvas, int x, int y, const Bitmap &bitmap, int top = 0,
          int bottom = INT_MAX);

// Decodes, scales and dithers to exactly w x h. False if it can't: an unsupported format, a
// broken file, a picture over about 4 megapixels (PNG), or no memory.
bool render(const std::string &data, int w, int h, Bitmap &out);

// The same for a picture file on the card, its format sniffed from the first bytes rather than
// the name. Streamed through the decoders instead of read into memory, so a JPEG can be any
// size (PNG: as above, and at most 2400 px wide, PNG_MAX_BUFFERED_PIXELS in platformio.ini).
// A JPEG's EXIF orientation is applied, so phone photos come out upright, and w x h is the size
// as shown; the in-memory functions above leave the orientation alone.
bool probeFile(const std::string &path, int &w, int &h);  // header only
bool renderFile(const std::string &path, int w, int h, Bitmap &out);

}  // namespace picture
