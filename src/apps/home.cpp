#include "apps/home.h"

#include <Arduino.h>
#include <stdio.h>

#include "apps/files.h"
#include "apps/music.h"
#include "apps/reader.h"
#include "apps/system.h"
#include "audio.h"
#include "fonts.h"
#include "pmic.h"
#include "readstate.h"
#include "rtc.h"
#include "sdcard.h"
#include "sysinfo.h"

namespace apps {
namespace {

using ui::Key;
using ui::Rect;

enum class Kind : uint8_t { Reader, Music, Files, System, NotBuilt };

struct Module {
  const char *label;
  const char *bus;  // tag in the list and the preview
  const char *blurb;
  const char *hw;
  Kind kind;
  int phase;  // PROGRESS.md phase that builds it
};

// Order and copy from the design canvas, trimmed to what the firmware does today.
// The \xC2\xB7 escapes in these strings are the middle dot (UTF-8 U+00B7).
constexpr Module kModules[] = {
    {"READER", "SD", "EPUB books from microSD. Opens where you left off.",
     "microSD \xC2\xB7 SDMMC 4-bit", Kind::Reader, 5},
    {"MUSIC", "I2S", "MP3 files from microSD through the onboard codec and speaker.",
     "ES8311 \xC2\xB7 NS4150B \xC2\xB7 I2S", Kind::Music, 6},
    {"PODCASTS", "NET", "RSS over Wi-Fi. Episodes cache to SD for offline listening.",
     "Wi-Fi \xC2\xB7 microSD \xC2\xB7 ES8311", Kind::NotBuilt, 11},
    {"WEATHER", "NET", "Now, hourly and 6-day forecast, pulled over Wi-Fi every hour.",
     "Wi-Fi \xC2\xB7 PCF85063A alarm", Kind::NotBuilt, 11},
    {"SENSORS", "I2C", "Live readouts from every chip on the shared I2C bus.",
     "SHTC3 \xC2\xB7 QMI8658A \xC2\xB7 TG28", Kind::NotBuilt, 8},
    {"CLOCK", "RTC", "Desk clock and alarms. An alarm can boot the board from power-off.",
     "PCF85063A @ 0x51", Kind::NotBuilt, 9},
    {"RECORDER", "MIC", "Voice memos from the onboard mic, written to SD as WAV.",
     "ES8311 ADC \xC2\xB7 ALDO2", Kind::NotBuilt, 12},
    {"LEVEL", "IMU", "Bubble level and tilt alarm from the 6-axis IMU.",
     "QMI8658A \xC2\xB7 INT2 GPIO40", Kind::NotBuilt, 12},
    {"FILES", "SD", "Browse the microSD card. Opens books and plays music.",
     "SDMMC 4-bit \xC2\xB7 20 MHz", Kind::Files, 4},
    {"SYSTEM", "PMIC", "Battery, power rails, sleep schedule, Wi-Fi and firmware updates.",
     "TG28 (AXP2101) @ 0x34", Kind::System, 7},
};
constexpr int kModuleCount = sizeof kModules / sizeof kModules[0];

std::string twoDigits(int n) {
  char buf[8];
  snprintf(buf, sizeof buf, "%02d", n);
  return buf;
}

std::string moduleTag(int i) { return twoDigits(i + 1) + " " + kModules[i].label; }

std::string cardSize() {
  if (!sdcard::mounted()) return "NO CARD";
  char buf[16];
  snprintf(buf, sizeof buf, "%.1f GB", sdcard::sizeBytes() / 1e9);
  return buf;
}

struct Stat {
  std::string key;
  std::string value;
};

// Up to four live readings for the preview. Only real values; unbuilt modules say so.
int statsFor(const Module &m, Stat out[4]) {
  switch (m.kind) {
    case Kind::Reader: {
      std::string path, title;
      readstate::Position pos;
      if (!readstate::last(path, title, pos)) {
        out[0] = {"LAST", ui::glyph::kEmDash};
        out[1] = {"SD CARD", cardSize()};
        return 2;
      }
      out[0] = {"LAST", title};
      out[1] = {"PLACE", std::to_string(pos.percent) + " %"};
      out[2] = {"FONT", ui::upper(fonts::kReader[pos.fontSize % fonts::kReaderSizeCount].name)};
      out[3] = {"SD CARD", cardSize()};
      return 4;
    }
    case Kind::Music: {
      const audio::Status s = audio::status();
      const bool on = s.state != audio::State::Stopped;
      out[0] = {"NOW", on ? s.title : ui::glyph::kEmDash};
      out[1] = {"STATE", s.state == audio::State::Playing  ? "PLAYING"
                         : s.state == audio::State::Paused ? "PAUSED"
                                                           : "STOPPED"};
      out[2] = {"QUEUE", on ? std::to_string(s.track + 1) + "/" + std::to_string(s.trackCount)
                            : ui::glyph::kEmDash};
      out[3] = {"VOLUME", std::to_string(s.volume) + " %"};
      return 4;
    }
    case Kind::Files:
      out[0] = {"SD CARD", cardSize()};
      out[1] = {"MOUNT", sdcard::kMountPoint};
      out[2] = {"OPENS", "EPUB \xC2\xB7 MP3"};
      return 3;
    case Kind::System: {
      const pmic::Status p = pmic::status();
      out[0] = {"BATTERY", p.percent >= 0 ? std::to_string(p.percent) + " %" : "NONE"};
      out[1] = {"POWER", !p.ok ? "?" : p.charging ? "USB, CHARGING" : p.vbus ? "USB" : "BATTERY"};
      out[2] = {"UPTIME", sysinfo::uptime()};
      out[3] = {"CLOCK", rtc::timeValid() ? "SET" : "NOT SET"};
      return 4;
    }
    case Kind::NotBuilt:
      break;
  }
  out[0] = {"STATUS", "NOT BUILT"};
  out[1] = {"PHASE", std::to_string(m.phase) + " IN PROGRESS.MD"};
  return 2;
}

// Stand-in for modules without a screen yet: the chrome, what it will do, which phase.
class NotBuiltScreen : public ui::Screen {
 public:
  explicit NotBuiltScreen(int index) : index_(index) {}

  std::string tag() const override { return moduleTag(index_); }
  std::string context() const override { return "not built yet"; }
  int hints(ui::Hint *out) const override {
    out[0] = {"BOOT", "BACK"};
    return 1;
  }
  bool onKey(Key) override { return false; }

  void draw(ui::Canvas &c, const Rect &body) override {
    const Module &m = kModules[index_];
    Rect r = body;
    ui::drawSectionHeader(c, r.takeTop(ui::kSectionH), m.label, std::string("BUS:") + m.bus);
    Rect p = r.inset(20, 24);
    int y = p.y + ui::capHeight(ui::style::kTitle);
    ui::drawText(c, p.x, y, "NOT BUILT", ui::style::kTitle);
    y += 24;
    for (const auto &line : ui::wrap(ui::style::kBody, m.blurb, p.w, 4)) {
      y += 22;
      ui::drawText(c, p.x, y, line, ui::style::kBody);
    }
    y += 22;
    ui::hrule(c, p.x, y, p.w);
    y += ui::kRule + 10;
    ui::drawStat(c, {p.x, y, p.w, 48}, "HARDWARE", m.hw);
    ui::drawStat(c, {p.x, y + 48, p.w, 48}, "BUILT IN", "PHASE " + std::to_string(m.phase) +
                                                           " (PROGRESS.MD)");
  }

 private:
  int index_;
};

class HomeScreen : public ui::Screen {
 public:
  HomeScreen() { cursor_.reset(kModuleCount); }

  std::string tag() const override { return "INKDECK/OS"; }
  std::string context() const override { return "~/home"; }
  int hints(ui::Hint *out) const override {
    out[0] = {ui::glyph::kDial, "DIAL"};
    out[1] = {ui::glyph::kPress, "OPEN"};
    out[2] = {"BOOT", "BACK"};
    out[3] = {"PWR", "OFF"};
    return 4;
  }

  bool onKey(Key key) override {
    switch (key) {
      case Key::Up: return cursor_.move(-1, true);
      case Key::Down: return cursor_.move(+1, true);
      case Key::Select: open(cursor_.cursor()); return true;
      default: return false;
    }
  }

  // The MUSIC preview shows the playing track.
  bool onTick() override {
    const uint32_t g = audio::status().generation;
    const bool changed = g != drawnGeneration_ && kModules[cursor_.cursor()].kind == Kind::Music;
    drawnGeneration_ = g;
    return changed;
  }

  void draw(ui::Canvas &c, const Rect &body) override {
    drawnGeneration_ = audio::status().generation;
    Rect r = body;
    const Rect list = r.takeBottom(kModuleCount * ui::kRowH);
    const Rect header = r.takeBottom(ui::kSectionH);
    ui::hrule(c, r.x, r.bottom() - ui::kRule, r.w);
    r.h -= ui::kRule;

    drawPreview(c, r, cursor_.cursor());
    ui::drawSectionHeader(c, header, "MODULES", cursor_.label());
    ui::drawList(c, list, ui::kRowH, cursor_, [](int i) {
      return ui::Row{twoDigits(i + 1), kModules[i].label, "", kModules[i].bus, ""};
    });
  }

 private:
  void open(int i) {
    switch (kModules[i].kind) {
      case Kind::Reader:
        if (auto reader = resumeLastBook()) {
          ui::nav::push(std::move(reader));
        } else {
          ui::nav::push(makeFiles(sdFolder("books")));
        }
        break;
      case Kind::Music:
        if (audio::active()) {
          ui::nav::push(makeMusic());
        } else {
          ui::nav::push(makeFiles(sdFolder("music")));
        }
        break;
      case Kind::Files: ui::nav::push(makeFiles(sdcard::kMountPoint)); break;
      case Kind::System: ui::nav::push(makeSystem()); break;
      case Kind::NotBuilt: ui::nav::push(std::make_unique<NotBuiltScreen>(i)); break;
    }
  }

  // Big index + bus tag + name, blurb, rule, 2x2 stats, then the HW line and OPEN button.
  static void drawPreview(ui::Canvas &c, const Rect &area, int i) {
    using namespace ui;
    const Module &m = kModules[i];
    const Rect p{area.x + 20, area.y + 18, area.w - 40, area.h - 18 - 16};

    const std::string num = twoDigits(i + 1);
    const int base = p.y + capHeight(style::kHuge);
    drawText(c, p.x, base, num, style::kHuge);
    const int colX = p.x + textWidth(style::kHuge, num) + 16;
    drawText(c, colX, base, m.label, style::kTitle);
    const int tagCenter = base - capHeight(style::kTitle) - 10 - 10;
    drawTag(c, colX, tagCenter, 20, std::string("BUS:") + m.bus, style::kPill, TagKind::Inverted,
            8);

    int y = base + 4;
    for (const auto &line : wrap(style::kBody, m.blurb, p.w, 2)) {
      y += 22;
      drawText(c, p.x, y, line, style::kBody);
    }
    y = base + 4 + 44 + 10 + 6;
    hrule(c, p.x, y, p.w);
    y += kRule + 10;

    Stat stats[4];
    const int n = statsFor(m, stats);
    const int colW = (p.w - 20) / 2;
    for (int k = 0; k < n; ++k) {
      drawStat(c, {p.x + (k % 2) * (colW + 20), y + (k / 2) * 48, colW, 48}, stats[k].key,
               stats[k].value);
    }

    const Rect button = drawButton(c, p.right() - 6, p.bottom() - 44, glyph::kPress, "OPEN");
    drawText(c, {p.x, button.y, button.x - p.x - 14, button.h},
             std::string("HW ") + glyph::kPointer + " " + m.hw, style::kLabel);
  }

  ui::ListCursor cursor_;
  uint32_t drawnGeneration_ = 0;
};

}  // namespace

std::unique_ptr<ui::Screen> makeHome() { return std::make_unique<HomeScreen>(); }

}  // namespace apps
