#include "apps/home.h"

#include <Arduino.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <algorithm>

#include "alarms.h"
#include "apps/clock.h"
#include "apps/files.h"
#include "apps/library.h"
#include "apps/lock.h"
#include "apps/music.h"
#include "apps/podcasts.h"
#include "apps/news.h"
#include "apps/recorder.h"
#include "apps/reader.h"
#include "apps/sensors.h"
#include "apps/system.h"
#include "features.h"
#if INKDECK_AGENT
#include "apps/agent.h"
#include "agent_client.h"
#endif
#if INKDECK_COMMS
#include "apps/comms_ui.h"
#endif
#include "apps/weather.h"
#include "audio.h"
#include "covers.h"
#include "fonts.h"
#include "pmic.h"
#include "podcast_library.h"
#include "news_library.h"
#include "readstate.h"
#include "rtc.h"
#include "flashfs.h"
#include "sdcard.h"
#include "sensor_hub.h"
#include "settings.h"
#include "suntimes.h"
#include "sysinfo.h"
#include "timesync.h"
#include "timeutil.h"
#include "units.h"
#include "forecast.h"
#include "wlan.h"

namespace apps {
namespace {

using ui::Key;
using ui::Rect;

enum class Kind : uint8_t {
  Reader, Music, Podcasts, Files, Sensors, System, Clock, Weather, Recorder, News,
#if INKDECK_AGENT
  Agent,
#endif
#if INKDECK_COMMS
  Comms,
#endif
};

struct Module {
  const char *label;
  const char *bus;  // tag in the list and the preview
  const char *blurb;
  const char *hw;
  Kind kind;
};

// Copy from the design canvas, trimmed to what the firmware does today. The order is the
// user's (2026-11-13; AGENT up to 03 and MUSIC down to 07 on 2026-11-20, issue #52): what's
// used most first, related modules together; NEWS up to 02 and PODCASTS down to 05 on
// 2026-11-21; COMMS at 05 above PODCASTS and LEVEL off Home (into Sensors' menu) on 2026-11-22,
// issue #66. Screen tags follow it through moduleTag().
// The \xC2\xB7 escapes in these strings are the middle dot (UTF-8 U+00B7).
constexpr Module kModules[] = {
    {"READER", "SD", "EPUB books from microSD. Opens where you left off.",
     "microSD \xC2\xB7 SDMMC 4-bit", Kind::Reader},
    {"NEWS", "NET", "RSS and Atom feeds over Wi-Fi, kept on the card to read offline.",
     "Wi-Fi \xC2\xB7 microSD", Kind::News},
#if INKDECK_AGENT
    {"AGENT", "NET",
     "Long jobs inkharness runs while the board sleeps. It asks here when you need to choose.",
     "Wi-Fi \xC2\xB7 ES8311 \xC2\xB7 inkharness", Kind::Agent},
#endif
    {"WEATHER", "NET", "Now, hourly and 6-day forecast over Wi-Fi, every half hour on USB.",
     "Wi-Fi \xC2\xB7 Open-Meteo", Kind::Weather},
#if INKDECK_COMMS
    {"COMMS", "RF",
     "Walkie-talkies with the other boards in your groups: board to board, no router.",
     "ESP-NOW \xC2\xB7 ES8311", Kind::Comms},
#endif
    {"PODCASTS", "NET", "RSS over Wi-Fi, or folders of MP3s. Episodes resume where you stopped.",
     "Wi-Fi \xC2\xB7 microSD \xC2\xB7 ES8311", Kind::Podcasts},
    {"CLOCK", "RTC", "Desk clock and alarms. An alarm can boot the board from power-off.",
     "PCF85063A @ 0x51", Kind::Clock},
    {"RECORDER", "MIC", "Voice memos from the onboard mic, written to SD as WAV.",
     "ES8311 ADC \xC2\xB7 ALDO2", Kind::Recorder},
    {"MUSIC", "I2S", "MP3 files from microSD through the onboard codec and speaker.",
     "ES8311 \xC2\xB7 NS4150B \xC2\xB7 I2S", Kind::Music},
    {"SENSORS", "I2C", "Live readouts from every chip on the I2C bus, and Wi-Fi and Bluetooth LE scans.",
     "SHTC3 \xC2\xB7 QMI8658A \xC2\xB7 TG28", Kind::Sensors},
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
#if INKDECK_AGENT
// The next scheduled run: "07:00", or "TUE 07:00" past the next 18 hours.
std::string agentNext(uint32_t unix) {
  const time_t now = time(nullptr);
  const time_t t = static_cast<time_t>(unix);
  tm local;
  localtime_r(&t, &local);
  char buf[16];
  strftime(buf, sizeof buf, t - now < 18 * 3600 ? "%H:%M" : "%a %H:%M", &local);
  for (char *p = buf; *p; ++p) *p = static_cast<char>(toupper(*p));
  return buf;
}
#endif

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
    case Kind::News: {
      static bool loaded = false;  // the library is read when the row is first shown
      if (!loaded) loaded = news::load(false);
      const int fresh = news::freshCount();
      out[0] = {"NEW", fresh ? std::to_string(fresh) : ui::glyph::kEmDash};
      const int feeds = news::feedCount();
      out[1] = {"FEEDS", feeds ? std::to_string(feeds) : "NONE"};
      const int saved = news::savedCount();
      out[2] = {"SAVED", saved ? std::to_string(saved) : ui::glyph::kEmDash};
      const news::Activity a = news::activity();
      std::string synced = a.busy ? a.text : std::string();
      if (!a.busy) {
        const time_t t = news::lastSync();
        if (!t) {
          synced = "NEVER";
        } else {
          tm l, today;
          localtime_r(&t, &l);
          const time_t now = time(nullptr);
          localtime_r(&now, &today);
          char buf[16];
          const bool sameDay = timeutil::sameDay(l, today);
          strftime(buf, sizeof buf, sameDay ? "%H:%M" : "%d %b", &l);
          synced = ui::upper(buf);
        }
      }
      out[3] = {a.busy ? "NETWORK" : "SYNCED", synced};
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
    case Kind::Recorder: {
      const MemoSummary memos = memoSummary();
      const audio::RecordStatus r = audio::recordStatus();
      out[0] = {"MEMOS", sdcard::mounted() ? std::to_string(memos.count) : "NO CARD"};
      out[1] = {"LAST", memos.last.empty() ? "--" : memos.last};
      out[2] = {"FORMAT", "16 kHz MONO WAV"};
      out[3] = {"MIC GAIN", std::to_string(r.gainDb) + " dB"};
      return 4;
    }
#if INKDECK_AGENT
    case Kind::Agent: {
      // What the last list said (a locked one, or one kept from before a power-off, says only
      // counts and times); until then, where. With the PIN on the Agent: counts and times only.
      const agent::Glance g = agent::glance();
      out[0] = {"NEEDS YOU", g.known ? std::to_string(g.asks + g.proposed) : "--"};
      out[1] = {"RUNNING", g.known ? std::to_string(g.running) : "--"};
      out[2] = {"NEXT", g.nextAt ? agentNext(g.nextAt) : g.known ? "NOTHING" : "--"};
      if (guardApplies(Guarded::Agent)) {
        out[3] = {"TALK", "HOLD BOOT"};
      } else {
        const agent::Sessions a = agent::sessions();
        out[3] = {"HOST", !a.place.empty()          ? a.place
                          : settings::hasAgent() ? settings::agentHost()
                                                 : "NOT SET"};
      }
      return 4;
    }
#endif
#if INKDECK_COMMS
    case Kind::Comms: {
      if (!comms::configured()) {
        out[0] = {"GROUP", "NOT SET"};
        out[1] = {"SET UP", "mesh_group"};  // a line in inkdeck.ini (the screen says how)
        return 2;
      }
      const std::vector<comms::Group> groups = comms::groups();
      const int a = comms::activeGroup();
      if (a < 0 || a >= static_cast<int>(groups.size())) return 0;
      const comms::Group &g = groups[a];
      const std::string dot = " \xC2\xB7 ";
      out[0] = {"GROUP", groups.size() > 1 ? g.name + dot + std::to_string(a + 1) + " of " +
                                                 std::to_string(groups.size())
                                           : g.name};
      std::string reached;
      if (g.via != comms::Via::Net) reached = std::to_string(g.onAir) + " air";
      if (g.via != comms::Via::Air) {
        reached += (reached.empty() ? "" : dot) + std::to_string(g.onNet) + " net";
      }
      out[1] = {"REACHED", reached};
      // The last call, ring or message: what it was in the key (MISSED RING, NEW MESSAGE),
      // who and when in the value.
      const std::vector<comms::Call> last = comms::calls(-1, 1);
      if (last.empty()) {
        out[2] = {"LAST", ui::glyph::kEmDash};
      } else {
        const comms::Call &c = last[0];
        std::string key = "LAST CALL";
        if (c.kind == comms::Kind::Ring) {
          key = !c.outgoing && c.missed ? "MISSED RING" : "LAST RING";
        } else if (c.kind == comms::Kind::Message) {
          key = !c.outgoing && !c.played && !c.live ? "NEW MESSAGE" : "LAST MESSAGE";
        }
        out[2] = {key, (c.outgoing ? std::string("YOU")
                                   : commsui::boardName(c.fromName, c.from)) +
                           dot + commsui::whenText(c.at, c.agoS)};
      }
      // The mics on: this board's, the others', a LOST one.
      out[3] = {"MICS", commsui::micsGlance()};
      return 4;
    }
#endif
  }
  return 0;
}

#if INKDECK_COMMS
// Calls, missed rings and messages not heard, over all groups: COMMS's "1 NEW".
int commsUnheard() {
  int n = 0;
  for (const comms::Group &g : comms::groups()) n += g.unheard;
  return n;
}
#endif

// The list's row for module i: its bus tag, or for the Agent while questions wait, an inverted
// "1 ASK"; for Comms with calls, rings or messages not heard, an inverted "1 NEW".
ui::Row moduleRow(int i) {
  ui::Row row;
  row.index = twoDigits(i + 1);
  row.title = kModules[i].label;
  row.tag = kModules[i].bus;
#if INKDECK_AGENT
  if (kModules[i].kind == Kind::Agent) {
    const agent::Glance g = agent::glance();
    if (g.known && g.asks > 0) {
      row.tag = std::to_string(g.asks) + " ASK";
      row.tagInverted = true;
    }
  }
#endif
#if INKDECK_COMMS
  if (kModules[i].kind == Kind::Comms) {
    const int n = commsUnheard();
    if (n > 0) {
      row.tag = std::to_string(std::min(n, 99)) + " NEW";
      row.tagInverted = true;
    }
  }
#endif
  return row;
}

class HomeScreen : public ui::Screen {
 public:
  HomeScreen() { cursor_.reset(kModuleCount); }

#if INKDECK_AGENT
  // Home coming up: what the agent is doing is worth a look (at most once a minute).
  void onEnter() override { agent::checkSoon(); }
#endif

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
    // A news sync starting or finishing.
    const bool n = news::busy();
    changed |= n != drawnNews_ && kind == Kind::News;
    drawnNews_ = n;
#if INKDECK_AGENT
    // The Agent's row tag and preview follow the list.
    const agent::Glance a = agent::glance();
    const uint32_t sig = static_cast<uint32_t>(a.asks) * 1000003u + a.running * 1009u +
                         a.proposed * 31u + a.nextAt;
    changed |= sig != drawnAgent_;
    drawnAgent_ = sig;
#endif
#if INKDECK_COMMS
    // The COMMS preview when it shows, and its row's "1 NEW" wherever the cursor: redrawn only
    // when what they say changes (comms::generation() changes several times a second while a
    // mic is on).
    const int unheard = commsUnheard();
    changed |= unheard != drawnCommsNew_;
    drawnCommsNew_ = unheard;
    if (kind == Kind::Comms) changed |= commsPreview() != drawnCommsPreview_;
#endif
    return changed;
  }

  void draw(ui::Canvas &c, const Rect &body) override {
    drawnGeneration_ = audio::status().generation;
    drawnWeather_ = weather::status().phase;
#if INKDECK_COMMS
    drawnCommsNew_ = commsUnheard();
    drawnCommsPreview_ = kModules[cursor_.cursor()].kind == Kind::Comms ? commsPreview() : "";
#endif
    Rect r = body;
    const Rect list = r.takeBottom(kModuleCount * ui::kRowH);
    const Rect header = r.takeBottom(ui::kSectionH);
    ui::hrule(c, r.x, r.bottom() - ui::kRule, r.w);
    r.h -= ui::kRule;

    drawPreview(c, r, cursor_.cursor());
    ui::drawSectionHeader(c, header, "MODULES", cursor_.label());
    ui::drawList(c, list, ui::kRowH, cursor_, moduleRow);
  }

 private:
  void open(int i) {
    switch (kModules[i].kind) {
      case Kind::Reader:
        if (auto reader = resumeLastBook()) {
          ui::nav::push(std::move(reader));
        } else {
          openBookLibrary();
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
      case Kind::Recorder: ui::nav::push(makeRecorder()); break;
      case Kind::News: ui::nav::push(makeNews()); break;
#if INKDECK_COMMS
      case Kind::Comms: ui::nav::push(makeComms()); break;
#endif
#if INKDECK_AGENT
      case Kind::Agent: openGuarded(Guarded::Agent, makeAgent); break;
#endif
    }
  }

  // Big index + bus tag + name, blurb, rule, 2x2 stats, then the HW line and OPEN button.
  static void drawPreview(ui::Canvas &c, const Rect &area, int i) {
    using namespace ui;
    const Module &m = kModules[i];
    // 10 px above and 8 below: with 11 modules in the list the preview is 34 px shorter than
    // the canvas drew it, and the stats would otherwise meet the OPEN button. With 12 (as now,
    // with COMMS and the agent) it's 68 px shorter, and the button goes.
    const Rect p{area.x + 20, area.y + 10, area.w - 40, area.h - 10 - 8};

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

    // With twelve modules the list leaves no room under the stats for the HW line and the OPEN
    // button (the hint bar says OPEN too).
    const bool buttonRow = p.bottom() - 44 >= y + 2 * 48;

    // READER: the last book's cover, framed, with a dithered shadow like the button's, as tall as
    // the space down to the button row (the bottom without one); the stats move right of it.
    Rect statsArea{p.x, y, p.w, 96};
    bool fitColumn = false;  // the right column only as wide as its stats need
    if (m.kind == Kind::Reader) {
      constexpr int kFrame = 2, kShadow = 4, kMaxW = 132, kGap = 16;
      const int bottom = buttonRow ? p.bottom() - 44 - 10 : p.bottom();
      const int maxH = bottom - y - kShadow - 2 * kFrame;
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
      fitColumn = true;  // PLACE and SD CARD are short; the title gets the room
    }

    Stat stats[4];
    const int n = statsFor(m, stats);
    constexpr int kColGap = 20;
    int rightW = (statsArea.w - kColGap) / 2;
    if (fitColumn) {
      int need = 0;
      for (int k = 1; k < n; k += 2) {
        need = std::max({need, textWidth(style::kKey, stats[k].key),
                         textWidth(style::kValue, stats[k].value)});
      }
      if (need > 0) rightW = std::min(rightW, need + 4);
    }
    const int leftW = statsArea.w - kColGap - rightW;
    for (int k = 0; k < n; ++k) {
      const bool right = k % 2 == 1;
      drawStat(c, {statsArea.x + (right ? leftW + kColGap : 0), y + (k / 2) * 48,
                   right ? rightW : leftW, 48},
               stats[k].key, stats[k].value);
    }

    if (buttonRow) {
      const Rect button = drawButton(c, p.right() - 6, p.bottom() - 44, glyph::kPress, "OPEN");
      drawText(c, {p.x, button.y, button.x - p.x - 14, button.h},
               std::string("HW ") + glyph::kPointer + " " + m.hw, style::kLabel);
    }
  }

  ui::ListCursor cursor_;
  uint32_t drawnGeneration_ = 0;
  weather::Phase drawnWeather_ = weather::Phase::Idle;
  bool drawnNews_ = false;
#if INKDECK_AGENT
  uint32_t drawnAgent_ = 0;
#endif
#if INKDECK_COMMS
  // What COMMS's preview says, as drawn.
  static std::string commsPreview() {
    Stat stats[4];
    const int n = statsFor(kModules[commsIndex()], stats);
    std::string k;
    for (int i = 0; i < n; ++i) k += stats[i].key + "=" + stats[i].value + "\n";
    return k;
  }
  static int commsIndex() {
    for (int i = 0; i < kModuleCount; ++i) {
      if (kModules[i].kind == Kind::Comms) return i;
    }
    return 0;
  }
  std::string drawnCommsPreview_;
  int drawnCommsNew_ = 0;
#endif
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
