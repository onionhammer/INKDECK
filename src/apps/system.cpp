#include "apps/system.h"

#include <Arduino.h>
#include <esp_system.h>
#include <stdio.h>

#include "alarms.h"
#include "apps/idle.h"
#include "apps/sleep.h"
#include "audio.h"
#include "board_pins.h"
#include "pmic.h"
#include "rtc.h"
#include "settings.h"
#include "sysinfo.h"
#include "timesync.h"
#include "ui/menu.h"
#include "wlan.h"

namespace apps {
namespace {

using ui::Key;
using ui::Rect;
using ui::TextStyle;

// Sizes and weights from the System artboard.
constexpr TextStyle kRailName{&ui::fonts::kMono15ExtraBold, 0};
constexpr TextStyle kRailNet{&ui::fonts::kMono14Medium, 0};
constexpr TextStyle kStateValue{&ui::fonts::kMono14Bold, 0};
constexpr TextStyle kAction{&ui::fonts::kMono15Bold, 0};
constexpr TextStyle kActionSelected{&ui::fonts::kMono15ExtraBold, 0};

constexpr int kRailH = 34;
constexpr int kStateH = 21;  // 7 rows in the space 6 rows of 24 had
constexpr int kStateRows = 7;
constexpr int kStateKeyW = 104;
constexpr int kActionH = 30;
constexpr int kPillW = 52;
constexpr int kCells = 10;

enum class Action : uint8_t { Sleep, SyncTime, Update, Reboot };
constexpr const char *kActionLabels[] = {"SLEEP NOW", "SYNC TIME (NTP)", "CHECK FOR UPDATE",
                                         "REBOOT"};
constexpr int kActionCount = sizeof kActionLabels / sizeof kActionLabels[0];

std::string volts(int mv, int decimals) {
  char buf[16];
  snprintf(buf, sizeof buf, "%.*f V", decimals, mv / 1000.0);
  return buf;
}

// The pill: where the power comes from.
std::string powerSource(const pmic::Detail &d) {
  if (!d.ok) return "NO PMIC";
  return d.status.vbus ? "ON USB" : "ON BATTERY";
}

// Under the voltage: what the battery is doing. The TG28 has no battery-current ADC, so the
// canvas's "−62 mA" becomes the charger state.
std::string batteryFlow(const pmic::Detail &d) {
  if (!d.ok) return "";
  if (!d.status.battery) return "NO BATTERY";
  switch (d.flow) {
    case pmic::Flow::Charging:
      switch (d.charger) {
        case pmic::Charger::Trickle:
        case pmic::Charger::Precharge: return "PRECHARGE";
        case pmic::Charger::ConstantCurrent: return "CHARGING CC";
        case pmic::Charger::ConstantVoltage: return "CHARGING CV";
        default: return "CHARGING";
      }
    case pmic::Flow::Discharging: return "DISCHARGING";
    case pmic::Flow::Standby: break;
  }
  return d.charger == pmic::Charger::Done ? "FULL" : "IDLE";
}

std::string chargeConfig(const pmic::Detail &d) {
  if (!d.ok) return "PMIC NOT ANSWERING";
  const std::string to = d.chargeToMv ? volts(d.chargeToMv, 2) : "?";
  return "CHG " + std::to_string(d.chargeMa) + " mA \xE2\x86\x92 " + to + " \xC2\xB7 CUTOFF " +
         volts(d.cutoffMv, 1);
}

// The idle rule, or once the Reader has light-slept, how much: "napped 41 min · 63 naps".
std::string sleepState() {
  const apps::NapStats n = apps::napStats();
  if (!n.naps) return "off after 10 min idle on battery";
  char buf[48];
  snprintf(buf, sizeof buf, "napped %lu min \xC2\xB7 %lu naps",
           static_cast<unsigned long>(n.asleepSeconds / 60), static_cast<unsigned long>(n.naps));
  return buf;
}

// The RTC is armed only on the way to a power-off (power.h), so while the board runs this is
// the wake that a power-off now would set: the next alarm or snooze.
std::string nextWake() {
  if (!rtc::timeValid()) return "none (clock not set)";
  const time_t now = time(nullptr);
  int slot = -1;
  const time_t next = alarms::next(now, &slot);
  if (!next) return "none: PWR key only";
  tm t;
  localtime_r(&next, &t);
  char buf[48];
  strftime(buf, sizeof buf, next - now < 86400 ? "%H:%M" : "%a %H:%M", &t);
  return std::string(buf) + " \xC2\xB7 alarm " + std::to_string(slot + 1) + ", at power-off";
}

class SystemScreen : public ui::Screen {
 public:
  SystemScreen() {
    cursor_.reset(kActionCount);
    // Opened while a sync runs (the boot-time one, say): follow it like a SYNC TIME press.
    const timesync::Phase p = timesync::status().phase;
    syncing_ = p == timesync::Phase::Connecting || p == timesync::Phase::Asking;
  }

  std::string tag() const override { return "10 SYSTEM"; }
  std::string context() const override {
    return std::string("FW ") + sysinfo::kFirmwareVersion + " \xC2\xB7 IDF " +
           sysinfo::idfVersion();
  }
  int hints(ui::Hint *out) const override {
    out[0] = {ui::glyph::kDial, "MOVE"};
    out[1] = {ui::glyph::kPress, "RUN"};
    out[2] = {"BOOT", "BACK"};
    out[3] = {"PWR", "OFF"};
    return 4;
  }

  bool onKey(Key key) override {
    switch (key) {
      case Key::Up:
      case Key::Down:
        message_.clear();
        cursor_.move(key == Key::Up ? -1 : +1, true);
        return true;
      case Key::Select: run(static_cast<Action>(cursor_.cursor())); return true;
      default: return false;
    }
  }

  // Live values: battery and power source (the PMIC status is cached for 5 s) and the uptime
  // minute. Voltage jitter alone doesn't redraw; it refreshes with the rest. A running time sync
  // redraws as it moves on (notice and WIFI row); moving the cursor hides its notice until then.
  bool onTick() override {
    if (rebootShown_) reboot();
    bool changed = signature() != drawnSignature_;
    if (syncing_) {
      const timesync::Status s = timesync::status();
      if (s.phase != shownPhase_) {
        shownPhase_ = s.phase;
        message_ = syncMessage(s);
        changed = true;
      }
      syncing_ = s.phase == timesync::Phase::Connecting || s.phase == timesync::Phase::Asking;
    }
    return changed;
  }

  void draw(ui::Canvas &c, const Rect &body) override {
    drawnSignature_ = signature();
    rebootShown_ = rebooting_;
    const pmic::Detail d = pmic::detail();
    Rect r = body;
    const Rect actions = r.takeBottom(ui::kRule + ui::kSectionH + kActionCount * kActionH);
    const Rect state = r.takeBottom(ui::kRule + ui::kSectionH + 4 + kStateRows * kStateH + 6);
    const Rect rails = r.takeBottom(ui::kRule + ui::kSectionH + 4 * kRailH);
    drawBattery(c, r, d);
    drawRails(c, rails, d);
    drawState(c, state);
    drawActions(c, actions);
  }

 private:
  static uint32_t signature() {
    const pmic::Status s = pmic::status();
    return static_cast<uint32_t>(s.percent & 0xFF) | (s.vbus << 8) | (s.charging << 9) |
           (s.battery << 10) | (s.ok << 11) | ((sysinfo::uptimeSeconds() / 60) << 12);
  }

  void run(Action a) {
    switch (a) {
      case Action::Sleep: apps::sleepNow(); break;
      case Action::SyncTime: {
        if (!wlan::configured()) {
          message_ = "NO WI-FI: SET WIFI_SSID IN .ENV, REBUILD";
          break;
        }
        timesync::start();  // if one is already running, follow that
        const timesync::Status s = timesync::status();
        shownPhase_ = s.phase;
        message_ = syncMessage(s);
        syncing_ = true;
        break;
      }
      case Action::Update: message_ = "UPDATES AREN'T BUILT YET"; break;
      case Action::Reboot:
        ui::nav::push(std::make_unique<ui::MenuScreen>(
            "REBOOT?", std::vector<ui::MenuItem>{
                           {"CANCEL", nullptr, [] { ui::nav::pop(); }},
                           {"REBOOT NOW", nullptr,
                            [this] {
                              rebooting_ = true;
                              message_ = "REBOOTING\xE2\x80\xA6";
                              ui::nav::pop();
                            }},
                       }));
        break;
    }
  }

  static std::string syncMessage(const timesync::Status &s) {
    switch (s.phase) {
      case timesync::Phase::Connecting: return "SYNC: JOINING WI-FI\xE2\x80\xA6";
      case timesync::Phase::Asking: return "SYNC: ASKING NTP\xE2\x80\xA6";
      case timesync::Phase::Done:
      case timesync::Phase::Failed: return s.result;
      case timesync::Phase::Idle: break;
    }
    return "";
  }

  // Runs on the first tick after the "REBOOTING…" frame was drawn.
  static void reboot() {
    audio::stop();  // amp off and DAC muted first, so the speaker doesn't pop
    delay(100);
    esp_restart();
  }

  static void drawBattery(ui::Canvas &c, const Rect &area, const pmic::Detail &d) {
    using namespace ui;
    const Rect p{area.x + 20, area.y + 16, area.w - 40, area.h - 16 - 14};
    constexpr int kHeadH = 19;
    drawText(c, {p.x, p.y, p.w, kHeadH}, "// BATTERY", style::kSection);
    const std::string pill = powerSource(d);
    drawTag(c, p.right() - tagWidth(pill, style::kPill, 6), p.y + kHeadH / 2, kHeadH, pill,
            style::kPill, TagKind::Inverted, 6);

    const int percent = d.status.percent;
    const std::string big = percent >= 0 ? std::to_string(percent) + "%" : "--%";
    const int bigBase = p.y + kHeadH + 12 + capHeight(style::kBig);
    drawText(c, p.x, bigBase, big, style::kBig);
    const std::string v = d.vbatMv > 0 ? volts(d.vbatMv, 2) : "-- V";
    const std::string flow = batteryFlow(d);
    drawText(c, p.right() - textWidth(style::kValue, flow), bigBase, flow, style::kValue);
    const int vBase = bigBase - lineHeight(style::kValue) - 4;
    drawText(c, p.right() - textWidth(style::kReading, v), vBase, v, style::kReading);

    // Segmented gauge: one cell per 10 %, a part-full cell dithered.
    const int cellsTop = bigBase + 14;
    constexpr int kGap = 4;
    constexpr int kCellH = 28;
    for (int i = 0; i < kCells; ++i) {
      const int x0 = p.x + i * (p.w + kGap) / kCells;
      const int x1 = p.x + (i + 1) * (p.w + kGap) / kCells - kGap;
      const Rect cell{x0, cellsTop, x1 - x0, kCellH};
      const int fill = percent - i * 10;  // this cell's share, in percent
      if (fill >= 10) {
        ui::fill(c, cell, kInk);
      } else {
        frame(c, cell, 2);
        if (fill > 0) dither50(c, cell.inset(2, 2));
      }
    }

    const int cfgBase = cellsTop + kCellH + 10 + capHeight(style::kLabel);
    drawText(c, p.x, cfgBase, chargeConfig(d), style::kLabel);
    if (d.ok && (d.vbusMv >= 0 || d.vsysMv >= 0)) {
      const std::string usb = !d.status.vbus ? "OFF" : d.vbusMv > 0 ? volts(d.vbusMv, 2) : "--";
      const std::string sys = d.vsysMv > 0 ? volts(d.vsysMv, 2) : "--";
      drawText(c, p.x, cfgBase + 20, "USB " + usb + " \xC2\xB7 SYSTEM " + sys, style::kLabel);
    }
  }

  static void drawRails(ui::Canvas &c, const Rect &area, const pmic::Detail &d) {
    using namespace ui;
    Rect r = area;
    hrule(c, r.x, r.y, r.w);
    r.takeTop(kRule);
    char addr[16];
    snprintf(addr, sizeof addr, "TG28 @0x%02X", board::I2C_ADDR_PMIC);
    drawSectionHeader(c, r.takeTop(kSectionH), "RAILS", addr);

    struct RailRow {
      const char *name;
      const char *net;
      pmic::Rail rail;
    };
    // Nets from AGENTS.md "Power architecture" and the schematic.
    const RailRow rows[] = {{"DC1", "3V3 system", d.dc1},
                            {"ALDO1", "audio codec + amp", d.aldo1},
                            {"ALDO2", "microphone", d.aldo2},
                            {"ALDO3", "display power", d.aldo3}};
    for (const RailRow &row : rows) {
      const Rect line = r.takeTop(kRailH);
      const int x = line.x + kPadX;
      const int netX = x + 72 + 10;
      const int pillX = line.right() - kPadX - kPillW;
      drawText(c, {x, line.y, 72, line.h}, row.name, kRailName);
      const Rect net{netX, line.y, pillX - 10 - netX, line.h};
      drawText(c, net, row.net, kRailNet);
      if (d.ok) drawText(c, net, volts(row.rail.mv, 1), kRailNet, Align::Right);

      const Rect pill{pillX, line.centerY() - 10, kPillW, 20};
      if (!d.ok) {
        frame(c, pill, 2);
        drawText(c, pill, "?", style::kTag, Align::Center);
      } else if (row.rail.on) {
        fill(c, pill, kInk);
        drawText(c, pill, "ON", style::kTag, Align::Center, kPaper);
      } else {
        frame(c, pill, 2);
        drawText(c, pill, "OFF", style::kTag, Align::Center);
      }
      dashedH(c, line.x, line.bottom() - 1, line.w);
    }
  }

  static void drawState(ui::Canvas &c, const Rect &area) {
    using namespace ui;
    Rect r = area;
    hrule(c, r.x, r.y, r.w);
    r.takeTop(kRule);
    drawSectionHeader(c, r.takeTop(kSectionH), "STATE", "UP " + sysinfo::uptime());
    r.takeTop(4);
    const std::pair<const char *, std::string> rows[kStateRows] = {
        {"SLEEP", sleepState()},
        {"NEXT WAKE", nextWake()},
        {"LAST BOOT", sysinfo::bootReason()},
        {"WIFI", sysinfo::wifi()},
        {"OTA", sysinfo::otaState()},
        {"MEMORY", sysinfo::memory()},
        {"SETTINGS", settings::describe()},
    };
    for (const auto &row : rows) {
      const Rect line = r.takeTop(kStateH);
      const int x = line.x + kPadX;
      drawText(c, {x, line.y, kStateKeyW, line.h}, row.first, style::kKey);
      drawText(c, {x + kStateKeyW, line.y, line.right() - kPadX - x - kStateKeyW, line.h},
               row.second, kStateValue);
    }
  }

  void drawActions(ui::Canvas &c, const Rect &area) const {
    using namespace ui;
    Rect r = area;
    hrule(c, r.x, r.y, r.w);
    r.takeTop(kRule);
    const Rect header = r.takeTop(kSectionH);
    if (message_.empty()) {
      drawSectionHeader(c, header, "ACTIONS", cursor_.label());
    } else {
      drawNotice(c, header, message_);
    }
    for (int i = 0; i < kActionCount; ++i) {
      const Rect line = r.takeTop(kActionH);
      const bool selected = i == cursor_.cursor();
      const int x = line.x + kPadX;
      if (selected) {
        fill(c, line, kInk);
        drawText(c, {x, line.y, 14, line.h}, glyph::kPlay, style::kCap, Align::Left, kPaper);
        drawText(c, {x + 24, line.y, line.w - x - 24 - kPadX, line.h}, kActionLabels[i],
                 kActionSelected, Align::Left, kPaper);
      } else {
        drawText(c, {x + 24, line.y, line.w - x - 24 - kPadX, line.h}, kActionLabels[i],
                 kAction);
        if (i + 1 < kActionCount) dashedH(c, line.x, line.bottom() - 1, line.w);
      }
    }
  }

  ui::ListCursor cursor_;
  std::string message_;  // shown in place of the ACTIONS header until the cursor moves
  bool rebooting_ = false;
  bool rebootShown_ = false;  // the "REBOOTING…" frame has been drawn
  bool syncing_ = false;      // following a time sync
  timesync::Phase shownPhase_ = timesync::Phase::Idle;
  uint32_t drawnSignature_ = 0;
};

}  // namespace

std::unique_ptr<ui::Screen> makeSystem() { return std::make_unique<SystemScreen>(); }

}  // namespace apps
