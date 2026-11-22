#include "apps/sensors.h"

#include <Arduino.h>
#include <math.h>
#include <stdio.h>

#include <algorithm>

#include "apps/home.h"
#include "apps/level.h"
#include "apps/radio_ui.h"
#include "apps/wifi_networks.h"
#include "board_pins.h"
#include "features.h"
#include "pmic.h"
#include "sensor_hub.h"
#include "ui/menu.h"
#include "units.h"
#include "wifi_survey.h"
#if INKDECK_BLE
#include "apps/ble_devices.h"
#include "ble_adv.h"
#include "ble_survey.h"
#endif

namespace apps {
namespace {

using ui::Align;
using ui::Key;
using ui::Rect;
using ui::TextStyle;

// Sizes and weights from the Sensors artboard.
constexpr TextStyle kUnit{&ui::fonts::kMono18Bold, 0};           // °C, % RH
constexpr TextStyle kAxis{&ui::fonts::kMono15ExtraBold, 0};      // X Y Z
constexpr TextStyle kAxisValue{&ui::fonts::kMono15Bold, 0};      // +0.01; scan panel rows
constexpr TextStyle kAddress{&ui::fonts::kMono15Medium, 0};      // a BLE device without a name
constexpr TextStyle kGrid{&ui::fonts::kMono13Medium, 0};         // "--"
constexpr TextStyle kGridBold{&ui::fonts::kMono13ExtraBold, 0};  // column heads, hits

// The dial's stops. The last three share the bottom panel, which shows the last of them focused.
// A build without INKDECK_BLE (features.h) stops short of kBle.
enum Panel : uint8_t { kEnv, kImu, kPmic, kI2c, kWifi, kBle, kPanelCount };
constexpr int kStops = INKDECK_BLE ? kPanelCount : kBle;

// How often a panel re-reads: live when focused, otherwise once a minute (the radios not at all:
// they scan only while focused).
constexpr uint32_t kLiveMs[kPanelCount] = {5000, 1000, 5000, 5000, 1000, 5000};
constexpr uint32_t kIdleMs = 60000;

// Panel heights (each includes its top rule except the first, which sits under the status bar).
constexpr int kEnvH = 174;
constexpr int kImuH = ui::kRule + ui::kSectionH + 154;
constexpr int kPmicH = ui::kRule + ui::kSectionH + 56;
constexpr int kBubble = 118;
constexpr int kScanRows = 7;  // networks or devices the scan panel lists

// IMU changes smaller than these don't redraw (sensor noise would refresh it every second).
constexpr float kAccDeadband = 0.02f;   // g
constexpr float kGyroDeadband = 1.5f;   // °/s
constexpr float kTiltDeadband = 0.5f;   // degrees
constexpr float kBubbleFullScale = 10;  // degrees of tilt that put the bubble at the rim

// The menu's counts: a Wi-Fi scan older than this isn't worth quoting.
constexpr uint32_t kWifiCountMs = 5 * 60 * 1000;
// A radio starts once the dial has rested on its stop this long, so rolling past BLE on the way
// to WI-FI doesn't start the Bluetooth stack.
constexpr uint32_t kSettleMs = 800;

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

std::string seen(int n) { return n > 0 ? std::to_string(n) + " SEEN" : ""; }

enum class Radio : uint8_t { None, Wifi, Ble };

class SensorsScreen : public ui::Screen {
 public:
  ~SensorsScreen() override { setRadio(Radio::None); }

  std::string tag() const override { return moduleTag("SENSORS"); }
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
    out[1] = {ui::glyph::kPress, "MENU"};
    out[2] = {ui::glyph::kHoldPress, sensors::logging() ? "LOG OFF" : "LOG"};
    out[3] = {"BOOT", "BACK"};
    return 4;
  }

  void onEnter() override {
    onTop_ = true;
    syncRadio();
    for (int p = 0; p < kPanelCount; ++p) refresh(static_cast<Panel>(p), true);
  }
  void onLeave() override {
    onTop_ = false;
    syncRadio();
  }

  bool onKey(Key key) override {
    switch (key) {
      case Key::Up:
      case Key::Down:
        focus_ = static_cast<Panel>((focus_ + (key == Key::Up ? kStops - 1 : 1)) % kStops);
        if (focus_ >= kI2c) scan_ = focus_;
        focusedAt_ = millis();
        // Off at once when the focus leaves a radio; on from onTick() once it has settled.
        if (wantedRadio() != radio_) setRadio(Radio::None);
        refresh(focus_, true);
        return true;
      case Key::Select: openMenu(); return true;
      case Key::SelectLong: toggleLog(); return true;
      default: return false;
    }
  }

  bool onTick() override {
    if (held_) return false;
    bool changed = false;
    const uint32_t now = millis();
    if (wantedRadio() != radio_ && now - focusedAt_ >= kSettleMs) syncRadio();
    for (int i = 0; i < kPanelCount; ++i) {
      const Panel p = static_cast<Panel>(i);
      const bool radio = p == kWifi || p == kBle;
      if (radio && p != focus_) continue;
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
    drawScan(c, r);
  }

 private:
  // The survey this screen keeps running: the focused radio's, while the screen is on top and
  // the readings aren't held.
  Radio wantedRadio() const {
    if (!onTop_ || held_) return Radio::None;
    if (focus_ == kWifi) return Radio::Wifi;
    if (focus_ == kBle) return Radio::Ble;
    return Radio::None;
  }
  void syncRadio() { setRadio(wantedRadio()); }

  void setRadio(Radio want) {
    if (want == radio_) return;
    if (radio_ == Radio::Wifi) wifisurvey::end();
#if INKDECK_BLE
    if (radio_ == Radio::Ble) blesurvey::end();
#endif
    radio_ = want;
    if (radio_ == Radio::Wifi) wifisurvey::begin();
#if INKDECK_BLE
    if (radio_ == Radio::Ble) blesurvey::begin();
#endif
  }

  void toggleHold() {
    held_ = !held_;
    syncRadio();
    if (!held_) {
      for (int p = 0; p < kPanelCount; ++p) refresh(static_cast<Panel>(p), true);
    }
  }

  void toggleLog() { logFailed_ = !sensors::setLogging(!sensors::logging()); }

  // ● : the // SENSORS menu sheet. It opens on the focused radio's row, else on the item last run.
  enum class MenuRow : uint8_t { Wifi, Ble, Level, Hold, Log };
  void openMenu() {
    std::vector<ui::MenuItem> items;
    std::vector<MenuRow> rows;
    items.push_back({"WI-FI NETWORKS",
                     [] {
                       const uint32_t at = wifisurvey::lastScanMs();
                       if (at == 0 || millis() - at > kWifiCountMs) return std::string();
                       return seen(static_cast<int>(wifisurvey::networks().size()));
                     },
                     [this] {
                       menuLast_ = MenuRow::Wifi;
                       ui::nav::replace(makeWifiNetworks());
                     }});
    rows.push_back(MenuRow::Wifi);
#if INKDECK_BLE
    items.push_back({"BLUETOOTH LE", [] { return seen(blesurvey::count()); },
                     [this] {
                       menuLast_ = MenuRow::Ble;
                       ui::nav::replace(makeBleDevices());
                     }});
    rows.push_back(MenuRow::Ble);
#endif
    items.push_back({"SPIRIT LEVEL",
                     [] {
                       // Read directly: the screen below refreshes its own tilt only while it has the focus.
                       const qmi8658::Reading m = sensors::motion();
                       if (!m.ok) return std::string(ui::glyph::kEmDash);
                       float pitch = 0, roll = 0;
                       sensors::tilt(m, pitch, roll);
                       return "P " + ui::signedNumber(pitch, 1) + "\xC2\xB0 \xC2\xB7 R " +
                              ui::signedNumber(roll, 1) + "\xC2\xB0";
                     },
                     [this] {
                       menuLast_ = MenuRow::Level;
                       ui::nav::replace(makeLevel());
                     }});
    rows.push_back(MenuRow::Level);
    items.push_back({"HOLD READINGS", [this] { return std::string(held_ ? "ON" : "OFF"); },
                     [this] {
                       menuLast_ = MenuRow::Hold;
                       toggleHold();
                     }});
    rows.push_back(MenuRow::Hold);
    items.push_back({"LOG TO SD",
                     [this] {
                       return std::string(logFailed_ ? "NO SD" : sensors::logging() ? "ON" : "OFF");
                     },
                     [this] {
                       menuLast_ = MenuRow::Log;
                       toggleLog();
                     }});
    rows.push_back(MenuRow::Log);
    const MenuRow want = focus_ == kWifi ? MenuRow::Wifi : focus_ == kBle ? MenuRow::Ble : menuLast_;
    const int cursor = static_cast<int>(std::find(rows.begin(), rows.end(), want) - rows.begin());
    ui::nav::push(std::make_unique<ui::MenuScreen>("SENSORS", std::move(items),
                                                   cursor < static_cast<int>(rows.size()) ? cursor : 0));
  }

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
      case kI2c: {
        const std::vector<uint8_t> s = sensors::scanBus();
        const bool changed = force || s != scan_i2c_;
        scan_i2c_ = s;
        return changed;
      }
      case kWifi: {
        const uint32_t g = wifisurvey::generation();
        std::vector<wifisurvey::Network> nets = wifisurvey::networks();
        const std::string state = wifiState(nets);
        const bool changed = force || g != wifiGen_ || state != wifiStateShown_;
        if (changed) {
          wifiGen_ = g;
          wifiStateShown_ = state;
          nets_ = std::move(nets);
        }
        return changed;
      }
      case kBle: {
#if INKDECK_BLE
        std::vector<blesurvey::Device> d = blesurvey::devices();
        const std::string sig = bleSignature(d) + bleState();
        const bool changed = force || sig != bleShown_;
        if (changed) {
          bleShown_ = sig;
          devs_ = std::move(d);
        }
        return changed;
#else
        return false;
#endif
      }
      case kPanelCount: break;
    }
    return false;
  }

  static std::string wifiState(const std::vector<wifisurvey::Network> &nets) {
    if (!wifisurvey::error().empty() && nets.empty()) return wifisurvey::error();
    return wifisurvey::lastScanMs() == 0 ? "SCANNING" : "";
  }

#if INKDECK_BLE
  std::string bleState() const {
    if (!blesurvey::error().empty()) return blesurvey::error();
    return blesurvey::running() ? "" : "STARTING";
  }

  // What the BLE rows would show: if it hasn't changed, the panel doesn't redraw.
  static std::string bleSignature(const std::vector<blesurvey::Device> &d) {
    std::string s = std::to_string(d.size());
    for (size_t i = 0; i < d.size() && i < static_cast<size_t>(kScanRows); ++i) {
      s += blesurvey::addressText(d[i]) + std::to_string(d[i].rssi) +
           std::to_string(d[i].advLen) + std::to_string(d[i].rspLen);
    }
    return s;
  }
#endif

  int knownInScan() const {
    int n = 0;
    for (const uint8_t a : scan_i2c_) n += sensors::deviceName(a) != nullptr;
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
    const ui::Stat cells[4] = {
        {"BAT", d.status.percent >= 0 ? std::to_string(d.status.percent) + " %" : "--"},
        {"VBAT", vbat},
        {"USB", vbus},
        {"STATE", chargeState(d)},
    };
    drawStatRow(c, r, cells);
  }

  // The bottom panel: the i2cdetect grid, the Wi-Fi scan or the BLE scan, whichever of the
  // three the dial was last on. Its header has the command on the left and the three as tabs.
  void drawScan(ui::Canvas &c, Rect r) const {
    using namespace ui;
    hrule(c, r.x, r.y, r.w);
    r.takeTop(kRule);
    const Rect head = r.takeTop(kSectionH);
    const bool focused = focus_ == scan_;
    const char *command = scan_ == kWifi  ? "$ iw wlan0 scan"
                          : scan_ == kBle ? "$ hcitool lescan"
                                          : "$ i2cdetect -y 0";
    drawSectionHeader(c, head, command, "", {focused, false});
    drawTabs(c, head, focused);
    switch (scan_) {
      case kWifi: drawWifi(c, r); break;
#if INKDECK_BLE
      case kBle: drawBle(c, r); break;
#endif
      default: drawBus(c, r); break;
    }
  }

  void drawTabs(ui::Canvas &c, const Rect &head, bool focused) const {
    using namespace ui;
    static const char *const kTabs[3] = {"I2C", "WI-FI", "BLE"};
    constexpr int kPad = 3;
    constexpr int kGap = 4;
    const uint16_t ink = focused ? kPaper : kInk;
    const int cy = head.y + (kSectionH - kRule) / 2;
    int x = head.right() - kPadX;
    for (int i = kStops - kI2c - 1; i >= 0; --i) {
      const int w = tagWidth(kTabs[i], style::kSection, kPad);
      x -= w;
      if (kI2c + i == scan_) {
        drawTag(c, x, cy, 19, kTabs[i], style::kSection, TagKind::Inverted, kPad, ink);
      } else {
        drawText(c, x + kPad + 2, baselineCentered(style::kSection, cy - 9, 19), kTabs[i],
                 style::kSection, ink);
      }
      x -= kGap;
    }
  }

  void drawBus(ui::Canvas &c, const Rect &r) const {
    using namespace ui;
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
        if (std::find(scan_i2c_.begin(), scan_i2c_.end(), a) != scan_i2c_.end()) {
          fill(c, cell, kInk);
          drawText(c, cell, hex2(a), kGridBold, Align::Center, kPaper);
        } else {
          drawText(c, cell, "--", kGrid, Align::Center);
        }
      }
    }

    // What answered, by name, at the bottom of the panel.
    std::string names;
    for (const uint8_t a : scan_i2c_) {
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

  // The scan panel's table: a head row with a 2 px rule under it, then up to kScanRows rows of
  // 25 px, then a footer line at the bottom; a message in their place while there's nothing.
  struct Columns {
    int bars, name, a, b, dbm;  // left edges; a and b are the middle columns
    int aW, bW, dbmW, right;
  };

  void drawTableHead(ui::Canvas &c, const Rect &p, const Columns &k, const char *name,
                     const char *a, const char *b) const {
    using namespace ui;
    drawText(c, {k.name, p.y, k.a - k.name, 24}, name, style::kKey);
    drawText(c, {k.a, p.y, k.aW, 24}, a, style::kKey);
    if (b) drawText(c, {k.b, p.y, k.bW, 24}, b, style::kKey, Align::Right);
    drawText(c, {k.dbm, p.y, k.dbmW, 24}, "dBm", style::kKey, Align::Right);
    fill(c, {p.x, p.y + 22, p.w, 2}, kInk);
  }

  void drawFooter(ui::Canvas &c, const Rect &p, const std::string &left,
                  const std::string &right) const {
    using namespace ui;
    const Rect line{p.x, p.bottom() - 10 - 17, p.w, 17};
    drawText(c, line, left, style::kLabel);
    drawText(c, line, right, style::kLabel, Align::Right);
  }

  void drawMessage(ui::Canvas &c, const Rect &p, const std::string &text) const {
    ui::drawText(c, {p.x, p.y + 24, p.w, 120}, text, ui::style::kLabel, Align::Center);
  }

  void drawWifi(ui::Canvas &c, const Rect &r) const {
    using namespace ui;
    const Rect p{r.x + kPadX, r.y + 8, r.w - 2 * kPadX, r.h - 8};
    Columns k;
    k.bars = p.x;
    k.name = p.x + 28;
    k.dbmW = 44;
    k.dbm = p.right() - k.dbmW;
    k.bW = 34;
    k.b = k.dbm - k.bW;
    k.aW = 60;
    k.a = k.b - k.aW;
    drawTableHead(c, p, k, "SSID", "SEC", "CH");
    if (nets_.empty()) {
      const std::string state = wifiStateShown_;
      drawMessage(c, p, state == "SCANNING" ? "SCANNING\xE2\x80\xA6"
                        : !state.empty()    ? "! " + state
                        : focus_ == kWifi   ? "NO NETWORKS IN RANGE"
                                            : "NOT SCANNING");
      return;
    }
    int y = p.y + 24;
    for (size_t i = 0; i < nets_.size() && i < static_cast<size_t>(kScanRows); ++i, y += 25) {
      const wifisurvey::Network &n = nets_[i];
      radio::drawBars(c, k.bars, y + 12 + 7, n.rssi);
      drawText(c, {k.name, y, k.a - k.name - 8, 25}, n.ssid.empty() ? "(hidden)" : n.ssid,
               kAxisValue);
      drawText(c, {k.a, y, k.aW, 25}, wifisurvey::securityShort(n.auth), style::kKey);
      drawText(c, {k.b, y, k.bW, 25}, std::to_string(n.channel), kAxisValue, Align::Right);
      drawText(c, {k.dbm, y, k.dbmW, 25}, radio::dbm(n.rssi), kAxisValue, Align::Right);
      dashedH(c, p.x, y + 24, p.w);
    }
    const int more = static_cast<int>(nets_.size()) - kScanRows;
    const std::string count = std::to_string(nets_.size()) + (nets_.size() == 1 ? " NETWORK" : " NETWORKS");
    int first, last;
    wifisurvey::channelRange(first, last);
    drawFooter(c, p, more > 0 ? "+" + std::to_string(more) + " MORE \xC2\xB7 " + count : count,
               "2.4 GHz \xC2\xB7 CH " + std::to_string(first) + "\xE2\x80\x93" + std::to_string(last));
  }

#if INKDECK_BLE
  void drawBle(ui::Canvas &c, const Rect &r) const {
    using namespace ui;
    const Rect p{r.x + kPadX, r.y + 8, r.w - 2 * kPadX, r.h - 8};
    Columns k;
    k.bars = p.x;
    k.name = p.x + 28;
    k.dbmW = 44;
    k.dbm = p.right() - k.dbmW;
    k.bW = 0;
    k.b = k.dbm;
    k.aW = 72;
    k.a = k.dbm - k.aW;
    drawTableHead(c, p, k, "DEVICE", "KIND", nullptr);
    if (devs_.empty()) {
      const std::string state = bleState();
      drawMessage(c, p, state == "STARTING" ? (focus_ == kBle ? "STARTING BLUETOOTH\xE2\x80\xA6"
                                                              : "NOT SCANNING")
                        : !state.empty()    ? "! " + state
                                            : "LISTENING\xE2\x80\xA6");
      return;
    }
    int y = p.y + 24;
    for (size_t i = 0; i < devs_.size() && i < static_cast<size_t>(kScanRows); ++i, y += 25) {
      const blesurvey::Device &d = devs_[i];
      const bleadv::Info info = bleadv::decode(d.adv, d.advLen, d.rsp, d.rspLen);
      radio::drawBars(c, k.bars, y + 12 + 7, d.rssi);
      const bool named = !info.name.empty();
      drawText(c, {k.name, y, k.a - k.name - 8, 25}, named ? info.name : blesurvey::addressText(d),
               named ? kAxisValue : kAddress);
      drawText(c, {k.a, y, k.aW, 25}, info.kind, style::kKey);
      drawText(c, {k.dbm, y, k.dbmW, 25}, radio::dbm(d.rssi), kAxisValue, Align::Right);
      dashedH(c, p.x, y + 24, p.w);
    }
    const int more = static_cast<int>(devs_.size()) - kScanRows;
    const std::string count = std::to_string(devs_.size()) + (devs_.size() == 1 ? " DEVICE" : " DEVICES");
    drawFooter(c, p, more > 0 ? "+" + std::to_string(more) + " MORE \xC2\xB7 " + count : count,
               "LE \xC2\xB7 ACTIVE SCAN");
  }
#endif

  Panel focus_ = kEnv;
  Panel scan_ = kI2c;  // which of I2C, WI-FI and BLE the bottom panel shows
  bool held_ = false;
  bool logFailed_ = false;
  bool onTop_ = false;
  MenuRow menuLast_ = MenuRow::Wifi;
  Radio radio_ = Radio::None;
  uint32_t focusedAt_ = 0;
  uint32_t refreshedAt_[kPanelCount] = {};
  shtc3::Reading env_;
  std::vector<float> history_;
  qmi8658::Reading imu_;
  float pitch_ = 0, roll_ = 0;
  pmic::Detail pmic_;
  std::vector<uint8_t> scan_i2c_;
  std::vector<wifisurvey::Network> nets_;
  uint32_t wifiGen_ = 0;
  std::string wifiStateShown_;
#if INKDECK_BLE
  std::vector<blesurvey::Device> devs_;
  std::string bleShown_;
#endif
};

}  // namespace

std::unique_ptr<ui::Screen> makeSensors() { return std::make_unique<SensorsScreen>(); }

}  // namespace apps
