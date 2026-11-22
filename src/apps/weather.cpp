#include "apps/weather.h"

#include <Arduino.h>
#include <math.h>
#include <stdio.h>

#include <algorithm>

#include "apps/home.h"
#include "rtc.h"
#include "sensor_hub.h"
#include "suntimes.h"
#include "timeutil.h"
#include "ui/icons.h"
#include "units.h"
#include "forecast.h"
#include "wlan.h"

namespace apps {
namespace {

using namespace ui;
using weather::Day;
using weather::Hour;

// Sizes and weights from the Weather artboard.
constexpr TextStyle kNowHeader{&fonts::kMono13Bold, 2};
constexpr TextStyle kBigTemp{&fonts::kMono130ExtraBold, -9};
constexpr TextStyle kCondition{&fonts::kMono28ExtraBold, 0};
constexpr TextStyle kHourTemp{&fonts::kMono15ExtraBold, 0};
constexpr TextStyle kHourRain{&fonts::kMono13Bold, 0};
constexpr TextStyle kHourLabel{&fonts::kMono13Medium, 0};
constexpr TextStyle kDayName{&fonts::kMono14ExtraBold, 0};
constexpr TextStyle kDayCondition{&fonts::kMono14Bold, 0};
constexpr TextStyle kDayLo{&fonts::kMono14Medium, 0};
constexpr TextStyle kDayHi{&fonts::kMono14ExtraBold, 0};

constexpr int kNowPadX = 20;
constexpr int kHourCount = 12;
constexpr int kHourlyBodyH = 150;  // temperature, bar box, rain chance, hour, with padding
constexpr int kBarBoxH = 64;       // includes the 2 px base line
constexpr int kDaysShown = 6;
constexpr int kDayRowH = 40;
constexpr int kHourlyH = kRule + kSectionH + kHourlyBodyH;
constexpr int kDaysH = kRule + kSectionH + kDaysShown * kDayRowH;

// The 6-day row's columns, left to right (the canvas's grid, with the temperatures 6 px wider
// so "−12°" and "102°" fit).
constexpr int kDayTagX = kPadX, kDayTagW = 48;
constexpr int kDayIconX = kDayTagX + kDayTagW + 10, kDayIconW = 28;
constexpr int kDayCondX = kDayIconX + kDayIconW + 10;
constexpr int kDayHiW = 36, kDayBarW = 112, kDayLoW = 36;

constexpr const char *kMinus = "\xE2\x88\x92";  // U+2212
constexpr const char *kDegree = "\xC2\xB0";
constexpr const char *kRise = "\xE2\x86\x91";  // ↑
constexpr const char *kSet = "\xE2\x86\x93";   // ↓

std::string format(const char *fmt, time_t t) {
  tm local;
  localtime_r(&t, &local);
  char buf[32];
  strftime(buf, sizeof buf, fmt, &local);
  return upper(buf);
}

int localHour(time_t t) {
  tm local;
  localtime_r(&t, &local);
  return local.tm_hour;
}


std::string percentText(uint8_t p) {
  return p == weather::kUnknown ? glyph::kEmDash : std::to_string(p);
}

std::string windText(float kmh, uint16_t degrees) {
  if (isnan(kmh)) return "--";
  return std::to_string(lroundf(units::windSpeed(kmh))) + " " + weather::compass(degrees);
}

icons::Weather iconFor(uint8_t code, bool day) {
  switch (weather::sky(code)) {
    case weather::Sky::Clear: return day ? icons::Weather::Sun : icons::Weather::Moon;
    case weather::Sky::Partly: return day ? icons::Weather::Partly : icons::Weather::PartlyNight;
    case weather::Sky::Cloud: return icons::Weather::Cloud;
    case weather::Sky::Fog: return icons::Weather::Fog;
    case weather::Sky::Showers: return icons::Weather::Showers;
    case weather::Sky::Rain: return icons::Weather::Rain;
    case weather::Sky::Snow: return icons::Weather::Snow;
    case weather::Sky::Storm: return icons::Weather::Storm;
  }
  return icons::Weather::Cloud;
}

// Day or night now, from the sun times worked out on the board; the forecast's own flag is
// for the start of its hour (or the fetch), so it lags at dawn and dusk.
bool isDay(const Hour &h, time_t t) {
  const sun::Day d = sun::on(t);
  if (d.alwaysUp || d.alwaysDown) return d.alwaysUp;
  return d.ok ? t >= d.rise && t < d.set : h.day;
}

// The next sunrise or sunset after t: "↓16:39".
std::string nextSunEvent(time_t t) {
  sun::Day d = sun::on(t);
  if (d.ok && t >= d.set) d = sun::on(t + 86400);
  if (!d.ok) return "--";
  return t < d.rise ? kRise + format("%H:%M", d.rise) : kSet + format("%H:%M", d.set);
}

// What the screen shows: a copy of the forecast, and the day under the cursor. Shared with the
// day sheet, which moves the same cursor.
struct View {
  weather::Forecast forecast;
  bool has = false;
  int day = 0;  // index into the days shown (0 = today)

  void reload() { has = weather::get(forecast); }

  int days(time_t t, const Day *out[kDaysShown]) const {
    return has && rtc::timeValid() ? weather::daysFrom(forecast, t, out, kDaysShown) : 0;
  }
};

// The selected day in detail: a sheet over the bottom of the screen.
//   ▲▼ day   ● or BOOT close
class DaySheet : public Screen {
 public:
  explicit DaySheet(std::shared_ptr<View> view) : view_(std::move(view)) {}

  std::string tag() const override { return {}; }
  bool overlay() const override { return true; }
  int hints(Hint *out) const override {
    out[0] = {glyph::kDial, "DAY"};
    out[1] = {"BOOT", "CLOSE"};  // ● closes it too
    return 2;
  }

  bool onKey(Key key) override {
    const Day *days[kDaysShown];
    const int n = view_->days(time(nullptr), days);
    switch (key) {
      case Key::Up:
      case Key::Down:
        if (n == 0) return false;
        view_->day = (view_->day + (key == Key::Up ? n - 1 : 1)) % n;
        return true;
      case Key::Select: nav::pop(); return true;
      default: return false;
    }
  }

  void draw(Canvas &c, const Rect &body) override {
    const time_t now = time(nullptr);
    const Day *days[kDaysShown];
    const int n = view_->days(now, days);
    if (n == 0) return;
    const int index = std::min(view_->day, n - 1);
    const Day &d = *days[index];

    // That day's hours, every three hours, as far as the forecast reaches.
    const Hour *hours[8];
    int hourCount = 0;
    const weather::Forecast &f = view_->forecast;
    for (int i = 0; i < f.hourCount && hourCount < 8; ++i) {
      const Hour &h = f.hours[i];
      if (h.time >= d.date && h.time < d.date + 86400 && localHour(h.time) % 3 == 0) {
        hours[hourCount++] = &h;
      }
    }

    constexpr int kStatH = 50;
    constexpr int kStripH = 72;
    const int height = kRule + kSectionH + 3 * kStatH + 6 +
                       (hourCount ? kRule + kSectionH + kStripH : 0);
    Rect sheet{body.x, body.bottom() - height, body.w, height};
    fill(c, sheet, kPaper);
    hrule(c, sheet.x, sheet.y, sheet.w);
    Rect r = sheet;
    r.takeTop(kRule);
    const std::string when = index == 0 ? "TODAY" : index == 1 ? "TOMORROW" : "";
    drawSectionHeader(c, r.takeTop(kSectionH), format("%a %d %b", d.date + 12 * 3600),
                      when.empty() ? std::to_string(index + 1) + "/" + std::to_string(n) : when);

    std::string rain = percentText(d.rain);
    if (d.rain != weather::kUnknown) rain += " %";
    if (d.precipMm >= 0.05f) {
      char amount[24];
      snprintf(amount, sizeof amount, " %s %.*f %s", glyph::kDot,
               units::precipitationDecimals(), units::precipitation(d.precipMm),
               units::precipitationUnit());
      rain += amount;
    }
    std::string sunText = "--", daylight = "--";
    if (d.sunrise && d.sunset > d.sunrise) {
      sunText = kRise + format("%H:%M", d.sunrise) + "  " + kSet + format("%H:%M", d.sunset);
      const long minutes = (d.sunset - d.sunrise) / 60;
      daylight = std::to_string(minutes / 60) + " H " + std::to_string(minutes % 60) + " M";
    }
    const std::string cells[6][2] = {
        {"SKY", weather::condition(d.code)},
        {"HIGH / LOW", temperatureText(d.hiC) + " / " + temperatureText(d.loC)},
        {"RAIN", rain},
        {"WIND", isnan(d.windMaxKmh) ? std::string("--")
                                     : "UP TO " + windText(d.windMaxKmh, d.windDeg)},
        {"SUN", sunText},
        {"DAYLIGHT", daylight},
    };
    const int colW = (r.w - 3 * kPadX) / 2;
    for (int i = 0; i < 6; ++i) {
      const Rect cell{r.x + kPadX + (i % 2) * (colW + kPadX), r.y + (i / 2) * kStatH, colW,
                      kStatH};
      drawStat(c, cell, cells[i][0], cells[i][1]);
    }
    r.takeTop(3 * kStatH + 6);

    if (hourCount == 0) return;
    hrule(c, r.x, r.y, r.w);
    r.takeTop(kRule);
    drawSectionHeader(c, r.takeTop(kSectionH), "HOURS",
                      std::string(units::temperatureUnit()) + " " + glyph::kDot + " RAIN %");
    const int gx = r.x + kPadX, gw = r.w - 2 * kPadX;
    for (int i = 0; i < hourCount; ++i) {
      const int x0 = gx + gw * i / 8, x1 = gx + gw * (i + 1) / 8;
      const Hour &h = *hours[i];
      drawText(c, {x0, r.y + 8, x1 - x0, 17}, format("%H", h.time), kHourLabel, Align::Center);
      drawText(c, {x0, r.y + 28, x1 - x0, 20}, temperatureText(h.tempC), kHourTemp,
               Align::Center);
      drawText(c, {x0, r.y + 50, x1 - x0, 17}, percentText(h.rain), kHourRain, Align::Center);
    }
  }

 private:
  std::shared_ptr<View> view_;
};

class WeatherScreen : public Screen {
 public:
  WeatherScreen() : view_(std::make_shared<View>()) {}

  std::string tag() const override { return moduleTag("WEATHER"); }
  std::string context() const override {
    const weather::Status s = weather::status();
    switch (s.phase) {
      case weather::Phase::Connecting: return "JOINING WI-FI\xE2\x80\xA6";
      case weather::Phase::Fetching: return "FETCHING THE FORECAST\xE2\x80\xA6";
      case weather::Phase::Failed:
        // Until a forecast newer than the failure arrives.
        if (s.at >= view_->forecast.fetched) return "SYNC FAILED: " + s.result;
        break;
      default: break;
    }
    return "LOC:HOME";
  }
  // The room, from the board's SHTC3, in whole degrees.
  std::string statusTemp() const override {
    const shtc3::Reading in = sensors::steadyEnv();
    if (!in.ok) return {};
    return "IN " + std::to_string(lroundf(units::temperature(in.tempC))) + units::temperatureUnit();
  }
  int hints(Hint *out) const override {
    out[0] = {glyph::kDial, "DAY"};
    out[1] = {glyph::kPress, "DETAIL"};
    out[2] = {glyph::kHoldPress, "SYNC"};
    out[3] = {"BOOT", "BACK"};
    return 4;
  }

  void onEnter() override {
    view_->reload();
    shownPhase_ = weather::status().phase;
    shownHour_ = time(nullptr) / 3600;
  }

  bool onKey(Key key) override {
    const Day *days[kDaysShown];
    const int n = view_->days(time(nullptr), days);
    switch (key) {
      case Key::Up:
      case Key::Down:
        if (n == 0) return false;
        view_->day = (std::min(view_->day, n - 1) + (key == Key::Up ? n - 1 : 1)) % n;
        return true;
      case Key::Select:
        if (n == 0) return false;
        nav::push(std::make_unique<DaySheet>(view_));
        return true;
      case Key::SelectLong:
        weather::start();  // a sync already running carries on
        shownPhase_ = weather::status().phase;
        return true;
      default: return false;
    }
  }

  bool onTick() override {
    bool redraw = false;
    const weather::Phase p = weather::status().phase;
    if (p != shownPhase_) {
      shownPhase_ = p;
      if (p == weather::Phase::Done) view_->reload();
      redraw = true;
    }
    const time_t hour = time(nullptr) / 3600;  // the next 12 hours move on
    if (hour != shownHour_) {
      shownHour_ = hour;
      redraw = true;
    }
    // Showing the room instead of a forecast: follow it.
    if (!forecastNow() && indoorText() != shownIndoor_) redraw = true;
    return redraw;
  }

  void draw(Canvas &c, const Rect &body) override {
    Rect r = body;
    const Rect days = r.takeBottom(kDaysH);
    const Rect hourly = r.takeBottom(kHourlyH);
    const time_t now = time(nullptr);
    drawNow(c, r, now);
    drawHourly(c, hourly, now);
    drawDays(c, days, now);
  }

 private:
  const Hour *forecastNow() const {
    return view_->has && rtc::timeValid() ? weather::at(view_->forecast, time(nullptr)) : nullptr;
  }

  // The room's temperature and humidity, "" without a reading.
  static std::string indoorText() {
    const shtc3::Reading in = sensors::env();
    if (!in.ok) return {};
    return temperatureText(in.tempC) + "|" + std::to_string(lroundf(in.rh)) + " %";
  }

  // Why there's nothing to show, in the condition's place.
  std::string emptyReason() const {
    if (!sun::configured()) return "SET LAT/LON IN .ENV";
    if (!wlan::configured()) return "SET WI-FI IN INKDECK.INI";
    if (weather::syncing()) return "SYNCING\xE2\x80\xA6";
    if (!rtc::timeValid()) return "CLOCK NOT SET";
    if (view_->has) return "FORECAST TOO OLD";
    return "HOLD \xE2\x97\x8F TO SYNC";
  }

  void drawNow(Canvas &c, const Rect &area, time_t now) {
    const weather::Forecast &f = view_->forecast;
    const Hour *h = forecastNow();
    // No forecast for now (none yet, too old, no Wi-Fi): the room's reading stands in.
    shownIndoor_ = h ? std::string() : indoorText();
    const bool indoor = !shownIndoor_.empty();
    const size_t bar = shownIndoor_.find('|');
    const int x = area.x + kNowPadX, w = area.w - 2 * kNowPadX;
    int y = area.y + 16;

    std::string left = indoor ? "// INSIDE (NO FORECAST)" : "// NOW";
    if (view_->has && h) {
      left += std::string(" ") + glyph::kDot + " UPDATED " +
              format(timeutil::sameLocalDay(f.fetched, now) ? "%H:%M" : "%a %H:%M", f.fetched);
    }
    std::string right;
    if (weather::syncing()) {
      right = "SYNCING\xE2\x80\xA6";
    } else if (const time_t next = weather::nextSync()) {
      right = "NEXT " + format("%H:%M", std::max(next, now));
    }
    const int rightW = right.empty() ? 0 : textWidth(kNowHeader, right) + 12;
    drawText(c, {x, y, w - rightW, 17}, left, kNowHeader);
    if (!right.empty()) drawText(c, {x, y, w, 17}, right, kNowHeader, Align::Right);
    y += 17 + 8;

    // The temperature (line-height 0.8 on the canvas: a 104 px row) and the icon, bottoms level.
    const std::string big = h        ? temperatureText(h->tempC)
                            : indoor ? shownIndoor_.substr(0, bar)
                                     : "--" + std::string(kDegree);
    drawText(c, x, y + 99, big, kBigTemp);
    if (h) icons::drawWeather(c, x + w - 100, y + 4, 100, weatherIcon(*h, now), 3);
    y += 104 + 8;

    drawText(c, {x, y, w, 37}, h ? weather::condition(h->code) : emptyReason(), kCondition);
    y += 37 + 8;

    dashedH(c, x, y, w);
    const std::string cells[4][2] = {
        {"FEELS", h ? temperatureText(h->feelsC) : "--"},
        {"WIND", h ? windText(h->windKmh, h->windDeg) : "--"},
        {"RH", h ? std::to_string(h->rh) + " %" : indoor ? shownIndoor_.substr(bar + 1) : "--"},
        {"SUN", sun::configured() && rtc::timeValid() ? nextSunEvent(now) : "--"},
    };
    for (int i = 0; i < 4; ++i) {
      drawStat(c, {x + w * i / 4, y + 1, w / 4, 52}, cells[i][0], cells[i][1]);
    }
  }

  void drawHourly(Canvas &c, const Rect &area, time_t now) {
    Rect r = area;
    hrule(c, r.x, r.y, r.w);
    r.takeTop(kRule);
    drawSectionHeader(c, r.takeTop(kSectionH), "NEXT 12 H",
                      std::string(units::temperatureUnit()) + " " + glyph::kDot + " RAIN %");
    const Hour *hours[kHourCount];
    const int n = view_->has && rtc::timeValid()
                      ? weather::hoursAfter(view_->forecast, now, hours, kHourCount)
                      : 0;
    const int gx = r.x + kPadX, gw = r.w - 2 * kPadX;
    const int top = r.y + 10;
    const int base = top + 20 + 4 + kBarBoxH;  // bottom of the bar box
    hrule(c, gx, base - 2, gw, 2);
    for (int i = 0; i < n; ++i) {
      const Hour &h = *hours[i];
      const int x0 = gx + gw * i / kHourCount, x1 = gx + gw * (i + 1) / kHourCount;
      const int cw = x1 - x0, cx = (x0 + x1) / 2;
      drawText(c, {x0, top, cw, 20}, temperatureText(h.tempC), kHourTemp, Align::Center);
      if (h.rain != weather::kUnknown && h.rain > 0) {
        // A dithered bar outlined on three sides, standing on the base line.
        const int bh = std::max(4, static_cast<int>(lroundf(h.rain * 0.6f)));
        const Rect bar{cx - 11, base - 2 - bh, 22, bh};
        dither50(c, {bar.x + 2, bar.y + 2, bar.w - 4, bar.h - 2});
        fill(c, {bar.x, bar.y, bar.w, 2}, kInk);
        fill(c, {bar.x, bar.y, 2, bar.h}, kInk);
        fill(c, {bar.right() - 2, bar.y, 2, bar.h}, kInk);
      }
      drawText(c, {x0, base + 4, cw, 17}, percentText(h.rain), kHourRain, Align::Center);
      drawText(c, {x0, base + 4 + 17 + 4, cw, 17}, format("%H", h.time), kHourLabel,
               Align::Center);
    }
  }

  void drawDays(Canvas &c, const Rect &area, time_t now) {
    Rect r = area;
    hrule(c, r.x, r.y, r.w);
    r.takeTop(kRule);
    const Rect header = r.takeTop(kSectionH);
    drawSectionHeader(c, header, "6 DAYS");
    // "LO ──── HI": the canvas's box-drawing line, drawn.
    {
      const int band = header.h - kRule;
      const int hiW = textWidth(style::kSection, "HI");
      const int hiX = header.right() - kPadX - hiW;
      drawText(c, {hiX, header.y, hiW, band}, "HI", style::kSection);
      const int lineX = hiX - 8 - 40;
      fill(c, {lineX, header.y + band / 2, 40, 2}, kInk);
      const int loW = textWidth(style::kSection, "LO");
      drawText(c, {lineX - 8 - loW, header.y, loW, band}, "LO", style::kSection);
    }

    const Day *days[kDaysShown];
    const int n = view_->days(now, days);
    if (n == 0) return;
    float lo = INFINITY, hi = -INFINITY;
    for (int i = 0; i < n; ++i) {
      if (!isnan(days[i]->loC)) lo = std::min(lo, days[i]->loC);
      if (!isnan(days[i]->hiC)) hi = std::max(hi, days[i]->hiC);
    }
    const float span = std::max(1.0f, hi - lo);
    const int hiX = r.right() - kPadX - kDayHiW;
    const int barX = hiX - 10 - kDayBarW;
    const int loX = barX - 10 - kDayLoW;
    const int cursor = std::min(view_->day, n - 1);
    for (int i = 0; i < n; ++i) {
      const Day &d = *days[i];
      const Rect row{r.x, r.y + i * kDayRowH, r.w, kDayRowH};
      const int cy = row.centerY();
      dashedH(c, row.x, row.bottom() - 1, row.w);
      const std::string name = format("%a", d.date + 12 * 3600);
      if (i == cursor) {
        fill(c, {kDayTagX, cy - 10, kDayTagW, 20}, kInk);
        drawText(c, {kDayTagX, cy - 10, kDayTagW, 20}, name, kDayName, Align::Center, kPaper);
      } else {
        drawText(c, {kDayTagX, cy - 10, kDayTagW, 20}, name, kDayName, Align::Center);
      }
      icons::drawWeather(c, kDayIconX, cy - kDayIconW / 2, kDayIconW, iconFor(d.code, true), 4);
      drawText(c, {kDayCondX, row.y, loX - 10 - kDayCondX, row.h}, weather::condition(d.code),
               kDayCondition);
      drawText(c, {loX, row.y, kDayLoW, row.h}, temperatureText(d.loC), kDayLo, Align::Right);
      dashedH(c, barX, cy, kDayBarW);
      if (!isnan(d.loC) && !isnan(d.hiC)) {
        const int l = static_cast<int>(lroundf((d.loC - lo) / span * kDayBarW));
        const int bw = std::max(4, static_cast<int>(lroundf((d.hiC - d.loC) / span * kDayBarW)));
        fill(c, {barX + std::min(l, kDayBarW - bw), cy - 5, bw, 10}, kInk);
      }
      drawText(c, {hiX, row.y, kDayHiW, row.h}, temperatureText(d.hiC), kDayHi);
    }
  }

  std::shared_ptr<View> view_;
  weather::Phase shownPhase_ = weather::Phase::Idle;
  time_t shownHour_ = 0;
  std::string shownIndoor_;  // the room reading drawn in place of a forecast, "" if none
};

}  // namespace

std::string temperatureText(float celsius) {
  if (isnan(celsius)) return std::string("--") + kDegree;
  const long v = lroundf(units::temperature(celsius));
  return (v < 0 ? kMinus + std::to_string(-v) : std::to_string(v)) + kDegree;
}

ui::icons::Weather weatherIcon(const Hour &h, time_t t) { return iconFor(h.code, isDay(h, t)); }

std::unique_ptr<ui::Screen> makeWeather() { return std::make_unique<WeatherScreen>(); }

}  // namespace apps
