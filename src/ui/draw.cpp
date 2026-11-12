#include "ui/draw.h"

namespace ui {

void fill(Canvas &c, const Rect &r, uint16_t color) {
  if (r.w > 0 && r.h > 0) c.fillRect(r.x, r.y, r.w, r.h, color);
}

void hrule(Canvas &c, int x, int y, int w, int thickness, uint16_t color) {
  fill(c, {x, y, w, thickness}, color);
}

void vrule(Canvas &c, int x, int y, int h, int thickness, uint16_t color) {
  fill(c, {x, y, thickness, h}, color);
}

void dashedH(Canvas &c, int x, int y, int w, uint16_t color) {
  for (int i = 0; i < w; i += 6) c.drawFastHLine(x + i, y, w - i < 3 ? w - i : 3, color);
}

void dashedV(Canvas &c, int x, int y, int h, uint16_t color) {
  for (int i = 0; i < h; i += 6) c.drawFastVLine(x, y + i, h - i < 3 ? h - i : 3, color);
}

void frame(Canvas &c, const Rect &r, int thickness, uint16_t color) {
  fill(c, {r.x, r.y, r.w, thickness}, color);
  fill(c, {r.x, r.bottom() - thickness, r.w, thickness}, color);
  fill(c, {r.x, r.y, thickness, r.h}, color);
  fill(c, {r.right() - thickness, r.y, thickness, r.h}, color);
}

void dither50(Canvas &c, const Rect &r) {
  for (int y = r.y; y < r.bottom(); ++y) {
    for (int x = r.x + ((r.x + y) & 1); x < r.right(); x += 2) c.drawPixel(x, y, kInk);
  }
}

void dither25(Canvas &c, const Rect &r) {
  for (int y = r.y + (r.y & 1); y < r.bottom(); y += 2) {
    for (int x = r.x + (r.x & 1); x < r.right(); x += 2) c.drawPixel(x, y, kInk);
  }
}

}  // namespace ui
