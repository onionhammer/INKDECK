#include "apps/system.h"

#include <Arduino.h>
#include <esp_system.h>
#include <stdio.h>

#include "alarms.h"
#include "apps/home.h"
#include "apps/idle.h"
#include "apps/lock.h"
#include "apps/sleep.h"
#include "apps/updater.h"
#include "apps/wifi_drive.h"
#include "audio.h"
#include "board_pins.h"
#include "pin_places.h"
#include "ota.h"
#include "pmic.h"
#include "power.h"
#include "rtc.h"
#include "screen.h"
#include "sdcard.h"
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
constexpr int kStateH = 21;
constexpr int kStateRows = 8;
constexpr int kStateKeyW = 104;
constexpr int kPillW = 52;
constexpr int kCells = 10;
// The ACTIONS strip under its header: the two settings' values beside the ● ACTIONS button.
constexpr int kStripH = 66;
constexpr int kStripKeyW = 124;

// The actions live in a sheet (● ACTIONS), which pages once they outgrow the screen.
enum class Action : uint8_t { Sleep, SyncTime, FullRefresh, PinLock, Update, WifiDrive, Reboot };
constexpr const char *kActionLabels[] = {"SLEEP NOW", "SYNC TIME (NTP)",   "AUTO FULL REFRESH",
                                         "PIN LOCK",  "UPDATE OVER WI-FI", "WI-FI DRIVE",
                                         "REBOOT"};
constexpr int kActionCount = sizeof kActionLabels / sizeof kActionLabels[0];

// AUTO FULL REFRESH's steps, in screens' worth of pixels flipped (screen.h); 0 = off.
constexpr int kFullRefreshSteps[] = {2, 5, 10, 20, 0};

std::string fullRefreshValue() {
  const int n = screen::fullRefreshScreens();
  return n ? std::to_string(n) + " SCREENS" : "OFF";
}

// The step after the current setting (the first larger one, if it was set to something else).
int nextFullRefresh() {
  const int n = screen::fullRefreshScreens();
  for (int step : kFullRefreshSteps) {
    if (n && (step == 0 || step > n)) return step;
  }
  return kFullRefreshSteps[0];
}

// PIN LOCK's value: OFF, ON, or ON · 5 MIN with an idle lock (lock_minutes on the card, which
// only a build that locks at power_on uses; pin_places.h).
std::string pinLockValue() {
  if (!settings::hasPin()) return "OFF";
  const int minutes = pinplaces::asks(pinplaces::kPowerOn) ? settings::lockMinutes() : 0;
  return minutes > 0 ? "ON \xC2\xB7 " + std::to_string(minutes) + " MIN" : "ON";
}

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

// The card's size, from the SDMMC driver (its free space takes FatFs a while to count).
std::string sdCardState() {
  if (!sdcard::mounted()) return "none";
  char buf[32];
  snprintf(buf, sizeof buf, "%.1f GB \xC2\xB7 mounted", sdcard::sizeBytes() / 1e9);
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
    // Opened while a sync runs (the boot-time one, say): follow it like a SYNC TIME press.
    const timesync::Phase p = timesync::status().phase;
    syncing_ = p == timesync::Phase::Connecting || p == timesync::Phase::Asking;
  }

  std::string tag() const override { return moduleTag("SYSTEM"); }
  // The build's commit, so an update over Wi-Fi shows it took. The IDF version, which used to
  // be here, is on the Updater: the status bar has room for about 20 characters.
  std::string context() const override {
    return std::string("FW ") + sysinfo::kFirmwareVersion + " \xC2\xB7 " + sysinfo::buildId();
  }
  int hints(ui::Hint *out) const override {
    out[0] = {ui::glyph::kPress, "ACTIONS"};
    out[1] = {"BOOT", "BACK"};
    out[2] = {"PWR", "OFF"};
    return 3;
  }

  // The PIN's minute for System (apps/lock.h openGuarded()): onEnter() may put the wheels in
  // front of it, if it hasn't been in use for a minute.
  void onEnter() override { guardedOnTop(Guarded::Settings); }
  void onLeave() override { guardedLeft(Guarded::Settings); }

  bool onKey(Key key) override {
    guardedInUse(Guarded::Settings);
    if (key != Key::Select) return false;
    message_.clear();
    openActions();
    return true;
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
    const Rect actions = r.takeBottom(ui::kRule + ui::kSectionH + kStripH);
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

  // The sheet opens on the action last run. Each item runs inside the sheet's key handler: one
  // that changes a value in place (AUTO FULL REFRESH) leaves it open, one that opens a screen
  // takes its place, and one with something to say closes it so the notice in the ACTIONS
  // header shows.
  void openActions() {
    std::vector<ui::MenuItem> items;
    for (int i = 0; i < kActionCount; ++i) {
      const Action a = static_cast<Action>(i);
      std::function<std::string()> value;
      if (a == Action::FullRefresh) value = fullRefreshValue;
      if (a == Action::PinLock) value = pinLockValue;
      items.push_back({kActionLabels[i], value, [this, i] {
                         lastAction_ = i;
                         run(static_cast<Action>(i));
                       }});
    }
    ui::nav::push(std::make_unique<ui::MenuScreen>("ACTIONS", std::move(items), lastAction_));
  }

  // Closes the sheet with a line for the ACTIONS header.
  void say(const std::string &m) {
    message_ = m;
    ui::nav::pop();
  }

  void run(Action a) {
    switch (a) {
      case Action::Sleep:
        ui::nav::pop();
        apps::sleepNow();
        break;
      case Action::SyncTime: {
        if (!wlan::configured()) {
          say("NO WI-FI: SET ONE IN INKDECK.INI OR .ENV");
          break;
        }
        timesync::start();  // if one is already running, follow that
        const timesync::Status s = timesync::status();
        shownPhase_ = s.phase;
        syncing_ = true;
        say(syncMessage(s));
        break;
      }
      case Action::FullRefresh:
        screen::setFullRefreshScreens(nextFullRefresh());
        message_ = screen::fullRefreshScreens() ? "AFTER " + fullRefreshValue() + " OF CHANGE"
                                                : "OFF: HOLD BOOT CLEANS THE SCREEN";
        break;
      case Action::PinLock: {
        // Each task asks for the current PIN first, if there is one (apps/lock.h).
        const auto done = [this](const std::string &m) { message_ = m; };
        if (!settings::hasPin()) {
          ui::nav::replace(apps::makePinTask(apps::PinTask::Set, done));
          break;
        }
        ui::nav::replace(std::make_unique<ui::MenuScreen>(
            "PIN LOCK",
            std::vector<ui::MenuItem>{
                {"CHANGE PIN", nullptr,
                 [done] { ui::nav::replace(apps::makePinTask(apps::PinTask::Set, done)); }},
                {"CLEAR PIN", nullptr,
                 [done] { ui::nav::replace(apps::makePinTask(apps::PinTask::Clear, done)); }},
            }));
        break;
      }
      case Action::Update: {
        if (!wlan::configured()) {
          say("NO WI-FI: SET ONE IN INKDECK.INI OR .ENV");
          break;
        }
        if (!ota::configured()) {
          say("NO OTA_PASSWORD: SET IT IN .ENV, REBUILD");
          break;
        }
        // An interrupted update is harmless (the running firmware stays), but a flat battery
        // halfway through wastes the try.
        const pmic::Status p = pmic::status();
        if (p.ok && !p.vbus && p.percent >= 0 && p.percent < power::kLowBatteryPercent) {
          say("CHARGE FIRST: BATTERY BELOW " + std::to_string(power::kLowBatteryPercent) + " %");
          break;
        }
        ui::nav::replace(apps::makeUpdater());
        break;
      }
      case Action::WifiDrive:
        if (!wlan::configured()) {
          say("NO WI-FI: SET ONE IN INKDECK.INI OR .ENV");
          break;
        }
        if (!sdcard::mounted()) {
          say("NO SD CARD");
          break;
        }
        // The whole card goes on the network, so it takes the PIN that Files takes, unless
        // getting into System took it already.
        if (pinplaces::asks(pinplaces::kSettings)) {
          ui::nav::replace(apps::makeWifiDrive());
        } else {
          apps::openGuarded(apps::Guarded::Files, apps::makeWifiDrive, true);
        }
        break;
      case Action::Reboot:
        ui::nav::replace(std::make_unique<ui::MenuScreen>(
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
    const Rect p{area.x + 20, area.y + 12, area.w - 40, area.h - 12 - 14};
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
    // Nets from AGENTS.md "Power architecture" and the schematic. ALDO1 has no load (R74 isn't
    // fitted) and stays off; ALDO2 is on while audio plays or records, ALDO3 off while napping.
    const RailRow rows[] = {{"DC1", "3V3 system", d.dc1},
                            {"ALDO1", "unused", d.aldo1},
                            {"ALDO2", "codec + mic", d.aldo2},
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
        {"SD CARD", sdCardState()},
    };
    for (const auto &row : rows) {
      const Rect line = r.takeTop(kStateH);
      const int x = line.x + kPadX;
      drawText(c, {x, line.y, kStateKeyW, line.h}, row.first, style::kKey);
      drawText(c, {x + kStateKeyW, line.y, line.right() - kPadX - x - kStateKeyW, line.h},
               row.second, kStateValue);
    }
  }

  // The header (or the last action's notice), then the two settings' values on the left and
  // the ● ACTIONS button that opens the sheet on the right.
  void drawActions(ui::Canvas &c, const Rect &area) const {
    using namespace ui;
    Rect r = area;
    hrule(c, r.x, r.y, r.w);
    r.takeTop(kRule);
    const Rect header = r.takeTop(kSectionH);
    if (message_.empty()) {
      drawSectionHeader(c, header, "ACTIONS", std::to_string(kActionCount));
    } else {
      drawNotice(c, header, message_);
    }
    constexpr int kButtonH = 44, kShadow = 6;
    const int top = r.y + (r.h - kButtonH - kShadow) / 2;
    const Rect button = drawButton(c, r.right() - 20 - kShadow, top, glyph::kPress, "ACTIONS");
    const std::pair<const char *, std::string> rows[] = {
        {"FULL REFRESH", fullRefreshValue()},
        {"PIN LOCK", pinLockValue()},
    };
    const int x = r.x + kPadX;
    int y = top + (kButtonH - 2 * kStateH) / 2;
    for (const auto &row : rows) {
      drawText(c, {x, y, kStripKeyW, kStateH}, row.first, style::kKey);
      drawText(c, {x + kStripKeyW, y, button.x - 12 - x - kStripKeyW, kStateH}, row.second,
               kStateValue);
      y += kStateH;
    }
  }

  int lastAction_ = 0;  // where the sheet opens: the action last run
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
