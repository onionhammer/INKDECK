#include "apps/news_edition.h"

#include <Arduino.h>
#include <Preferences.h>
#include <math.h>
#include <time.h>

#include <algorithm>
#include <atomic>
#include <set>
#include <string>
#include <vector>

#include "alarms.h"
#include "apps/agent_face.h"
#include "apps/clock.h"
#include "apps/lock.h"
#include "apps/news.h"
#include "audio.h"
#include "board_pins.h"
#include "fonts.h"
#include "forecast.h"
#include "netget.h"
#include "news_library.h"
#include "notedoc.h"
#include "pmic.h"
#include "power.h"
#include "rtc.h"
#include "screen.h"
#include "sdcard.h"
#include "settings.h"
#include "timeutil.h"
#include "ui/nav.h"
#include "units.h"
#include "wlan.h"

namespace apps {
namespace {

using news::StoryInfo;
using ui::Align;
using ui::Canvas;
using ui::Key;
using ui::Rect;
using ui::TextStyle;
namespace glyph = ui::glyph;
namespace jb = ui::fonts;

constexpr const char *kNvs = "edition";
constexpr uint32_t kSyncMs = 90 * 1000;     // an edition's sync, at most
constexpr uint32_t kStopMs = 20 * 1000;     // then for the feed under way to finish
constexpr time_t kFullEveryS = 30 * 60;     // live on USB: a full refresh this often at least

// The Edition artboard.
constexpr int kBandH = 47;  // the foot band, its rule included
constexpr int kPad = 18;
constexpr int kMastH = 110;
constexpr int kAlsoRowH = 32;
constexpr int kAlsoRows = 3;
constexpr int kLeadLines = 3;     // of the headline, at most
constexpr int kSummaryLines = 3;
constexpr int kSummaryLineH = 26;  // Literata 16/26 (fonts::kReader[0])
constexpr TextStyle kLabel{&jb::kMono13Bold, 1};
constexpr TextStyle kMasthead{&jb::kMono68ExtraBold, -2};  // capitals only
constexpr TextStyle kLeadTitle{&jb::kMono28ExtraBold, 0};
constexpr int kLeadLineH = 37;
constexpr TextStyle kBrief{&jb::kMono15ExtraBold, 0};
constexpr int kBriefLineH = 20;
constexpr TextStyle kAlso{&jb::kMono14Medium, 0};
constexpr TextStyle kBig{&jb::kMono38ExtraBold, 0};

std::atomic<bool> requested{false};

uint32_t loadU32(const char *key) {
  Preferences prefs;
  if (!prefs.begin(kNvs, true)) return 0;  // read-only fails until the namespace exists
  const uint32_t v = prefs.getUInt(key, 0);
  prefs.end();
  return v;
}

void saveU32(const char *key, uint32_t v) {
  Preferences prefs;
  if (!prefs.begin(kNvs, false)) return;
  if (prefs.getUInt(key, ~v) != v) prefs.putUInt(key, v);
  prefs.end();
}

bool batteryLow() {
  const pmic::Status p = pmic::status();
  return p.ok && !p.vbus && p.percent >= 0 && p.percent < power::kLowBatteryPercent;
}

bool pwrHeld() { return digitalRead(board::PWR_KEY_SENSE) == HIGH; }

std::string dot() { return std::string(" ") + glyph::kDot + " "; }

std::string fmt(time_t t, const char *pattern) {
  tm l;
  localtime_r(&t, &l);
  char buf[32];
  strftime(buf, sizeof buf, pattern, &l);
  return ui::upper(buf);
}

// "05:12" today, "YDA" yesterday, else "18 NOV".
std::string shortWhen(time_t t) {
  const time_t now = time(nullptr);
  tm a, b, c;
  localtime_r(&t, &a);
  localtime_r(&now, &b);
  if (a.tm_year == b.tm_year && a.tm_yday == b.tm_yday) return fmt(t, "%H:%M");
  const time_t y = now - 86400;
  localtime_r(&y, &c);
  if (a.tm_year == c.tm_year && a.tm_yday == c.tm_yday) return "YDA";
  return fmt(t, "%d %b");
}

// The next edition after now (a minute on at least): today's time, else tomorrow's. 0 if none.
time_t nextEditionAt(time_t now) {
  const int minutes = settings::newsEdition();
  if (minutes < 0) return 0;
  tm today;
  localtime_r(&now, &today);
  time_t t = timeutil::localAt(today, minutes / 60, minutes % 60);
  if (t <= now + 60) {
    const time_t later = now + 86400;
    tm tomorrow;
    localtime_r(&later, &tomorrow);
    t = timeutil::localAt(tomorrow, minutes / 60, minutes % 60);
  }
  return t;
}

// "1° FOG · HI 7°" when the forecast reaches now; "" if it doesn't.
std::string weatherLine(time_t now) {
  if (!rtc::timeValid()) return {};
  weather::Forecast f;
  if (!weather::get(f)) return {};
  const weather::Hour *h = weather::at(f, now);
  const weather::Day *days[1] = {};
  const int n = weather::daysFrom(f, now, days, 1);
  std::string out;
  char buf[24];
  if (h) {
    const int t = static_cast<int>(lroundf(units::temperature(h->tempC)));
    snprintf(buf, sizeof buf, "%d\xC2\xB0 ", t);
    out = buf + std::string(weather::condition(h->code));
  }
  if (n > 0) {
    const int hi = static_cast<int>(lroundf(units::temperature(days[0]->hiC)));
    snprintf(buf, sizeof buf, "HI %d\xC2\xB0", hi);
    out += (out.empty() ? "" : dot()) + buf;
  }
  return out;
}

// --- What goes on the page -------------------------------------------------------------------

struct Edition {
  uint32_t number = 0;  // 0: not counted (a face before the first morning edition)
  time_t at = 0;
  bool card = true;
  int feeds = 0;
  int fresh = 0;
  int more = 0;  // new stories not on the page
  std::vector<StoryInfo> lead, briefs, also;
  std::string weather;
  std::string band;
};

Edition gather(const std::string &band, uint32_t number) {
  Edition e;
  e.at = time(nullptr);
  e.number = number;
  e.band = band;
  e.card = sdcard::mounted();
  e.feeds = news::feedCount();
  e.fresh = news::freshCount();
  e.weather = weatherLine(e.at);
  std::vector<StoryInfo> all = news::stories(-1, news::Show::All);
  // New stories first, then the rest; newest first in each.
  std::stable_partition(all.begin(), all.end(), [](const StoryInfo &s) { return s.fresh; });
  std::vector<bool> used(all.size(), false);
  // The lead: the first that has a summary to show under it.
  int lead = -1;
  for (int i = 0; i < static_cast<int>(all.size()) && i < 8 && lead < 0; ++i) {
    if (!all[i].summary.empty()) lead = i;
  }
  if (lead < 0 && !all.empty()) lead = 0;
  std::set<int> feedsOnPage;
  if (lead >= 0) {
    e.lead.push_back(all[lead]);
    used[lead] = true;
    feedsOnPage.insert(all[lead].feed);
  }
  // Four briefs, from feeds not on the page yet while there are some.
  for (int pass = 0; pass < 2 && e.briefs.size() < 4; ++pass) {
    for (int i = 0; i < static_cast<int>(all.size()) && e.briefs.size() < 4; ++i) {
      if (used[i] || (pass == 0 && feedsOnPage.count(all[i].feed))) continue;
      e.briefs.push_back(all[i]);
      used[i] = true;
      feedsOnPage.insert(all[i].feed);
    }
  }
  for (int i = 0; i < static_cast<int>(all.size()) && e.also.size() < kAlsoRows; ++i) {
    if (used[i]) continue;
    e.also.push_back(all[i]);
    used[i] = true;
  }
  int shown = 0;
  for (const auto *part : {&e.lead, &e.briefs, &e.also}) {
    for (const StoryInfo &s : *part) shown += s.fresh ? 1 : 0;
  }
  e.more = std::max(0, e.fresh - shown);
  return e;
}

// --- Drawing ---------------------------------------------------------------------------------

void drawMasthead(Canvas &c, const Rect &area, const Edition &e) {
  using namespace ui;
  const int x = area.x + kPad, right = area.right() - kPad;
  int y = area.y + 16;
  const std::string label =
      e.number ? "// EDITION NO. " + std::to_string(e.number) : std::string("// EDITION");
  const std::string date = rtc::timeValid() ? fmt(e.at, "%a %d %b %Y") : "";
  const int dateW = textWidth(kLabel, date);
  drawText(c, {right - dateW, y, dateW, 17}, date, kLabel);
  drawText(c, {x, y, right - x - dateW - 12, 17}, label, kLabel);
  y += 17 + 6;
  const int base = y + capHeight(kMasthead) + 2;
  const int nameW = drawText(c, x, base, "NEWS", kMasthead) - x;
  // On the right, bottom-aligned with the name: the edition's time, the counts, the weather.
  const int colX = x + nameW + 12, colW = right - colX;
  const int lineH = 17;
  int ly = base + 4 - 3 * lineH - 2;
  const std::string pill = rtc::timeValid() ? fmt(e.at, "%H:%M") + " EDITION" : "EDITION";
  const int pillW = tagWidth(pill, kLabel, 6);
  drawTag(c, right - pillW, ly + lineH / 2, 19, pill, kLabel, TagKind::Inverted, 6);
  ly += lineH + 2;
  std::string counts = std::to_string(e.fresh) + " NEW" + dot() + std::to_string(e.feeds) +
                       (e.feeds == 1 ? " FEED" : " FEEDS");
  drawText(c, {colX, ly, colW, lineH}, counts, kLabel, Align::Right);
  ly += lineH;
  if (!e.weather.empty()) drawText(c, {colX, ly, colW, lineH}, e.weather, kLabel, Align::Right);
  hrule(c, area.x, area.bottom() - kRule, area.w);
}

// The lead's height: its headline's lines and its summary's.
int leadHeight(const StoryInfo &s, int w) {
  const int lines =
      static_cast<int>(ui::wrap(kLeadTitle, ui::upper(s.title), w, kLeadLines).size());
  const int summary = s.summary.empty() ? 0 : 10 + kSummaryLines * kSummaryLineH;
  return 14 + 19 + 10 + lines * kLeadLineH + summary + 16 + ui::kRule;
}

void drawLead(Canvas &c, const Rect &area, const StoryInfo &s) {
  using namespace ui;
  const int x = area.x + kPad, w = area.w - 2 * kPad;
  int y = area.y + 14;
  const int tagW = drawTag(c, x, y + 9, 19, "LEAD", kLabel, TagKind::Inverted, 6);
  const std::string minutes = std::to_string(std::max<uint32_t>(1, (s.words + 100) / 200)) + " MIN";
  const int minW = textWidth(kLabel, minutes);
  drawText(c, {x + w - minW, y, minW, 19}, minutes, kLabel);
  drawText(c, {x + tagW + 8, y, w - tagW - 8 - minW - 12, 19},
           upper(s.feedTitle) + dot() + shortWhen(s.published), kLabel);
  y += 19 + 10;
  for (const std::string &line : wrap(kLeadTitle, upper(s.title), w, kLeadLines)) {
    drawText(c, x, y + (kLeadLineH + capHeight(kLeadTitle)) / 2 - 2, line, kLeadTitle);
    y += kLeadLineH;
  }
  if (!s.summary.empty()) {
    y += 10;
    NoteDoc doc;
    doc.parse("", s.summary);
    doc.layout(::fonts::kReader[0], w, kSummaryLines * kSummaryLineH);
    doc.drawPage(c, 0, x, y);
  }
  hrule(c, area.x, area.bottom() - kRule, area.w);
}

void drawBriefs(Canvas &c, const Rect &area, const std::vector<StoryInfo> &briefs) {
  using namespace ui;
  const int n = static_cast<int>(briefs.size());
  const int rows = (n + 1) / 2;
  const int colW = (area.w - kRule) / 2;
  const int rowH = area.h / rows;
  for (int i = 0; i < n; ++i) {
    const int col = i % 2, row = i / 2;
    const Rect cell{area.x + col * (colW + kRule), area.y + row * rowH, colW, rowH};
    if (row + 1 < rows) dashedH(c, cell.x, cell.bottom() - 1, cell.w);
    const Rect in = cell.inset(16, 12);
    const StoryInfo &s = briefs[i];
    drawText(c, {in.x, in.y, in.w, 17}, upper(s.feedTitle) + dot() + shortWhen(s.published),
             kLabel);
    const int lines = (in.h - 17 - 8) / kBriefLineH;
    int y = in.y + 17 + 8;
    if (lines > 0) {
      for (const std::string &line : wrap(kBrief, upper(s.title), in.w, lines)) {
        drawText(c, in.x, y + (kBriefLineH + capHeight(kBrief)) / 2 - 1, line, kBrief);
        y += kBriefLineH;
      }
    }
  }
  if (n > 1) vrule(c, area.x + colW, area.y, area.h);
}

void drawAlso(Canvas &c, const Rect &area, const Edition &e) {
  using namespace ui;
  hrule(c, area.x, area.y, area.w);
  Rect r{area.x, area.y + kRule, area.w, area.h - kRule};
  drawSectionHeader(c, r.takeTop(kSectionH), "ALSO",
                    e.more ? "+ " + std::to_string(e.more) + " MORE" : std::string());
  for (size_t i = 0; i < e.also.size(); ++i) {
    const Rect row = r.takeTop(kAlsoRowH);
    if (i + 1 < e.also.size()) dashedH(c, row.x, row.bottom() - 1, row.w);
    const std::string feed = upper(e.also[i].feedTitle);
    const int feedW = std::min(textWidth(kLabel, feed), row.w / 3);
    drawText(c, {row.right() - kPad - feedW, row.y, feedW, row.h}, feed, kLabel, Align::Right);
    drawText(c, {row.x + kPad, row.y, row.w - 2 * kPad - feedW - 12, row.h}, e.also[i].title,
             kAlso);
  }
}

// No stories: why, large, and what to do.
void drawNothing(Canvas &c, const Rect &area, const Edition &e) {
  using namespace ui;
  const char *big = !e.card ? "NO CARD" : e.feeds == 0 ? "NO FEEDS" : "NOTHING YET";
  const char *small = !e.card    ? "NEWS KEEPS ITS FEEDS ON THE MICROSD CARD"
                      : e.feeds == 0 ? "ADD FEEDS TO SD:/NEWS/FEEDS.TXT"
                                     : "OPEN NEWS TO SYNC THE FEEDS";
  const int base = area.y + area.h / 2;
  drawText(c, {area.x + kPad, base - 50, area.w - 2 * kPad, 50}, big, kBig, Align::Center);
  drawText(c, {area.x + kPad, base + 10, area.w - 2 * kPad, 17}, small, kLabel, Align::Center);
}

void drawEdition(Canvas &c, const Edition &e) {
  Rect r{0, 0, c.width(), c.height()};
  ui::drawResumeBand(c, r.takeBottom(kBandH), e.band);
  drawMasthead(c, r.takeTop(kMastH), e);
  if (e.lead.empty()) {
    drawNothing(c, r, e);
    return;
  }
  drawLead(c, r.takeTop(leadHeight(e.lead[0], r.w - 2 * kPad)), e.lead[0]);
  Rect also;
  if (!e.also.empty()) {
    also = r.takeBottom(ui::kRule + ui::kSectionH + static_cast<int>(e.also.size()) * kAlsoRowH);
  }
  if (!e.briefs.empty()) drawBriefs(c, r, e.briefs);
  if (!e.also.empty()) drawAlso(c, also, e);
}

// "NEXT EDITION SUN 06:30", "PAUSED · BATTERY LOW", "LIVE · SYNCED 14:20"...
std::string bandText(bool usb, time_t wakeAt) {
  const time_t synced = news::lastSync();
  if (usb) return "LIVE" + (synced ? dot() + "SYNCED " + shortWhen(synced) : std::string());
  if (batteryLow()) return "PAUSED" + dot() + "BATTERY LOW";
  if (wakeAt) return "NEXT EDITION " + fmt(wakeAt, "%a %H:%M");
  return synced ? "SYNCED " + shortWhen(synced) : std::string();
}

// Draws an edition straight to the panel, with a full refresh (the RTC wake's path).
void presentNow(const Edition &e) {
  Canvas &c = screen::canvas();
  screen::lock();
  screen::setLandscape(false);
  screen::clear();
  drawEdition(c, e);
  screen::sleepWhileBusy(!pmic::status().vbus);
  screen::present(true);
  screen::sleepWhileBusy(false);
  screen::unlock();
}

// --- The face, awake -------------------------------------------------------------------------

class EditionScreen;
EditionScreen *face = nullptr;

class EditionScreen : public ui::Screen {
 public:
  EditionScreen(bool usb, bool devWake) : usb_(usb), devWake_(devWake) {
    face = this;
    plan();
  }
  ~EditionScreen() override {
    if (face == this) face = nullptr;
  }

  std::string tag() const override { return "EDITION"; }
  bool chrome() const override { return false; }
  bool offFace() const override { return true; }
  bool onKey(Key) override {  // a key: awake again (before the power-off, or on USB)
    ui::nav::pop();
    return true;
  }

  bool onTick() override {
    if (usb_) {
      const pmic::Status p = pmic::status();
      if (p.ok && !p.vbus) {  // unplugged: off behind it, as on battery
        usb_ = false;
        plan();
        return true;
      }
      // Live: a new page once a sync has brought something.
      if (news::busy() || news::generation() == drawnGen_) return false;
      if (time(nullptr) - lastFull_ > kFullEveryS) {
        lastFull_ = time(nullptr);
        ui::nav::invalidate(ui::Refresh::Full);
      }
      return true;
    }
    // On battery: the second tick after it's drawn, off.
    if (!drawn_ || ++ticks_ < 2) return false;
    const time_t now = time(nullptr);
    const time_t alarm = rtc::timeValid() ? alarms::next(now) : 0;
    saveU32("next", static_cast<uint32_t>(wakeAt_));
    if (devWake_) saveU32("dev", 1);  // the dev tool's: one edition, then the board stays on
    power::off(power::Mode::Edition, earliestWake(earliestWake(alarm, wakeAt_), agentCheckWake()));
    ui::nav::pop();  // still here: the power-off failed
    return true;
  }

  void draw(Canvas &c, const Rect &) override {
    drawnGen_ = news::generation();
    drawEdition(c, gather(bandText(usb_, wakeAt_), loadU32("num")));
    drawn_ = true;
    if (!lastFull_) lastFull_ = time(nullptr);
  }

 private:
  void plan() {
    ticks_ = 0;
    const time_t now = time(nullptr);
    if (usb_ || batteryLow() || !rtc::timeValid()) {
      wakeAt_ = 0;
    } else {
      wakeAt_ = devWake_ ? now + 60 : nextEditionAt(now);
    }
  }

  bool usb_;
  bool devWake_;
  time_t wakeAt_ = 0;
  uint32_t drawnGen_ = 0;
  bool drawn_ = false;
  int ticks_ = 0;
  time_t lastFull_ = 0;
};

}  // namespace

bool editionWanted() { return newsOnScreen(); }

void showEdition(bool usb, bool devWake) {
  const ui::Screen *top = ui::nav::top();
  if ((top && top->offFace()) || deskShowing() || face) return;
  if (audio::status().state == audio::State::Playing) audio::togglePause();  // "off" is quiet
  lockNow();  // under the face: waking it lands on the lock (power_on)
  ui::nav::push(std::make_unique<EditionScreen>(usb, devWake));
  ui::nav::invalidate(ui::Refresh::Full);
}

bool wakeEdition() {
  if (!face || ui::nav::base() != face) return false;
  while (ui::nav::top() != face) ui::nav::pop();
  ui::nav::pop();
  return true;
}

bool editionShowing() { return face != nullptr; }

time_t editionWakeAt() { return static_cast<time_t>(loadU32("next")); }

bool editionWake() {
  if (power::wake() != power::Wake::Alarm || !rtc::timeValid()) return false;
  if (power::lastMode() != power::Mode::Edition) return false;
  const time_t now = time(nullptr);
  const time_t armed = power::armedFor();
  const time_t ring = alarms::next(armed ? armed - 1 : now - 60);
  if (ring && ring <= now + 1) return false;  // an alarm: boot, and it rings
  const time_t due = static_cast<time_t>(loadU32("next"));
  if (due == 0 || now + 30 < due) {
    // Woken for something else (an alarm snoozed or deleted since): back off until it's time.
    power::off(power::Mode::Edition, earliestWake(earliestWake(ring, due), agentCheckWake()));
    return false;
  }
  pinMode(board::PWR_KEY_SENSE, INPUT);
  sdcard::mount();
  news::load(false);
  uint32_t number = loadU32("num");
  if (batteryLow()) {
    log_i("edition: battery low, no sync and no more wakes");
    saveU32("next", 0);
    presentNow(gather(bandText(false, 0), number));
    power::off(power::Mode::Edition, ring);
    return false;
  }
  if (news::hasFeeds() && wlan::configured()) {
    net::begin();
    news::sync();
    const uint32_t start = millis();
    bool stopping = false;
    while (news::busy()) {
      if (pwrHeld()) return true;  // PWR: boot into News (the sync finishes in the background)
      const uint32_t took = millis() - start;
      if (!stopping && took > kSyncMs) {
        news::cancel();
        stopping = true;
      }
      if (took > kSyncMs + kStopMs) break;
      delay(100);
    }
    log_i("edition: synced in %lu ms (%s)", static_cast<unsigned long>(millis() - start),
          news::activity().text.c_str());
  }
  if (pwrHeld()) return true;
  saveU32("num", ++number);
  const bool dev = loadU32("dev") != 0;
  const time_t next = dev ? 0 : nextEditionAt(time(nullptr));
  presentNow(gather(bandText(false, next), number));
  if (dev) {
    saveU32("dev", 0);
    saveU32("next", 0);
    log_i("edition: dev wake done, staying on");
    return false;
  }
  saveU32("next", static_cast<uint32_t>(next));
  log_i("edition: no. %lu drawn, next %ld", static_cast<unsigned long>(number),
        static_cast<long>(next));
  power::off(power::Mode::Edition, earliestWake(earliestWake(ring, next), agentCheckWake()));
  return false;
}

void requestEdition() { requested = true; }

void pollEdition() {
  if (requested.exchange(false)) showEdition(false, true);
}

}  // namespace apps
