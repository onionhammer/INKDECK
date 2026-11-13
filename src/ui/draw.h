// 1-bit drawing primitives for the INKDECK/OS look (docs/ui-design.md, "Visual rules"):
// ink and paper only, greys as 2x2 dithers, 3 px rules between regions, 1 px dashed rules
// between rows, hard edges.
#pragma once
#include <Adafruit_GFX.h>

namespace ui {

using Canvas = GFXcanvas1;

constexpr uint16_t kInk = 0;    // framebuffer bit 0 = black on this panel
constexpr uint16_t kPaper = 1;  // bit 1 = white

constexpr int kRule = 3;  // region separator thickness

struct Rect {
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;

  int right() const { return x + w; }
  int bottom() const { return y + h; }
  int centerY() const { return y + h / 2; }
  Rect inset(int dx, int dy) const { return {x + dx, y + dy, w - 2 * dx, h - 2 * dy}; }
  // Slices a band off one edge and shrinks this rect by it.
  Rect takeTop(int n) {
    const Rect r{x, y, w, n};
    y += n;
    h -= n;
    return r;
  }
  Rect takeBottom(int n) {
    h -= n;
    return {x, y + h, w, n};
  }
  Rect takeLeft(int n) {
    const Rect r{x, y, n, h};
    x += n;
    w -= n;
    return r;
  }
};

void fill(Canvas &c, const Rect &r, uint16_t color);
void hrule(Canvas &c, int x, int y, int w, int thickness = kRule, uint16_t color = kInk);
void vrule(Canvas &c, int x, int y, int h, int thickness = kRule, uint16_t color = kInk);
void dashedH(Canvas &c, int x, int y, int w, uint16_t color = kInk);  // 1 px, 3 on / 3 off
void dashedV(Canvas &c, int x, int y, int h, uint16_t color = kInk);  // 1 px, 3 on / 3 off
void frame(Canvas &c, const Rect &r, int thickness, uint16_t color = kInk);  // inside r

// Tones. The patterns are anchored to screen coordinates, so adjacent fills line up.
void dither50(Canvas &c, const Rect &r);  // 2x2 checkerboard
void dither25(Canvas &c, const Rect &r);  // one ink pixel per 2x2 cell

}  // namespace ui
