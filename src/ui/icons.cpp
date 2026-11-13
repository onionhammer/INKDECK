#include "ui/icons.h"

#include <math.h>
#include <stdlib.h>

#include <algorithm>
#include <initializer_list>

namespace ui::icons {
namespace {

struct Pt {
  float x, y;
};

// Draws in grid units: 48 units across the icon's square, optionally through a sub-box (a small
// moon behind a cloud).
class Pen {
 public:
  Pen(Canvas &c, int x, int y, int size, float strokeUnits)
      : c_(c), x_(x), y_(y), scale_(size / 48.0f) {
    width_ = std::max(2, static_cast<int>(lroundf(strokeUnits * scale_)));
  }

  void subBox(float ox, float oy, float k) {
    ox_ = ox;
    oy_ = oy;
    k_ = k;
  }

  int px(float u) const { return x_ + static_cast<int>(lroundf((ox_ + u * k_) * scale_)); }
  int py(float u) const { return y_ + static_cast<int>(lroundf((oy_ + u * k_) * scale_)); }

  // A stroke with square caps, centred on the path.
  void line(float x0, float y0, float x1, float y1, float units = 0) {
    const int w = units > 0 ? std::max(2, static_cast<int>(lroundf(units * scale_))) : width_;
    const int half = w / 2;
    const int ax = px(x0), ay = py(y0), bx = px(x1), by = py(y1);
    if (ax == bx || ay == by) {
      const int l = std::min(ax, bx) - half, t = std::min(ay, by) - half;
      const int r = std::max(ax, bx) - half + w, b = std::max(ay, by) - half + w;
      c_.fillRect(l, t, r - l, b - t, kInk);
      return;
    }
    const int steps = std::max(abs(bx - ax), abs(by - ay));
    for (int i = 0; i <= steps; ++i) {
      const int cx = ax + static_cast<int>(lroundf(static_cast<float>(bx - ax) * i / steps));
      const int cy = ay + static_cast<int>(lroundf(static_cast<float>(by - ay) * i / steps));
      c_.fillRect(cx - half, cy - half, w, w, kInk);
    }
  }

  void polyline(std::initializer_list<Pt> pts) {
    const Pt *p = pts.begin();
    for (size_t i = 1; i < pts.size(); ++i) line(p[i - 1].x, p[i - 1].y, p[i].x, p[i].y);
  }

  // A closed outline; paper inside first if `fill`, so it hides what's behind it.
  void polygon(std::initializer_list<Pt> pts, bool fill = false) {
    const Pt *p = pts.begin();
    const int n = static_cast<int>(pts.size());
    if (fill) fillPaper(p, n);
    for (int i = 0; i < n; ++i) {
      const Pt &a = p[i], &b = p[(i + 1) % n];
      line(a.x, a.y, b.x, b.y);
    }
  }

  void rect(float x, float y, float w, float h) {
    polygon({{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}});
  }

  void block(float cx, float cy, float units) {  // a filled square centred on (cx, cy)
    const int l = px(cx - units / 2), t = py(cy - units / 2);
    const int r = px(cx + units / 2), b = py(cy + units / 2);
    c_.fillRect(l, t, std::max(2, r - l), std::max(2, b - t), kInk);
  }

 private:
  // Even-odd scanline fill of the polygon's inside, sampled at pixel centres.
  void fillPaper(const Pt *p, int n) {
    int top = py(p[0].y), bottom = top;
    for (int i = 1; i < n; ++i) {
      top = std::min(top, py(p[i].y));
      bottom = std::max(bottom, py(p[i].y));
    }
    float xs[16];
    for (int y = top; y < bottom; ++y) {
      const float sy = y + 0.5f;
      int count = 0;
      for (int i = 0; i < n && count < 16; ++i) {
        const float ax = px(p[i].x), ay = py(p[i].y);
        const float bx = px(p[(i + 1) % n].x), by = py(p[(i + 1) % n].y);
        if ((ay <= sy && sy < by) || (by <= sy && sy < ay)) {
          xs[count++] = ax + (sy - ay) * (bx - ax) / (by - ay);
        }
      }
      std::sort(xs, xs + count);
      for (int i = 0; i + 1 < count; i += 2) {
        const int l = static_cast<int>(ceilf(xs[i] - 0.5f));
        const int r = static_cast<int>(floorf(xs[i + 1] - 0.5f));
        if (r >= l) c_.drawFastHLine(l, y, r - l + 1, kPaper);
      }
    }
  }

  Canvas &c_;
  int x_, y_;
  float scale_;
  int width_;
  float ox_ = 0, oy_ = 0, k_ = 1;
};

// The canvas's clouds: low (overcast), high (with rain under it), and the one in front of the
// sun in "partly cloudy".
void cloudLow(Pen &p) {
  p.polygon({{6, 38}, {42, 38}, {42, 30}, {38, 30}, {38, 24}, {32, 24}, {32, 18}, {20, 18},
             {20, 24}, {14, 24}, {14, 30}, {6, 30}});
}

void cloudHigh(Pen &p) {
  p.polygon({{6, 30}, {42, 30}, {42, 22}, {38, 22}, {38, 16}, {32, 16}, {32, 10}, {20, 10},
             {20, 16}, {14, 16}, {14, 22}, {6, 22}});
}

void cloudFront(Pen &p) {
  p.polygon({{10, 42}, {44, 42}, {44, 34}, {40, 34}, {40, 28}, {34, 28}, {34, 22}, {22, 22},
             {22, 28}, {16, 28}, {16, 34}, {10, 34}},
            true);
}

// A stepped crescent opening to the right, its horns tapering.
void moon(Pen &p) {
  p.polygon({{37, 7}, {21, 7}, {21, 10}, {15, 10}, {15, 15}, {11, 15}, {11, 33}, {15, 33},
             {15, 38}, {21, 38}, {21, 41}, {37, 41}, {37, 37}, {31, 37}, {31, 33}, {26, 33},
             {26, 15}, {31, 15}, {31, 11}, {37, 11}});
}

}  // namespace

void drawWeather(Canvas &c, int x, int y, int size, Weather icon, float strokeUnits) {
  Pen p(c, x, y, size, strokeUnits);
  switch (icon) {
    case Weather::Sun:
      p.rect(16, 16, 16, 16);
      p.line(24, 3, 24, 9);
      p.line(24, 39, 24, 45);
      p.line(3, 24, 9, 24);
      p.line(39, 24, 45, 24);
      p.line(9, 9, 12, 12);
      p.line(39, 9, 36, 12);
      p.line(9, 39, 12, 36);
      p.line(39, 39, 36, 36);
      break;
    case Weather::Moon:
      moon(p);
      break;
    case Weather::Partly:
      p.rect(8, 8, 12, 12);
      p.line(14, 2, 14, 4);
      p.line(2, 14, 4, 14);
      p.line(25, 14, 27, 14);
      cloudFront(p);
      break;
    case Weather::PartlyNight:
      p.subBox(0, 0, 0.6f);
      moon(p);
      p.subBox(0, 0, 1);
      cloudFront(p);
      break;
    case Weather::Cloud:
      cloudLow(p);
      if (size >= 64) {  // the wisps above the big one
        p.line(16, 12, 26, 12, 2);
        p.line(30, 12, 34, 12, 2);
      }
      break;
    case Weather::Fog:
      p.polygon({{8, 24}, {40, 24}, {40, 18}, {34, 18}, {34, 12}, {28, 12}, {28, 6}, {18, 6},
                 {18, 12}, {12, 12}, {12, 18}, {8, 18}});
      p.line(4, 32, 44, 32);
      p.line(10, 40, 38, 40);
      break;
    case Weather::Showers:
      cloudHigh(p);
      p.line(18, 36, 18, 42);
      p.line(30, 38, 30, 44);
      break;
    case Weather::Rain:
      cloudHigh(p);
      p.line(14, 36, 14, 44);
      p.line(24, 36, 24, 44);
      p.line(34, 36, 34, 44);
      break;
    case Weather::Snow:
      cloudHigh(p);
      p.block(14, 38, 5);
      p.block(24, 43, 5);
      p.block(34, 38, 5);
      break;
    case Weather::Storm:
      cloudHigh(p);
      p.polyline({{26, 33}, {20, 40}, {28, 40}, {22, 47}});
      break;
  }
}

}  // namespace ui::icons
