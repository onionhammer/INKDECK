#include "apps/sensors.h"

#include <Arduino.h>
#include <math.h>
#include <stdio.h>

#include <algorithm>

#include "board_pins.h"
#include "pmic.h"
#include "sensor_hub.h"
#include "units.h"

namespace apps {
namespace {

using ui::Key;
using ui::Rect;
using ui::TextStyle;

// Sizes and weights from the Sensors artboard.
constexpr TextStyle kUnit{&ui::fonts::kMono18Bold, 0};           // °C, % RH
constexpr TextStyle kAxis{&ui::fonts::kMono15ExtraBold, 0};      // X Y Z
constexpr TextStyle kAxisValue{&ui::fonts::kMono15Bold, 0};      // +0.01
constexpr TextStyle kCellValue{&ui::fonts::kMono17ExtraBold, 0};  // PMIC cells
constexpr TextStyle kGrid{&ui::fonts::kMono13Medium, 0};         // "--"
constexpr TextStyle kGridBold{&ui::fonts::kMono13ExtraBold, 0};  // column heads, hits

enum Panel : uint8_t { kEnv, kImu, kPmic, kBus, kPanelCount };

// How often a panel re-reads: live when focused, otherwise once a minute.
constexpr uint32_t kLiveMs[kPanelCount] = {5000, 1000, 5000, 5000};
constexpr uint32_t kIdleMs = 60000;

// Panel heights (each includes its top rule except the first, which sits under the status bar).
constexpr int kEnvH = 174;
constexpr int kImuH = ui::kRule + ui::kSectionH + 154;
constexpr int kPmicH = ui::kRule + ui::kSectionH + 56;
constexpr int kBubble = 118;

// IMU changes smaller than these don't redraw (sensor noise would refresh it every second).
constexpr float kAccDeadband = 0.02f;   // g
constexpr float kGyroDeadband = 1.5f;   // °/s
constexpr float kTiltDeadband = 0.5f;   // degrees
constexpr float kBubbleFullScale = 10;  // degrees of tilt that put the bubble at the rim

std::string fixed(float v, int decimals) {
  char buf[16];
  snprintf(buf, sizeof buf, "%.*f", decimals, v);
  return buf;
}

// A temperature in the display unit (units.h), one decimal.
std::string temp1(float celsius) { return fixed(units::temperature(celsius), 1); }

std::string hex2(int v) {
  char buf[4];
  snprintf(buf, sizeof buf, "%02x", v);
  return buf;
}

std::string address(uint8_t a) {
  char buf[8];
  snprintf(buf, sizeof buf, "@0x%02X", a);
  return buf;
}

std::string chargeState(const pmic::Detail &d) {
  if (!d.ok) return "?";
  if (!d.status.battery) return "NO BAT";
  switch (d.flow) {
    case pmic::Flow::Charging:
      return d.charger == pmic::Charger::ConstantVoltage   ? "CHG CV"
             : d.charger == pmic::Charger::ConstantCurrent ? "CHG CC"
                                                           : "PRECHG";
    case pmic::Flow::Discharging: return "DISCHG";
    case pmic::Flow::Standby: break;
  }
  return d.charger == pmic::Charger::Done ? "FULL" : "IDLE";
}

class SensorsScreen : public ui::Screen {
 public:
  std::string tag() const override { return "05 SENSORS"; }
  std::string context() const override {
    // "I2C · 5/5 ACK" as on the canvas; flags replace the "I2C ·" so they fit.
    std::string flags;
    if (held_) flags += " \xC2\xB7 HELD";
    if (logFailed_) {
      flags += " \xC2\xB7 NO SD";
    } else if (sensors::logging()) {
      flags += " \xC2\xB7 LOG";
    }
    const std::string ack = std::to_string(knownInScan()) + "/" +
                            std::to_string(sensors::kKnownCount) + " ACK";
    return flags.empty() ? "I2C \xC2\xB7 " + ack : ack + flags;
  }
  int hints(ui::Hint *out) const override {
    out[0] = {ui::glyph::kDial, "PANEL"};
    out[1] = {ui::glyph::kPress, held_ ? "RESUME" : "HOLD"};
    out[2] = {ui::glyph::kHoldPress, sensors::logging() ? "LOG OFF" : "LOG"};
    out[3] = {"BOOT", "BACK"};
    return 4;
  }

  void onEnter() override {
    for (int p = 0; p < kPanelCount; ++p) refresh(static_cast<Panel>(p), true);
  }

  bool onKey(Key key) override {
    switch (key) {
      case Key::Up:
      case Key::Down:
        focus_ = static_cast<Panel>((focus_ + (key == Key::Up ? kPanelCount - 1 : 1)) %
                                    kPanelCount);
        refresh(focus_, true);
        return true;
      case Key::Select:
        held_ = !held_;
        if (!held_) {
          for (int p = 0; p < kPanelCount; ++p) refresh(static_cast<Panel>(p), true);
        }
        return true;
      case Key::SelectLong:
        logFailed_ = !sensors::setLogging(!sensors::logging());
        return true;
      default: return false;
    }
  }

  bool onTick() override {
    if (held_) return false;
    bool changed = false;
    const uint32_t now = millis();
    for (int i = 0; i < kPanelCount; ++i) {
      const Panel p = static_cast<Panel>(i);
      const uint32_t every = p == focus_ ? kLiveMs[p] : kIdleMs;
      if (now - refreshedAt_[p] >= every) changed |= refresh(p, false);
    }
    return changed;
  }

  void draw(ui::Canvas &c, const Rect &body) override {
    Rect r = body;
    drawEnv(c, r.takeTop(kEnvH));
    drawImu(c, r.takeTop(kImuH));
    drawPmic(c, r.takeTop(kPmicH));
    drawBus(c, r);
  }

 private:
  // Re-reads a panel's data. Returns true if what it shows changed (always stores on force).
  bool refresh(Panel p, bool force) {
    refreshedAt_[p] = millis();
    switch (p) {
      case kEnv: {
        const shtc3::Reading e = sensors::env();
        const std::vector<float> h = sensors::history();
        const bool changed = force || e.ok != env_.ok || temp1(e.tempC) != temp1(env_.tempC) ||
                             lroundf(e.rh) != lroundf(env_.rh) || h.size() != history_.size() ||
                             (!h.empty() && temp1(h.back()) != temp1(history_.back()));
        if (changed) {
          env_ = e;
          history_ = h;
        }
        return changed;
      }
      case kImu: {
        const qmi8658::Reading m = sensors::motion();
        float pitch = 0, roll = 0;
        sensors::tilt(m, pitch, roll);
        bool changed = force || m.ok != imu_.ok || fabsf(pitch - pitch_) > kTiltDeadband ||
                       fabsf(roll - roll_) > kTiltDeadband;
        for (int i = 0; i < 3 && !changed; ++i) {
          changed = fabsf(m.acc[i] - imu_.acc[i]) > kAccDeadband ||
                    fabsf(m.gyro[i] - imu_.gyro[i]) > kGyroDeadband;
        }
        if (changed) {
          imu_ = m;
          pitch_ = pitch;
          roll_ = roll;
        }
        return changed;
      }
      case kPmic: {
        const pmic::Detail d = pmic::detail();
        const bool changed = force || d.ok != pmic_.ok ||
                             d.status.percent != pmic_.status.percent ||
                             abs(d.vbatMv - pmic_.vbatMv) >= 20 ||
                             d.status.vbus != pmic_.status.vbus ||
                             abs(d.vbusMv - pmic_.vbusMv) >= 50 ||
                             chargeState(d) != chargeState(pmic_);
        if (changed) pmic_ = d;
        return changed;
      }
      case kBus: {
        const std::vector<uint8_t> s = sensors::scanBus();
        const bool changed = force || s != scan_;
        scan_ = s;
        return changed;
      }
      case kPanelCount: break;
    }
    return false;
  }

  int knownInScan() const {
    int n = 0;
    for (const uint8_t a : scan_) n += sensors::deviceName(a) != nullptr;
    return n;
  }

  ui::SectionOptions headerOptions(Panel p) const { return {p == focus_, true}; }

  void drawEnv(ui::Canvas &c, Rect r) const {
    using namespace ui;
    drawSectionHeader(c, r.takeTop(kSectionH), "ENV \xC2\xB7 SHTC3",
                      address(board::I2C_ADDR_SHTC3), headerOptions(kEnv));
    const Rect p = r.inset(kPadX, 12);
    constexpr int kBigH = 51;  // the canvas's 64 px at line-height 0.8
    const int base = p.y + capHeight(style::kReadout) + (kBigH - capHeight(style::kReadout)) / 2;
    const int colW = (p.w - 16) / 2;

    const std::string t = env_.ok ? temp1(env_.tempC) : "--.-";
    int x = drawText(c, p.x, base, t, style::kReadout);
    drawText(c, x + 6, base, units::temperatureUnit(), kUnit);
    const int col2 = p.x + colW + 16;
    vrule(c, col2, p.y, kBigH);
    const std::string h = env_.ok ? std::to_string(lroundf(env_.rh)) : "--";
    x = drawText(c, col2 + kRule + 16, base, h, style::kReadout);
    drawText(c, x + 6, base, "% RH", kUnit);

    const int labelTop = p.y + kBigH + 8 + 4;
    const Rect labels{p.x, labelTop, p.w, 17};
    drawText(c, labels, "TEMP \xC2\xB7 24 H", style::kLabel);
    std::string range = std::string("MIN ") + glyph::kEmDash + " \xC2\xB7 MAX " + glyph::kEmDash;
    if (!history_.empty()) {
      const auto [mn, mx] = std::minmax_element(history_.begin(), history_.end());
      range = "MIN " + temp1(*mn) + " \xC2\xB7 MAX " + temp1(*mx);
    }
    drawText(c, labels, range, style::kLabel, Align::Right);
    drawSparkline(c, {p.x, labels.bottom() + 8, p.w, 32});
  }

  // 48 half-hour bars, newest on the right; the bottom rule is the axis.
  void drawSparkline(ui::Canvas &c, const Rect &r) const {
    using namespace ui;
    fill(c, {r.x, r.bottom() - 2, r.w, 2}, kInk);
    if (history_.empty()) return;
    auto [mnIt, mxIt] = std::minmax_element(history_.begin(), history_.end());
    float mn = *mnIt, mx = *mxIt;
    if (mx - mn < 1.0f) {  // don't blow sensor noise up to full height
      const float mid = (mn + mx) / 2;
      mn = mid - 0.5f;
      mx = mid + 0.5f;
    }
    constexpr int kSlots = sensors::kHistorySlots;
    constexpr int kGap = 2;
    const int first = kSlots - static_cast<int>(history_.size());
    for (int i = std::max(first, 0); i < kSlots; ++i) {
      const float v = history_[i - first];
      const int x0 = r.x + i * (r.w + kGap) / kSlots;
      const int x1 = r.x + (i + 1) * (r.w + kGap) / kSlots - kGap;
      const int h = std::max(2, static_cast<int>(lroundf(4 + (v - mn) / (mx - mn) * 26)));
      fill(c, {x0, r.bottom() - 2 - h, x1 - x0, h}, kInk);
    }
  }

  void drawImu(ui::Canvas &c, Rect r) const {
    using namespace ui;
    hrule(c, r.x, r.y, r.w);
    r.takeTop(kRule);
    drawSectionHeader(c, r.takeTop(kSectionH), "IMU \xC2\xB7 QMI8658A",
                      address(board::I2C_ADDR_QMI8658), headerOptions(kImu));
    const Rect p = r.inset(kPadX, 12);
    drawBubble(c, p.x, p.y + (p.h - kBubble) / 2);

    // Accel/gyro table and the tilt line.
    const int x = p.x + kBubble + 16;
    const int w = p.right() - x;
    const int colW = (w - 36) / 2;
    int y = p.y;
    drawText(c, {x + 36, y, colW, 24}, "ACC g", style::kKey);
    drawText(c, {x + 36 + colW, y, colW, 24}, "GYR \xC2\xB0/s", style::kKey);
    fill(c, {x, y + 22, w, 2}, kInk);
    y += 24;
    static const char *const kAxes[3] = {"X", "Y", "Z"};
    for (int i = 0; i < 3; ++i, y += 26) {
      drawText(c, {x, y, 36, 26}, kAxes[i], kAxis);
      drawText(c, {x + 36, y, colW, 26}, imu_.ok ? signedNumber(imu_.acc[i], 2) : "--", kAxisValue);
      drawText(c, {x + 36 + colW, y, colW, 26}, imu_.ok ? signedNumber(imu_.gyro[i], 1) : "--",
               kAxisValue);
      dashedH(c, x, y + 25, w);
    }
    const int tagW = drawTag(c, x, y + 14, 19, "TILT", style::kPill, TagKind::Inverted, 6);
    const std::string tilt = imu_.ok ? "P " + signedNumber(pitch_, 1) + "\xC2\xB0 \xC2\xB7 R " +
                                           signedNumber(roll_, 1) + "\xC2\xB0"
                                     : std::string(glyph::kEmDash);
    drawText(c, {x + tagW + 8, y, w - tagW - 8, 28}, tilt, style::kLabel);
  }

  // Bubble level: 3 px rim, dashed 2-degree ring, dashed crosshair, and the bubble, which
  // rises to the high side like a real one.
  void drawBubble(ui::Canvas &c, int left, int top) const {
    using namespace ui;
    const int cx = left + kBubble / 2, cy = top + kBubble / 2;
    c.fillCircle(cx, cy, 56, kInk);
    c.fillCircle(cx, cy, 53, kPaper);
    for (int i = 0; i < 360 * 4; ++i) {  // r = 18, 2 px, dashes 3 px on / 3 px off
      const float a = i * static_cast<float>(M_PI) / 720;
      if (static_cast<int>(a * 18 / 3) % 2) continue;
      for (const float rr : {17.0f, 18.0f}) {
        c.drawPixel(cx + lroundf(rr * cosf(a)), cy + lroundf(rr * sinf(a)), kInk);
      }
    }
    for (int d = 6; d < 112; d += 6) {  // crosshair, 2 on / 4 off
      c.drawFastVLine(cx, top + d, 2, kInk);
      c.drawFastHLine(left + d, cy, 2, kInk);
    }
    if (!imu_.ok) return;
    constexpr float kTravel = 53 - 11 - 2;  // rim inside minus bubble radius
    float dx = roll_ / kBubbleFullScale * kTravel;
    float dy = -pitch_ / kBubbleFullScale * kTravel;
    const float len = sqrtf(dx * dx + dy * dy);
    if (len > kTravel) {
      dx *= kTravel / len;
      dy *= kTravel / len;
    }
    c.fillCircle(cx + lroundf(dx), cy + lroundf(dy), 11, kInk);
  }

  void drawPmic(ui::Canvas &c, Rect r) const {
    using namespace ui;
    hrule(c, r.x, r.y, r.w);
    r.takeTop(kRule);
    drawSectionHeader(c, r.takeTop(kSectionH), "PMIC \xC2\xB7 TG28",
                      address(board::I2C_ADDR_PMIC), headerOptions(kPmic));
    const pmic::Detail &d = pmic_;
    char vbat[16] = "--", vbus[16] = "OFF";
    if (d.vbatMv > 0) snprintf(vbat, sizeof vbat, "%.2f V", d.vbatMv / 1000.0);
    if (d.status.vbus && d.vbusMv > 0) snprintf(vbus, sizeof vbus, "%.2f V", d.vbusMv / 1000.0);
    // The canvas's IBAT and DIE cells: the TG28 has no battery-current ADC, and its die
    // temperature isn't enabled, so USB voltage and the charger state take their places.
    const std::pair<const char *, std::string> cells[4] = {
        {"BAT", d.status.percent >= 0 ? std::to_string(d.status.percent) + " %" : "--"},
        {"VBAT", vbat},
        {"USB", vbus},
        {"STATE", chargeState(d)},
    };
    const int cellW = r.w / 4;
    for (int i = 0; i < 4; ++i) {
      const int x = r.x + i * cellW;
      drawText(c, x + kPadX, r.y + 8 + capHeight(style::kKey), cells[i].first, style::kKey);
      drawText(c, {x + kPadX, r.y + 26, cellW - 2 * kPadX + 6, 24}, cells[i].second, kCellValue);
      if (i < 3) dashedV(c, x + cellW - 1, r.y, r.h);
    }
  }

  void drawBus(ui::Canvas &c, Rect r) const {
    using namespace ui;
    hrule(c, r.x, r.y, r.w);
    r.takeTop(kRule);
    drawSectionHeader(c, r.takeTop(kSectionH), "$ i2cdetect -y 0",
                      std::to_string(scan_.size()) + " FOUND", {focus_ == kBus, false});
    const Rect p{r.x + kPadX, r.y + 8, r.w - 2 * kPadX, r.h - 8};
    constexpr int kLabelW = 30;
    const float cellW = (p.w - kLabelW) / 16.0f;
    auto cellRect = [&](int col, int y, int h) {
      const int x0 = p.x + kLabelW + lroundf(col * cellW);
      const int x1 = p.x + kLabelW + lroundf((col + 1) * cellW);
      return Rect{x0, y, x1 - x0, h};
    };
    static const char kHex[] = "0123456789abcdef";
    for (int col = 0; col < 16; ++col) {
      drawText(c, cellRect(col, p.y, 18), std::string(1, kHex[col]), kGridBold, Align::Center);
    }
    for (int row = 0; row < 8; ++row) {
      const int y = p.y + 18 + row * 17;
      drawText(c, {p.x, y, kLabelW, 17}, std::string(1, kHex[row]) + "0:", kGridBold);
      for (int col = 0; col < 16; ++col) {
        const int a = row * 16 + col;
        if (a < 0x03 || a > 0x77) continue;
        const Rect cell = cellRect(col, y, 17);
        if (std::find(scan_.begin(), scan_.end(), a) != scan_.end()) {
          fill(c, cell, kInk);
          drawText(c, cell, hex2(a), kGridBold, Align::Center, kPaper);
        } else {
          drawText(c, cell, "--", kGrid, Align::Center);
        }
      }
    }

    // What answered, by name, at the bottom of the panel.
    std::string names;
    for (const uint8_t a : scan_) {
      const char *name = sensors::deviceName(a);
      if (!names.empty()) names += " \xC2\xB7 ";
      names += ui::upper(hex2(a)) + " " + (name ? name : "?");
    }
    if (names.empty()) names = "NOTHING ANSWERED";
    const std::vector<std::string> lines = wrap(style::kLabel, names, p.w, 3);
    int y = p.bottom() - 10 - static_cast<int>(lines.size()) * 20;
    for (const auto &line : lines) {
      y += 20;
      drawText(c, p.x, y - 5, line, style::kLabel);
    }
  }

  Panel focus_ = kEnv;
  bool held_ = false;
  bool logFailed_ = false;
  uint32_t refreshedAt_[kPanelCount] = {};
  shtc3::Reading env_;
  std::vector<float> history_;
  qmi8658::Reading imu_;
  float pitch_ = 0, roll_ = 0;
  pmic::Detail pmic_;
  std::vector<uint8_t> scan_;
};

}  // namespace

std::unique_ptr<ui::Screen> makeSensors() { return std::make_unique<SensorsScreen>(); }

}  // namespace apps
