// The per-pixel loops run noticeably faster at -O2 than at the core's -Os.
#pragma GCC optimize("O2")

#include "picture.h"

#include <Arduino.h>
#include <JPEGDEC.h>
#include <PNGdec.h>
#include <esp_heap_caps.h>
#include <math.h>

#include <algorithm>
#include <memory>
#include <new>

namespace picture {
namespace {

constexpr int kMaxPngPixels = 4000000;  // full-size grey buffer in PSRAM
constexpr float kGamma = 0.8f;          // < 1 lifts the mid-tones

const uint8_t *bytes(const std::string &s) { return reinterpret_cast<const uint8_t *>(s.data()); }

// Width and height from a JPEG's start-of-frame segment.
bool jpegSize(const uint8_t *p, size_t n, int &w, int &h) {
  if (n < 4 || p[0] != 0xFF || p[1] != 0xD8) return false;
  size_t i = 2;
  while (i + 9 < n) {
    if (p[i] != 0xFF) {
      ++i;
      continue;
    }
    const uint8_t m = p[i + 1];
    if (m == 0xFF) {  // fill byte
      ++i;
      continue;
    }
    if (m == 0x01 || (m >= 0xD0 && m <= 0xD8)) {  // markers without a length
      i += 2;
      continue;
    }
    const size_t len = static_cast<size_t>(p[i + 2]) << 8 | p[i + 3];
    if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
      h = p[i + 5] << 8 | p[i + 6];
      w = p[i + 7] << 8 | p[i + 8];
      return w > 0 && h > 0;
    }
    i += 2 + len;
  }
  return false;
}

// Width and height from a PNG's IHDR chunk.
bool pngSize(const uint8_t *p, size_t n, int &w, int &h) {
  if (n < 24 || memcmp(p + 12, "IHDR", 4) != 0) return false;
  w = static_cast<int>(p[16] << 24 | p[17] << 16 | p[18] << 8 | p[19]);
  h = static_cast<int>(p[20] << 24 | p[21] << 16 | p[22] << 8 | p[23]);
  return w > 0 && h > 0;
}

// A grey image in PSRAM.
struct Grey {
  int w = 0;
  int h = 0;
  std::unique_ptr<uint8_t, void (*)(void *)> px{nullptr, heap_caps_free};

  bool allocate(int width, int height) {
    w = width;
    h = height;
    px.reset(static_cast<uint8_t *>(
        heap_caps_malloc(static_cast<size_t>(w) * h, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
    if (px) memset(px.get(), 0xFF, static_cast<size_t>(w) * h);  // paper where nothing lands
    return px != nullptr;
  }
};

// --- JPEG ---------------------------------------------------------------------------------

int jpegDraw(JPEGDRAW *d) {
  Grey &g = *static_cast<Grey *>(d->pUser);
  const auto *src = reinterpret_cast<const uint8_t *>(d->pPixels);  // 8-bit grey, stride iWidth
  const int cols = std::min(d->iWidthUsed, g.w - d->x);
  for (int y = 0; y < d->iHeight && d->y + y < g.h; ++y) {
    if (cols > 0) memcpy(g.px.get() + (d->y + y) * g.w + d->x, src + y * d->iWidth, cols);
  }
  return 1;
}

bool decodeJpeg(const std::string &data, int targetW, int targetH, Grey &out) {
  // JPEGDEC holds ~20 KB of tables and buffers; internal RAM keeps its SIMD paths happy.
  void *mem = heap_caps_malloc(sizeof(JPEGDEC), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!mem) return false;
  std::unique_ptr<JPEGDEC, void (*)(JPEGDEC *)> jpeg(new (mem) JPEGDEC, [](JPEGDEC *j) {
    j->close();
    j->~JPEGDEC();
    heap_caps_free(j);
  });
  if (!jpeg->openRAM(const_cast<uint8_t *>(bytes(data)), static_cast<int>(data.size()),
                     jpegDraw)) {
    return false;
  }
  const int w = jpeg->getWidth(), h = jpeg->getHeight();
  // The decoder can shrink by 2, 4 or 8 for free: take the most that stays at or above the
  // target. Progressive files decode only their DC scan, which is 1/8 scale.
  int scale = 1, option = 0;
  if (jpeg->getJPEGType() == JPEG_MODE_PROGRESSIVE) {
    scale = 8;
    option = JPEG_SCALE_EIGHTH;
  } else {
    const int opts[] = {JPEG_SCALE_EIGHTH, JPEG_SCALE_QUARTER, JPEG_SCALE_HALF};
    for (const int s : {8, 4, 2}) {
      if (w / s >= targetW && h / s >= targetH) {
        scale = s;
        option = opts[s == 8 ? 0 : s == 4 ? 1 : 2];
        break;
      }
    }
  }
  if (!out.allocate((w + scale - 1) / scale, (h + scale - 1) / scale)) return false;
  jpeg->setPixelType(EIGHT_BIT_GRAYSCALE);
  jpeg->setUserPointer(&out);
  return jpeg->decode(0, 0, option) == 1;
}

// --- PNG ----------------------------------------------------------------------------------

struct PngJob {
  PNG *png;
  Grey *grey;
  uint16_t *line;
};

int pngDraw(PNGDRAW *d) {
  auto &job = *static_cast<PngJob *>(d->pUser);
  if (d->y >= job.grey->h) return 1;
  job.png->getLineAsRGB565(d, job.line, PNG_RGB565_LITTLE_ENDIAN, 0xFFFFFFFF);  // alpha on white
  uint8_t *row = job.grey->px.get() + d->y * job.grey->w;
  for (int x = 0; x < d->iWidth && x < job.grey->w; ++x) {
    const uint16_t v = job.line[x];
    const int r = (v >> 11) * 255 / 31, g = (v >> 5 & 63) * 255 / 63, b = (v & 31) * 255 / 31;
    row[x] = static_cast<uint8_t>((r * 77 + g * 150 + b * 29) >> 8);
  }
  return 1;
}

bool decodePng(const std::string &data, Grey &out) {
  void *mem = heap_caps_malloc(sizeof(PNG), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);  // ~60 KB
  if (!mem) return false;
  std::unique_ptr<PNG, void (*)(PNG *)> png(new (mem) PNG, [](PNG *p) {
    p->close();
    p->~PNG();
    heap_caps_free(p);
  });
  if (png->openRAM(const_cast<uint8_t *>(bytes(data)), static_cast<int>(data.size()), pngDraw) !=
      PNG_SUCCESS) {
    return false;
  }
  const int w = png->getWidth(), h = png->getHeight();
  if (static_cast<int64_t>(w) * h > kMaxPngPixels || !out.allocate(w, h)) return false;
  std::unique_ptr<uint16_t, void (*)(void *)> line(
      static_cast<uint16_t *>(heap_caps_malloc(w * sizeof(uint16_t), MALLOC_CAP_8BIT)), free);
  if (!line) return false;
  PngJob job{png.get(), &out, line.get()};
  return png->decode(&job, 0) == PNG_SUCCESS;
}

// --- Scale, tone, dither ------------------------------------------------------------------

// Area average when shrinking, nearest pixel when growing (the layout grows at most 2x).
// Source spans for each target column (or row): [edge[i], max(edge[i] + 1, edge[i + 1])).
// 32-bit maths; the sizes are at most a few thousand.
std::vector<int> spans(int from, int to) {
  std::vector<int> edge(to + 1);
  for (int i = 0; i <= to; ++i) edge[i] = i * from / to;
  return edge;
}

bool resample(const Grey &src, int w, int h, Grey &out, uint32_t (&hist)[256]) {
  if (!out.allocate(w, h)) return false;
  const std::vector<int> xs = spans(src.w, w), ys = spans(src.h, h);
  std::vector<uint32_t> sums(w);
  for (int ty = 0; ty < h; ++ty) {
    const int y0 = ys[ty], y1 = std::max(y0 + 1, ys[ty + 1]);
    std::fill(sums.begin(), sums.end(), 0);
    for (int y = y0; y < y1; ++y) {
      const uint8_t *row = src.px.get() + y * src.w;
      for (int tx = 0; tx < w; ++tx) {
        const int x0 = xs[tx], x1 = std::max(x0 + 1, xs[tx + 1]);
        uint32_t s = 0;
        for (int x = x0; x < x1; ++x) s += row[x];
        sums[tx] += s;
      }
    }
    uint8_t *dst = out.px.get() + ty * w;
    for (int tx = 0; tx < w; ++tx) {
      const int area = (y1 - y0) * (std::max(xs[tx] + 1, xs[tx + 1]) - xs[tx]);
      dst[tx] = static_cast<uint8_t>(sums[tx] / area);
      ++hist[dst[tx]];
    }
  }
  return true;
}

// The tone curve, from the histogram: the 1st-99th percentile stretched to full range, then
// the mid-tones lifted. Applied while dithering, so it costs no pass of its own.
void toneCurve(const uint32_t (&hist)[256], size_t n, uint8_t (&lut)[256]) {
  int lo = 0, hi = 255;
  for (uint32_t acc = 0; lo < 255 && (acc += hist[lo]) < n / 100;) ++lo;
  for (uint32_t acc = 0; hi > 0 && (acc += hist[hi]) < n / 100;) --hi;
  if (hi - lo < 32) {  // nearly flat: leave the levels alone
    lo = 0;
    hi = 255;
  }
  for (int v = 0; v < 256; ++v) {
    const float x = std::min(1.0f, std::max(0.0f, (v - lo) / static_cast<float>(hi - lo)));
    lut[v] = static_cast<uint8_t>(lroundf(255.0f * powf(x, kGamma)));
  }
}

void atkinson(const Grey &g, const uint8_t (&lut)[256], Bitmap &out) {
  const int w = g.w, h = g.h, stride = (w + 7) / 8;
  out.w = w;
  out.h = h;
  out.bits.assign(static_cast<size_t>(stride) * h, 0);
  // Error for this row and the next two, with 2 px of margin each side.
  std::vector<int16_t> err(3 * (w + 4), 0);
  int16_t *rows[3] = {&err[2], &err[w + 4 + 2], &err[2 * (w + 4) + 2]};
  for (int y = 0; y < h; ++y) {
    const uint8_t *src = g.px.get() + y * w;
    for (int x = 0; x < w; ++x) {
      const int old = std::min(255, std::max(0, lut[src[x]] + rows[0][x]));
      const bool ink = old < 128;
      const int e = (old - (ink ? 0 : 255)) / 8;
      rows[0][x + 1] += e;
      rows[0][x + 2] += e;
      rows[1][x - 1] += e;
      rows[1][x] += e;
      rows[1][x + 1] += e;
      rows[2][x] += e;
      if (ink) out.bits[y * stride + x / 8] |= 0x80 >> (x & 7);
    }
    int16_t *done = rows[0];
    rows[0] = rows[1];
    rows[1] = rows[2];
    rows[2] = done;
    std::fill(done - 2, done + w + 2, 0);
  }
}

}  // namespace

Format sniff(const std::string &d) {
  const uint8_t *p = bytes(d);
  if (d.size() >= 3 && p[0] == 0xFF && p[1] == 0xD8 && p[2] == 0xFF) return Format::Jpeg;
  if (d.size() >= 8 && memcmp(p, "\x89PNG\r\n\x1a\n", 8) == 0) return Format::Png;
  if (d.size() >= 6 && memcmp(p, "GIF8", 4) == 0) return Format::Gif;
  if (d.find("<svg") != std::string::npos) return Format::Svg;
  return Format::Unknown;
}

bool size(const std::string &data, int &w, int &h) {
  switch (sniff(data)) {
    case Format::Jpeg: return jpegSize(bytes(data), data.size(), w, h);
    case Format::Png: return pngSize(bytes(data), data.size(), w, h);
    default: return false;
  }
}

void fit(int w, int h, int boxW, int boxH, int &outW, int &outH) {
  if (w <= 0 || h <= 0) {
    outW = outH = 0;
    return;
  }
  const float scale = std::min(static_cast<float>(boxW) / w, static_cast<float>(boxH) / h);
  outW = std::max(1, static_cast<int>(lroundf(w * scale)));
  outH = std::max(1, static_cast<int>(lroundf(h * scale)));
}

void draw(GFXcanvas1 &canvas, int x0, int y0, const Bitmap &b) {
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
      int rx = px, ry = py;  // GFXcanvas1::drawPixel's rotation mapping
      switch (rot) {
        case 1: rx = rawW - 1 - py; ry = px; break;
        case 2: rx = rawW - 1 - px; ry = rawH - 1 - py; break;
        case 3: rx = py; ry = rawH - 1 - px; break;
        default: break;
      }
      uint8_t &byte = buf[(rx + ry * rawW) / 8];
      const uint8_t mask = 0x80 >> (rx & 7);
      if (row[x / 8] & (0x80 >> (x & 7))) {
        byte &= ~mask;  // ink: bit 0 on this panel
      } else {
        byte |= mask;
      }
    }
  }
}

bool render(const std::string &data, int w, int h, Bitmap &out) {
  if (w <= 0 || h <= 0) return false;
  const uint32_t t0 = millis();
  Grey decoded;
  const Format f = sniff(data);
  const bool ok = f == Format::Jpeg ? decodeJpeg(data, w, h, decoded)
                  : f == Format::Png ? decodePng(data, decoded)
                                     : false;
  if (!ok) return false;
  const uint32_t t1 = millis();
  Grey scaled;
  uint32_t hist[256] = {};
  if (!resample(decoded, w, h, scaled, hist)) return false;
  decoded.px.reset();
  uint8_t lut[256];
  toneCurve(hist, static_cast<size_t>(w) * h, lut);
  atkinson(scaled, lut, out);
  log_i("picture: %s %dx%d -> %dx%d, decode %lu ms, scale+dither %lu ms",
        f == Format::Jpeg ? "JPEG" : "PNG", decoded.w, decoded.h, w, h,
        static_cast<unsigned long>(t1 - t0), static_cast<unsigned long>(millis() - t1));
  return true;
}

}  // namespace picture
