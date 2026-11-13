#include "book_pictures.h"

#include <Arduino.h>
#include <math.h>

#include <algorithm>

#include "text.h"

namespace {

// Copies a bitmap into the canvas's buffer directly (drawBitmap() is a virtual drawPixel per
// pixel, about 4x slower for a page-sized picture), honouring the canvas rotation the way
// GFXcanvas1::drawPixel does. Pixels off the canvas are dropped.
void blit(GFXcanvas1 &canvas, int x0, int y0, const picture::Bitmap &b) {
  uint8_t *buf = canvas.getBuffer();
  const int rot = canvas.getRotation();
  const int cw = canvas.width(), ch = canvas.height();
  const int rawW = (rot & 1) ? ch : cw, rawH = (rot & 1) ? cw : ch;
  const int stride = (b.w + 7) / 8;
  for (int y = 0; y < b.h; ++y) {
    const int py = y0 + y;
    if (py < 0 || py >= ch) continue;
    const uint8_t *row = b.bits.data() + y * stride;
    for (int x = 0; x < b.w; ++x) {
      const int px = x0 + x;
      if (px < 0 || px >= cw) continue;
      int rx = px, ry = py;
      switch (rot) {
        case 1: rx = rawW - 1 - py; ry = px; break;
        case 2: rx = rawW - 1 - px; ry = rawH - 1 - py; break;
        case 3: rx = py; ry = rawH - 1 - px; break;
        default: break;
      }
      uint8_t &byte = buf[(rx + ry * rawW) / 8];
      const uint8_t mask = 0x80 >> (rx & 7);
      if (row[x / 8] & (0x80 >> (x & 7))) {
        byte &= ~mask;  // ink: bit 0 on this panel (gfx::kBlack)
      } else {
        byte |= mask;
      }
    }
  }
}

}  // namespace

void BookPictures::reset(const Epub *book, std::vector<Epub::Image> images) {
  book_ = book;
  images_ = std::move(images);
  info_.assign(images_.size(), Info{});
  cache_.clear();
}

const BookPictures::Info &BookPictures::probe(int index) {
  Info &info = info_[index];
  if (info.probed) return info;
  info.probed = true;
  std::string data, err;
  if (!book_ || !book_->readFile(images_[index].path, data, err)) {
    log_w("picture: %s: %s", images_[index].path.c_str(), err.c_str());
    return info;
  }
  info.ok = picture::size(data, info.w, info.h);
  if (!info.ok) log_w("picture: %s: not a JPEG or PNG we can read", images_[index].path.c_str());
  return info;
}

void BookPictures::layoutSize(int index, int maxW, int maxH, int &w, int &h) {
  w = h = 0;
  if (index < 0 || index >= static_cast<int>(images_.size())) return;
  const Info &info = probe(index);
  if (!info.ok) {  // one line for the placeholder
    w = maxW;
    h = font_ ? font_->yAdvance : 20;
    return;
  }
  // Shrink to fit. Grow a picture that is already a good part of the column, up to 2x (small
  // covers); leave small ones (ornaments, icons) at their own size.
  float scale = std::min(static_cast<float>(maxW) / info.w, static_cast<float>(maxH) / info.h);
  if (scale > 1) {
    const bool large = info.w * 3 >= maxW || info.h * 3 >= maxH;
    scale = large ? std::min(scale, 2.0f) : 1.0f;
  }
  w = std::max(1, static_cast<int>(lroundf(info.w * scale)));
  h = std::max(1, static_cast<int>(lroundf(info.h * scale)));
}

void BookPictures::draw(GFXcanvas1 &canvas, int index, int x, int y, int w, int h) {
  if (index < 0 || index >= static_cast<int>(images_.size())) return;
  if (!probe(index).ok) {
    drawPlaceholder(canvas, index, x, y, w, h);
    return;
  }
  auto it = std::find_if(cache_.begin(), cache_.end(), [&](const Cached &c) {
    return c.index == index && c.bitmap.w == w && c.bitmap.h == h;
  });
  if (it == cache_.end()) {
    Cached c{index, {}};
    std::string data, err;
    if (!book_->readFile(images_[index].path, data, err) ||
        !picture::render(data, w, h, c.bitmap)) {
      log_w("picture: %s: couldn't decode", images_[index].path.c_str());
      canvas.drawRect(x, y, w, h, gfx::kBlack);
      drawPlaceholder(canvas, index, x, y, w, h);
      return;
    }
    if (cache_.size() >= kCacheSize) cache_.erase(cache_.begin());
    cache_.push_back(std::move(c));
    it = cache_.end() - 1;
  } else {
    std::rotate(it, it + 1, cache_.end());  // now the most recent
    it = cache_.end() - 1;
  }
  blit(canvas, x, y, it->bitmap);
}

void BookPictures::drawPlaceholder(GFXcanvas1 &canvas, int index, int x, int y, int w,
                                   int h) const {
  if (!font_) return;
  const std::string &alt = images_[index].alt;
  std::string label = "[";
  size_t i = 0;
  while (i < alt.size()) text::appendSlots(label, text::nextCodepoint(alt, i));
  label = alt.empty() ? "[image]" : label + "]";
  label = gfx::ellipsize(font_, label, w);
  const int tw = gfx::textWidth(font_, label);
  gfx::drawText(canvas, x + (w - tw) / 2, y + (h - font_->yAdvance) / 2 + gfx::ascent(font_), label,
                font_);
}
