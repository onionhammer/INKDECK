#include "apps/home.h"

#include <Arduino.h>
#include <stdio.h>
#include <string.h>

#include "alarms.h"
#include "apps/clock.h"
#include "apps/files.h"
#include "apps/level.h"
#include "apps/lock.h"
#include "apps/music.h"
#include "apps/podcasts.h"
#include "apps/recorder.h"
#include "apps/reader.h"
#include "apps/sensors.h"
#include "apps/system.h"
#include "apps/weather.h"
#include "audio.h"
#include "covers.h"
#include "fonts.h"
#include "pmic.h"
#include "podcast_library.h"
#include "readstate.h"
#include "rtc.h"
#include "flashfs.h"
#include "sdcard.h"
#include "sensor_hub.h"
#include "suntimes.h"
#include "sysinfo.h"
#include "timesync.h"
#include "units.h"
#include "forecast.h"

namespace apps {
namespace {

using ui::Key;
using ui::Rect;

enum class Kind : uint8_t {
  Reader, Music, Podcasts, Files, Sensors, System, Clock, Weather, Level, Recorder
};

struct Module {
  const char *label;
  const char *bus;  // tag in the list and the preview
  const char *blurb;
  const char *hw;
  Kind kind;
};

// Copy from the design canvas, trimmed to what the firmware does today. The order is the
// user's (2026-11-13): what's used most first, related modules together. Screen tags follow it
// through moduleTag().
// The \xC2\xB7 escapes in these strings are the middle dot (UTF-8 U+00B7).
constexpr Module kModules[] = {
    {"READER", "SD", "EPUB books from microSD. Opens where you left off.",
     "microSD \xC2\xB7 SDMMC 4-bit", Kind::Reader},
    {"PODCASTS", "NET", "RSS over Wi-Fi, or folders of MP3s. Episodes resume where you stopped.",
     "Wi-Fi \xC2\xB7 microSD \xC2\xB7 ES8311", Kind::Podcasts},
    {"MUSIC", "I2S", "MP3 files from microSD through the onboard codec and speaker.",
     "ES8311 \xC2\xB7 NS4150B \xC2\xB7 I2S", Kind::Music},
    {"WEATHER", "NET", "Now, hourly and 6-day forecast over Wi-Fi, every half hour on USB.",
     "Wi-Fi \xC2\xB7 Open-Meteo", Kind::Weather},
    {"CLOCK", "RTC", "Desk clock and alarms. An alarm can boot the board from power-off.",
     "PCF85063A @ 0x51", Kind::Clock},
    {"RECORDER", "MIC", "Voice memos from the onboard mic, written to SD as WAV.",
     "ES8311 ADC \xC2\xB7 ALDO2", Kind::Recorder},
    {"SENSORS", "I2C", "Live readouts from every chip on the shared I2C bus.",
     "SHTC3 \xC2\xB7 QMI8658A \xC2\xB7 TG28", Kind::Sensors},
    {"LEVEL", "IMU", "Bubble level for surfaces, a vial for edges, and a tone when it's level.",
     "QMI8658A @ 0x6B", Kind::Level},
    {"FILES", "SD",
     "Browse the microSD card, onboard flash and NFS shares. Books, music, pictures, text.",
     "SDMMC 4-bit \xC2\xB7 16 MB flash", Kind::Files},
    {"SYSTEM", "PMIC", "Battery, power rails, sleep schedule, Wi-Fi and firmware updates.",
     "TG28 (AXP2101) @ 0x34", Kind::System},
};
constexpr int kModuleCount = sizeof kModules / sizeof kModules[0];

std::string twoDigits(int n) {
  char buf[12];  // room for any int, so the compiler can see nothing is cut off
  snprintf(buf, sizeof buf, "%02d", n);
  return buf;
}

std::string cardSize() {
  if (!sdcard::mounted()) return "NO CARD";
  char buf[16];
  snprintf(buf, sizeof buf, "%.1f GB", sdcard::sizeBytes() / 1e9);
  return buf;
}

std::string flashSize() {
  if (!flashfs::available()) return "NONE";
  char buf[16];
  snprintf(buf, sizeof buf, "%.1f MB", flashfs::partitionBytes() / 1048576.0);
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
      out[1] = {"FLASH FAT", flashSize()};
      out[2] = {"OPENS", "EPUB MP3 JPG TXT"};
      return 3;
    case Kind::Sensors: {
      const shtc3::Reading e = sensors::env();
      char temp[16] = "--", rh[16] = "--";
      if (e.ok) {
        snprintf(temp, sizeof temp, "%.1f %s", units::temperature(e.tempC),
                 units::temperatureUnit());
        snprintf(rh, sizeof rh, "%ld %%", lroundf(e.rh));
      }
      out[0] = {"TEMP", temp};
      out[1] = {"HUMIDITY", rh};
      const qmi8658::Reading m = sensors::motion();
      float pitch = 0, roll = 0;
      sensors::tilt(m, pitch, roll);
      out[2] = {"TILT", m.ok ? ui::signedNumber(pitch, 1) + "\xC2\xB0 / " +
                                   ui::signedNumber(roll, 1) + "\xC2\xB0"
                             : "--"};
      out[3] = {"BUS", std::to_string(sensors::knownFound()) + "/" +
                           std::to_string(sensors::kKnownCount) + " ACK" +
                           (sensors::logging() ? " \xC2\xB7 LOG" : "")};
      return 4;
    }
    case Kind::System: {
      const pmic::Status p = pmic::status();
      out[0] = {"BATTERY", p.percent >= 0 ? std::to_string(p.percent) + " %" : "NONE"};
      out[1] = {"POWER", !p.ok ? "?" : p.charging ? "USB, CHARGING" : p.vbus ? "USB" : "BATTERY"};
      out[2] = {"UPTIME", sysinfo::uptime()};
      out[3] = {"CLOCK", rtc::timeValid() ? "SET" : "NOT SET"};
      return 4;
    }
    case Kind::Clock: {
      const bool valid = rtc::timeValid();
      const time_t now = time(nullptr);
      tm t;
      localtime_r(&now, &t);
      char buf[24] = "--:--";
      if (valid) strftime(buf, sizeof buf, "%H:%M %Z", &t);
      out[0] = {"TIME", buf};

      const time_t next = valid ? alarms::next(now) : 0;
      std::string alarm = valid ? "ALL OFF" : ui::glyph::kEmDash;
      if (next) {
        tm a;
        localtime_r(&next, &a);
        strftime(buf, sizeof buf, next - now < 86400 ? "%H:%M" : "%a %H:%M", &a);
        alarm = ui::upper(buf);
      }
      out[1] = {"NEXT ALARM", alarm};

      std::string sunText = sun::configured() ? ui::glyph::kEmDash : "SET LAT/LON IN .ENV";
      const sun::Day d = valid ? sun::on(now) : sun::Day{};
      if (d.ok) {
        char rise[8], set[8];
        tm r, s;
        localtime_r(&d.rise, &r);
        localtime_r(&d.set, &s);
        strftime(rise, sizeof rise, "%H:%M", &r);
        strftime(set, sizeof set, "%H:%M", &s);
        sunText = std::string("\xE2\x86\x91") + rise + " \xE2\x86\x93" + set;  // ↑ ↓
      }
      out[2] = {"SUN", sunText};

      const time_t synced = timesync::lastSync();
      std::string ntp = "NEVER";
      if (synced) {
        tm s;
        localtime_r(&synced, &s);
        strftime(buf, sizeof buf, "%d %b %H:%M", &s);
        ntp = ui::upper(buf);
      }
      out[3] = {"NTP SYNC", ntp};
      return 4;
    }
    case Kind::Podcasts: {
      static bool loaded = false;  // the library is read when the row is first shown
      if (!loaded) loaded = podcasts::load(false);
      const int shows = podcasts::showCount();
      out[0] = {"SHOWS", shows ? std::to_string(shows) : "NONE"};
      const int fresh = podcasts::freshTotal();
      out[1] = {"NEW", fresh ? std::to_string(fresh) : ui::glyph::kEmDash};
      const podcasts::Activity a = podcasts::activity();
      out[2] = {"NETWORK", a.text.empty() ? "IDLE" : a.text};
      const int64_t used = podcasts::usedBytes();
      char buf[24];
      if (used < 0) {
        snprintf(buf, sizeof buf, "%s", "\xE2\x80\xA6");
      } else if (used < 1000000000LL) {
        snprintf(buf, sizeof buf, "%.0f MB", used / 1e6);
      } else {
        snprintf(buf, sizeof buf, "%.1f GB", used / 1e9);
      }
      out[3] = {"ON SD", buf};
      return 4;
    }
    case Kind::Weather: {
      weather::Forecast f;
      const time_t now = time(nullptr);
      const bool has = rtc::timeValid() && weather::get(f);
      const weather::Hour *h = has ? weather::at(f, now) : nullptr;
      out[0] = {"NOW", h ? temperatureText(h->tempC) + " " + weather::condition(h->code)
                         : ui::glyph::kEmDash};
      const weather::Day *today[1];
      std::string range = ui::glyph::kEmDash;
      if (has && weather::daysFrom(f, now, today, 1)) {
        range = temperatureText(today[0]->loC) + " / " + temperatureText(today[0]->hiC);
      }
      out[1] = {"TODAY LO / HI", range};
      // The likeliest rain in the next 12 hours, and when.
      const weather::Hour *hours[12];
      const int n = has ? weather::hoursAfter(f, now, hours, 12) : 0;
      const weather::Hour *wettest = nullptr;
      for (int i = 0; i < n; ++i) {
        if (hours[i]->rain != weather::kUnknown && (!wettest || hours[i]->rain > wettest->rain)) {
          wettest = hours[i];
        }
      }
      std::string rain = ui::glyph::kEmDash;
      if (wettest) {
        char buf[24];
        tm t;
        localtime_r(&wettest->time, &t);
        if (wettest->rain == 0) {
          snprintf(buf, sizeof buf, "NONE");
        } else {
          snprintf(buf, sizeof buf, "%u %% AT %02d:00", wettest->rain, t.tm_hour);
        }
        rain = buf;
      }
      out[2] = {"RAIN, 12 H", rain};
      std::string updated = weather::configured() ? "NEVER" : "SET LAT/LON, WI-FI";
      if (weather::syncing()) {
        updated = "SYNCING\xE2\x80\xA6";
      } else if (has) {
        char buf[24];
        tm t;
        localtime_r(&f.fetched, &t);
        strftime(buf, sizeof buf, "%a %H:%M", &t);
        updated = ui::upper(buf);
      }
      out[3] = {"UPDATED", updated};
      return 4;
    }
    case Kind::Level: {
      const qmi8658::Reading m = sensors::motion();
      if (!m.ok) {
        out[0] = {"IMU", "NOT ANSWERING"};
        return 1;
      }
      const float g = sqrtf(m.acc[0] * m.acc[0] + m.acc[1] * m.acc[1] + m.acc[2] * m.acc[2]);
      const bool flat = g > 0 && fabsf(m.acc[2]) / g >= 0.7f;
      float pitch = 0, roll = 0;
      sensors::tilt(m, pitch, roll);
      char tilt[24];
      snprintf(tilt, sizeof tilt, "%.1f\xC2\xB0", sqrtf(pitch * pitch + roll * roll));
      out[0] = {"POSE", flat ? "LYING FLAT" : "ON AN EDGE"};
      out[1] = {"TILT", flat ? tilt : "--"};
      out[2] = {"OPENS", "LANDSCAPE"};
      out[3] = {"IMU", "QMI8658A \xC2\xB7 56 Hz"};
      return 4;
    }
    case Kind::Recorder: {
      const MemoSummary memos = memoSummary();
      const audio::RecordStatus r = audio::recordStatus();
      out[0] = {"MEMOS", sdcard::mounted() ? std::to_string(memos.count) : "NO CARD"};
      out[1] = {"LAST", memos.last.empty() ? "--" : memos.last};
      out[2] = {"FORMAT", "16 kHz MONO WAV"};
      out[3] = {"MIC GAIN", std::to_string(r.gainDb) + " dB"};
      return 4;
    }
  }
  return 0;
}

class HomeScreen : public ui::Screen {
 public:
  HomeScreen() { cursor_.reset(kModuleCount); }

  std::string tag() const override { return "INKDECK/OS"; }
  std::string context() const override { return "~/home"; }
  // The room temperature, held so its wandering last digit doesn't redraw the status bar.
  std::string statusTemp() const override {
    const shtc3::Reading e = sensors::steadyEnv();
    if (!e.ok) return {};
    char buf[16];
    snprintf(buf, sizeof buf, "%.1f%s", units::temperature(e.tempC), units::temperatureUnit());
    return buf;
  }
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
    const Kind kind = kModules[cursor_.cursor()].kind;
    bool changed = g != drawnGeneration_ && kind == Kind::Music;
    drawnGeneration_ = g;
    // A forecast sync starting or finishing in the background.
    const weather::Phase w = weather::status().phase;
    changed |= w != drawnWeather_ && kind == Kind::Weather;
    drawnWeather_ = w;
    return changed;
  }

  void draw(ui::Canvas &c, const Rect &body) override {
    drawnGeneration_ = audio::status().generation;
    drawnWeather_ = weather::status().phase;
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
          openLibrary("books");
        }
        break;
      case Kind::Music:
        if (audio::active()) {
          ui::nav::push(makeMusic());
        } else {
          openLibrary("music");
        }
        break;
      case Kind::Files:
        openGuarded(Guarded::Files, [] { return makeFiles(sdcard::kMountPoint); });
        break;
      case Kind::Sensors: ui::nav::push(makeSensors()); break;
      case Kind::System: openGuarded(Guarded::Settings, makeSystem); break;
      case Kind::Clock: ui::nav::push(makeClock()); break;
      case Kind::Podcasts: ui::nav::push(makePodcasts()); break;
      case Kind::Weather: ui::nav::push(makeWeather()); break;
      case Kind::Level: ui::nav::push(makeLevel()); break;
      case Kind::Recorder: ui::nav::push(makeRecorder()); break;
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

    // READER: the last book's cover, framed, with a dithered shadow like the button's; the
    // stats move right of it.
    Rect statsArea{p.x, y, p.w, 96};
    if (m.kind == Kind::Reader) {
      constexpr int kFrame = 2, kShadow = 4, kMaxW = 84, kGap = 16;
      const int maxH = (p.bottom() - 44 - 10) - y - kShadow - 2 * kFrame;
      std::string path, title;
      readstate::Position pos;
      picture::Bitmap cover;
      if (readstate::last(path, title, pos) && covers::thumbnail(path, kMaxW, maxH, cover)) {
        const Rect box{p.x, y, cover.w + 2 * kFrame, cover.h + 2 * kFrame};
        dither50(c, {box.x + kShadow, box.bottom(), box.w, kShadow});
        dither50(c, {box.right(), box.y + kShadow, kShadow, box.h - kShadow});
        frame(c, box, kFrame);
        picture::draw(c, box.x + kFrame, box.y + kFrame, cover);
        const int shift = box.w + kShadow + kGap;
        statsArea.x += shift;
        statsArea.w -= shift;
      }
    }

    Stat stats[4];
    const int n = statsFor(m, stats);
    const int colW = (statsArea.w - 20) / 2;
    for (int k = 0; k < n; ++k) {
      drawStat(c, {statsArea.x + (k % 2) * (colW + 20), y + (k / 2) * 48, colW, 48},
               stats[k].key, stats[k].value);
    }

    const Rect button = drawButton(c, p.right() - 6, p.bottom() - 44, glyph::kPress, "OPEN");
    drawText(c, {p.x, button.y, button.x - p.x - 14, button.h},
             std::string("HW ") + glyph::kPointer + " " + m.hw, style::kLabel);
  }

  ui::ListCursor cursor_;
  uint32_t drawnGeneration_ = 0;
  weather::Phase drawnWeather_ = weather::Phase::Idle;
};

}  // namespace

std::unique_ptr<ui::Screen> makeHome() { return std::make_unique<HomeScreen>(); }

std::string moduleTag(const char *label) {
  for (int i = 0; i < kModuleCount; ++i) {
    if (strcmp(kModules[i].label, label) == 0) return twoDigits(i + 1) + " " + label;
  }
  return label;
}

}  // namespace apps
