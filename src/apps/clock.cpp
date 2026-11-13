#include "apps/clock.h"

#include <Arduino.h>
#include <Preferences.h>
#include <stdio.h>
#include <time.h>


#include "alarms.h"
#include "apps/sleep.h"
#include "audio.h"
#include "pmic.h"
#include "power.h"
#include "rtc.h"
#include "screen.h"
#include "suntimes.h"
#include "timesync.h"
#include "timeutil.h"

namespace apps {
namespace {

using ui::Key;
using ui::Rect;
using ui::TextStyle;

// Sizes, weights and letter-spacing from the Clock artboard.
constexpr TextStyle kHead{&ui::fonts::kMono13Bold, 2};         // "// LOCAL TIME", "WK 46"
constexpr TextStyle kTime{&ui::fonts::kMono172ExtraBold, -9};  // the artboard's 176 px
constexpr TextStyle kWeekday{&ui::fonts::kMono24ExtraBold, 0};
constexpr TextStyle kDate{&ui::fonts::kMono20Medium, 0};
constexpr TextStyle kBarLabel{&ui::fonts::kMono13Bold, 1};    // "DAY ▸ 60.6%", sun times
constexpr TextStyle kHourLabel{&ui::fonts::kMono13Medium, 0};  // 00 06 12 18 24
constexpr TextStyle kCalHead{&ui::fonts::kMono13ExtraBold, 0};
constexpr TextStyle kCalDay{&ui::fonts::kMono13Bold, 0};
constexpr TextStyle kAlarmTime{&ui::fonts::kMono24ExtraBold, 0};
constexpr TextStyle kAlarmDays{&ui::fonts::kMono13Medium, 0};
constexpr TextStyle kPill{&ui::fonts::kMono12Bold, 1};
// The editor (not on the canvas; built from the same parts).
constexpr TextStyle kEditTime{&ui::fonts::kMono64ExtraBold, -3};
constexpr TextStyle kEditKey{&ui::fonts::kMono13Medium, 1};
constexpr TextStyle kEditValue{&ui::fonts::kMono17Bold, 0};
constexpr TextStyle kEditValueSelected{&ui::fonts::kMono17ExtraBold, 0};

// Geometry, in body coordinates (the body is 800 x 410 between the chrome bands).
constexpr int kAsideW = 272;  // right column, including its 3 px left rule
constexpr int kMainPadX = 24;
constexpr int kMainPadTop = 20;
constexpr int kMainPadBottom = 18;
constexpr int kLabelH = 17;       // one 13 px line
constexpr int kTimeCapTop = 65;   // where the artboard's digits start, from the body top
constexpr int kDateBaseline = 238;
constexpr int kCellH = 22;
constexpr int kHours = 24;
constexpr int kCalPadTop = 8;
constexpr int kCalPadBottom = 10;
constexpr int kCalRowH = 22;
constexpr int kCalGap = 2;
constexpr int kCalWeeks = 6;  // always room for six, so the alarms don't move month to month
constexpr int kCalH = kCalPadTop + kCalRowH + kCalWeeks * (kCalGap + kCalRowH) + kCalPadBottom;
constexpr int kAsidePadX = 14;
constexpr int kMarkerW = 14;  // ▶ column in the alarm rows
constexpr int kPillH = 22;

const char *const kWeekdayNames[] = {"SUNDAY",   "MONDAY", "TUESDAY", "WEDNESDAY",
                                     "THURSDAY", "FRIDAY", "SATURDAY"};
const char *const kWeekdayShort[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
const char *const kMonthNames[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                   "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};

std::string hhmm(int hour, int minute) {
  char buf[8];
  snprintf(buf, sizeof buf, "%02d:%02d", hour, minute);
  return buf;
}

std::string hhmm(time_t t) {
  tm l;
  localtime_r(&t, &l);
  return hhmm(l.tm_hour, l.tm_min);
}

bool sameDay(const tm &a, const tm &b) {
  return a.tm_year == b.tm_year && a.tm_yday == b.tm_yday;
}

// "07:00" today or within a day, else "TUE 07:00".
std::string whenText(time_t t, time_t now) {
  if (t - now < 86400) return hhmm(t);
  tm l;
  localtime_r(&t, &l);
  return std::string(kWeekdayShort[l.tm_wday]) + " " + hhmm(l.tm_hour, l.tm_min);
}

// The days column of an alarm row: the repeat, or for a one-off that's on, when it rings.
std::string daysText(const alarms::Alarm &a, time_t now, bool valid) {
  if (a.days != alarms::kOnce || !a.on || !valid) return alarms::repeatLabel(a.days);
  const time_t ring = alarms::ringAfter(a, now);
  tm r, n;
  localtime_r(&ring, &r);
  localtime_r(&now, &n);
  return sameDay(r, n) ? "today" : "tomorrow";
}

Rect asideOf(const Rect &body) { return {body.right() - kAsideW, body.y, kAsideW, body.h}; }

// The right column's inside, after its rule.
Rect asideInner(const Rect &body) {
  const Rect a = asideOf(body);
  return {a.x + ui::kRule, a.y, a.w - ui::kRule, a.h};
}

// --- The face -------------------------------------------------------------------------------

// Everything the clock face shows, read once. The standby face is kept in NVS, so the next
// wake can draw it again as the panel's "old" image for a partial refresh.
struct Face {
  bool valid = false;  // the clock is set
  time_t now = 0;      // the minute shown (seconds dropped, so a redraw comes out the same)
  int battery = -1;
  bool usb = false;
  alarms::Alarm alarm[alarms::kCount];
  time_t snoozeAt = 0;
  time_t lastSync = 0;
  int cursor = -1;  // selected alarm row; -1 in standby
};

Face liveFace(int cursor) {
  Face f;
  f.valid = rtc::timeValid();
  f.now = time(nullptr) / 60 * 60;
  const pmic::Status p = pmic::status();
  f.battery = p.percent;
  f.usb = p.vbus;
  for (int i = 0; i < alarms::kCount; ++i) f.alarm[i] = alarms::get(i);
  f.snoozeAt = alarms::snoozedUntil();
  f.lastSync = timesync::lastSync();
  f.cursor = cursor;
  return f;
}

// NVS record of the last standby face; bump kFaceVersion when Face or the drawing changes.
constexpr uint8_t kFaceVersion = 1;
struct FaceRecord {
  uint8_t version, valid, usb;
  int8_t battery, cursor;
  uint8_t alarm[alarms::kCount][4];
  uint32_t now, snoozeAt, lastSync;
};

void saveFace(const Face &f) {
  FaceRecord r = {};
  r.version = kFaceVersion;
  r.valid = f.valid;
  r.usb = f.usb;
  r.battery = static_cast<int8_t>(f.battery);
  r.cursor = static_cast<int8_t>(f.cursor);
  for (int i = 0; i < alarms::kCount; ++i) {
    const alarms::Alarm &a = f.alarm[i];
    r.alarm[i][0] = a.hour;
    r.alarm[i][1] = a.minute;
    r.alarm[i][2] = a.days;
    r.alarm[i][3] = a.on;
  }
  r.now = static_cast<uint32_t>(f.now);
  r.snoozeAt = static_cast<uint32_t>(f.snoozeAt);
  r.lastSync = static_cast<uint32_t>(f.lastSync);
  Preferences prefs;
  if (!prefs.begin("clockface", false)) return;
  prefs.putBytes("face", &r, sizeof r);
  prefs.end();
}

bool loadFace(Face &f) {
  FaceRecord r;
  Preferences prefs;
  if (!prefs.begin("clockface", false)) return false;
  const bool ok = prefs.isKey("face") && prefs.getBytes("face", &r, sizeof r) == sizeof r &&
                  r.version == kFaceVersion;
  prefs.end();
  if (!ok) return false;
  f.valid = r.valid;
  f.usb = r.usb;
  f.battery = r.battery;
  f.cursor = r.cursor;
  for (int i = 0; i < alarms::kCount; ++i) {
    f.alarm[i] = {r.alarm[i][0], r.alarm[i][1], r.alarm[i][2], r.alarm[i][3] != 0};
  }
  f.now = r.now;
  f.snoozeAt = r.snoozeAt;
  f.lastSync = r.lastSync;
  return true;
}

// The soonest ring among the face's alarms (and its snooze) after `after`; 0 if none.
time_t nextRing(const Face &f, time_t after) {
  time_t best = f.snoozeAt > after ? f.snoozeAt : 0;
  for (const alarms::Alarm &a : f.alarm) {
    if (!a.on) continue;
    const time_t t = alarms::ringAfter(a, after);
    if (t && (!best || t < best)) best = t;
  }
  return best;
}

// Status bar context after "RTC PCF85063A · ": when NTP last set the clock.
std::string syncText(const Face &f) {
  if (!f.valid) return "TIME NOT SET";
  if (!f.lastSync) return "NOT SYNCED";
  tm s, n;
  localtime_r(&f.lastSync, &s);
  localtime_r(&f.now, &n);
  if (sameDay(s, n)) return "NTP SYNC " + hhmm(s.tm_hour, s.tm_min);
  char buf[24];
  snprintf(buf, sizeof buf, "NTP SYNC %d %s", s.tm_mday, kMonthNames[s.tm_mon]);
  return buf;
}

std::string sunText(const Face &f) {
  if (!f.valid || !sun::configured()) return "";
  const sun::Day d = sun::on(f.now);
  if (d.alwaysUp) return "SUN UP ALL DAY";
  if (d.alwaysDown) return "NO SUNRISE";
  if (!d.ok) return "";
  return std::string("\xE2\x86\x91") + hhmm(d.rise) + " \xE2\x86\x93" + hhmm(d.set);  // ↑ ↓
}

void drawAlarmRow(ui::Canvas &c, const Rect &row, const alarms::Alarm &a,
                  const std::string &days, bool selected, bool last) {
  using namespace ui;
  const uint16_t ink = selected ? kPaper : kInk;
  if (selected) {
    fill(c, row, kInk);
  } else if (!last) {
    dashedH(c, row.x, row.bottom() - 1, row.w);
  }
  int x = row.x + kAsidePadX;
  if (selected) {
    drawText(c, {x, row.y, kMarkerW, row.h}, glyph::kPlay, kCalHead, Align::Left, ink);
  }
  x += kMarkerW + 6;
  const int timeEnd =
      drawText(c, x, baselineCentered(kAlarmTime, row.y, row.h), hhmm(a.hour, a.minute),
               kAlarmTime, ink);
  const std::string pill = a.on ? "ON" : "OFF";
  const int pillX = row.right() - kAsidePadX - tagWidth(pill, kPill, 6);
  drawTag(c, pillX, row.centerY(), kPillH, pill, kPill,
          a.on ? TagKind::Inverted : TagKind::Outlined, 6, ink);
  drawText(c, {timeEnd + 10, row.y, pillX - 10 - timeEnd - 10, row.h}, days, kAlarmDays,
           Align::Left, ink);
}

// Left column: header, the time, weekday and date, the day bar.
void drawMain(ui::Canvas &c, const Rect &area, const Face &f) {
  using namespace ui;
  tm t;
  localtime_r(&f.now, &t);
  const int x = area.x + kMainPadX;
  const int w = area.w - 2 * kMainPadX;

  // "// LOCAL TIME" and the ISO week / day of the year.
  const Rect head{x, area.y + kMainPadTop, w, kLabelH};
  drawText(c, head, "// LOCAL TIME", kHead);
  std::string weekDay = "WK -- \xC2\xB7 DAY ---";
  if (f.valid) {
    char iso[4];
    strftime(iso, sizeof iso, "%V", &t);
    weekDay = "WK " + std::to_string(atoi(iso)) + " \xC2\xB7 DAY " +
              std::to_string(t.tm_yday + 1);
  }
  drawText(c, head, weekDay, kHead, Align::Right);

  // The time, then weekday and date on one baseline.
  const int timeBase = area.y + kTimeCapTop + capHeight(kTime);
  drawText(c, x, timeBase, f.valid ? hhmm(t.tm_hour, t.tm_min) : "--:--", kTime);
  const int dateBase = area.y + kDateBaseline;
  if (f.valid) {
    const int after = drawText(c, x, dateBase, kWeekdayNames[t.tm_wday], kWeekday);
    char date[24];
    snprintf(date, sizeof date, "%d %s %d", t.tm_mday, kMonthNames[t.tm_mon], t.tm_year + 1900);
    drawText(c, after + 12, dateBase, date, kDate);
  } else {
    const int after = drawText(c, x, dateBase, "TIME NOT SET", kWeekday);
    drawText(c, after + 12, dateBase, std::string("SYSTEM ") + glyph::kPointer + " SYNC TIME",
             kDate);
  }

  // Day bar, from the bottom up: hour labels, the 24 cells, then progress and sun times.
  const int bottom = area.bottom() - kMainPadBottom;
  const Rect hoursRow{x, bottom - kLabelH, w, kLabelH};
  const int cellsTop = hoursRow.y - 6 - kCellH;
  const Rect labelRow{x, cellsTop - 6 - kLabelH, w, kLabelH};

  const char *const marks[] = {"00", "06", "12", "18", "24"};
  const int markW = textWidth(kHourLabel, "00");
  for (int i = 0; i < 5; ++i) {
    drawText(c, {x + i * (w - markW) / 4, hoursRow.y, markW, hoursRow.h}, marks[i], kHourLabel);
  }

  for (int i = 0; i < kHours; ++i) {
    const int x0 = x + i * (w + 2) / kHours;
    const int x1 = x + (i + 1) * (w + 2) / kHours - 2;
    const Rect cell{x0, cellsTop, x1 - x0, kCellH};
    if (f.valid && i < t.tm_hour) {
      fill(c, cell, kInk);
    } else {
      if (f.valid && i == t.tm_hour) dither50(c, cell);
      frame(c, cell, 2);
    }
  }

  std::string progress = std::string("DAY ") + glyph::kPointer + " --";
  if (f.valid) {
    char buf[32];
    snprintf(buf, sizeof buf, "DAY %s %.1f%%", glyph::kPointer,
             (t.tm_hour * 60 + t.tm_min) / 14.4);
    progress = buf;
  }
  drawText(c, labelRow, progress, kBarLabel);
  drawText(c, labelRow, sunText(f), kBarLabel, Align::Right);
}

// Right column: the month calendar and the alarms.
void drawAside(ui::Canvas &c, const Rect &body, const Face &f) {
  using namespace ui;
  tm t;
  localtime_r(&f.now, &t);
  const Rect outer = asideOf(body);
  vrule(c, outer.x, outer.y, outer.h);
  const Rect inner = asideInner(body);

  // Month calendar, Monday first, today inverted.
  char month[16] = "CALENDAR";
  if (f.valid) snprintf(month, sizeof month, "%s %d", kMonthNames[t.tm_mon], t.tm_year + 1900);
  drawSectionHeader(c, {inner.x, inner.y, inner.w, kSectionH}, month,
                    "MON\xE2\x80\x93SUN");  // en dash
  const int gx = inner.x + kAsidePadX;
  const int gw = inner.w - 2 * kAsidePadX;
  const auto colX = [&](int col) { return gx + col * gw / 7; };
  const auto colW = [&](int col) { return colX(col + 1) - colX(col); };
  int y = inner.y + kSectionH + kCalPadTop;
  const char *const letters[] = {"M", "T", "W", "T", "F", "S", "S"};
  for (int col = 0; col < 7; ++col) {
    drawText(c, {colX(col), y, colW(col), kCalRowH}, letters[col], kCalHead, Align::Center);
  }
  dashedH(c, gx, y + kCalRowH - 1, gw);
  y += kCalRowH + kCalGap;
  if (f.valid) {
    const int lead = (t.tm_wday - (t.tm_mday - 1) % 7 + 13) % 7;  // Monday index of the 1st
    const int days = timeutil::daysInMonth(t.tm_year + 1900, t.tm_mon + 1);
    for (int week = 0; week < kCalWeeks; ++week) {
      const int rowY = y + week * (kCalRowH + kCalGap);
      for (int col = 0; col < 7; ++col) {
        const int day = week * 7 + col - lead + 1;
        if (day < 1 || day > days) continue;
        const Rect cell{colX(col), rowY, colW(col), kCalRowH};
        const bool today = day == t.tm_mday;
        if (today) fill(c, cell, kInk);
        drawText(c, cell, std::to_string(day), today ? kCalHead : kCalDay, Align::Center,
                 today ? kPaper : kInk);
      }
    }
  }

  // Alarms: header with the next ring, then one row per slot.
  const int alarmsTop = inner.y + kSectionH + kCalH;
  hrule(c, inner.x, alarmsTop, inner.w);
  const time_t next = f.valid ? nextRing(f, f.now) : 0;
  const std::string nextLabel = !f.valid ? "" : next ? "NEXT " + whenText(next, f.now) : "ALL OFF";
  drawSectionHeader(c, {inner.x, alarmsTop + kRule, inner.w, kSectionH - kRule}, "ALARMS",
                    nextLabel);
  const int rowsTop = alarmsTop + kSectionH;
  const int rowsH = inner.bottom() - rowsTop;
  for (int i = 0; i < alarms::kCount; ++i) {
    const int y0 = rowsTop + i * rowsH / alarms::kCount;
    const int y1 = rowsTop + (i + 1) * rowsH / alarms::kCount;
    drawAlarmRow(c, {inner.x, y0, inner.w, y1 - y0}, f.alarm[i],
                 daysText(f.alarm[i], f.now, f.valid), i == f.cursor, i + 1 == alarms::kCount);
  }
}

// The body between the chrome bands.
void drawFace(ui::Canvas &c, const Rect &body, const Face &f) {
  drawMain(c, {body.x, body.y, body.w - kAsideW, body.h}, f);
  drawAside(c, body, f);
}

// The whole standby frame: chrome drawn here, since no screen is running it.
void drawStandbyFace(ui::Canvas &c, const Face &f) {
  Rect r{0, 0, c.width(), c.height()};
  ui::StatusInfo info;
  info.tag = "06 CLOCK";
  info.context = "STANDBY \xC2\xB7 " + syncText(f);
  info.battery = f.battery;
  info.usb = f.usb;
  ui::drawStatusBar(c, r.takeTop(ui::kStatusBarH), info);
  const ui::Hint hint[] = {{"PWR", "HOLD 1 S TO WAKE"}};
  ui::drawHintBar(c, r.takeBottom(ui::kHintBarH), hint, 1);
  drawFace(c, r, f);
}

// --- Alarm editor ---------------------------------------------------------------------------

class AlarmEditor : public ui::Screen {
 public:
  explicit AlarmEditor(int slot) : slot_(slot), draft_(alarms::get(slot)) {
    draft_.on = true;  // editing an alarm means wanting it; the last field can turn it off
  }

  std::string tag() const override { return {}; }
  bool overlay() const override { return true; }
  int hints(ui::Hint *out) const override {
    out[0] = {ui::glyph::kDial, "SET"};
    out[1] = {ui::glyph::kPress, field_ == Field::On ? "SAVE" : "NEXT"};
    out[2] = {ui::glyph::kHoldPress, "SAVE"};
    out[3] = {"BOOT", "CANCEL"};
    return 4;
  }

  bool onKey(Key key) override {
    switch (key) {
      case Key::Up: adjust(+1); return true;
      case Key::Down: adjust(-1); return true;
      case Key::Select:
        if (field_ == Field::On) {
          save();
        } else {
          field_ = static_cast<Field>(static_cast<int>(field_) + 1);
        }
        return true;
      case Key::SelectLong: save(); return true;
      default: return false;  // BOOT: the navigator closes the editor, nothing saved
    }
  }

  void draw(ui::Canvas &c, const Rect &body) override {
    using namespace ui;
    const Rect outer = asideOf(body);
    fill(c, outer, kPaper);
    vrule(c, outer.x, outer.y, outer.h);
    Rect r = asideInner(body);
    drawSectionHeader(c, r.takeTop(kSectionH), "EDIT ALARM " + std::to_string(slot_ + 1),
                      std::to_string(slot_ + 1) + "/" + std::to_string(alarms::kCount));

    // The time, the field being set inverted.
    const Rect timeArea = r.takeTop(112);
    const std::string hh = hhmm(draft_.hour, 0).substr(0, 2);
    const std::string mm = hhmm(0, draft_.minute).substr(3, 2);
    const int w = textWidth(kEditTime, hh + ":" + mm);
    const int cap = capHeight(kEditTime);
    const int base = timeArea.y + (timeArea.h - cap) / 2 + cap;
    int x = timeArea.x + (timeArea.w - w) / 2;
    const auto part = [&](const std::string &text, bool selected) {
      const int pw = textWidth(kEditTime, text);
      if (selected) fill(c, {x - 6, base - cap - 10, pw + 12, cap + 20}, kInk);
      drawText(c, x, base, text, kEditTime, selected ? kPaper : kInk);
      x += pw + kEditTime.tracking;
    };
    part(hh, field_ == Field::Hour);
    part(":", false);
    part(mm, field_ == Field::Minute);
    hrule(c, r.x, r.y, r.w);
    r.takeTop(kRule);

    const std::pair<std::string, std::string> rows[] = {
        {"REPEAT", alarms::repeatLabel(draft_.days)},
        {"ALARM", draft_.on ? "ON" : "OFF"},
    };
    for (int i = 0; i < 2; ++i) {
      const Rect line = r.takeTop(kRowH);
      const bool selected = static_cast<int>(field_) == 2 + i;
      const uint16_t ink = selected ? kPaper : kInk;
      const int lx = line.x + kAsidePadX;
      if (selected) {
        fill(c, line, kInk);
        drawText(c, {lx, line.y, kMarkerW, line.h}, glyph::kPlay, kCalHead, Align::Left, ink);
      } else {
        dashedH(c, line.x, line.bottom() - 1, line.w);
      }
      drawText(c, {lx + kMarkerW + 6, line.y, 80, line.h}, rows[i].first, kEditKey, Align::Left,
               ink);
      drawText(c, {lx + kMarkerW + 86, line.y, line.right() - kAsidePadX - lx - kMarkerW - 86,
                   line.h},
               rows[i].second, selected ? kEditValueSelected : kEditValue, Align::Right, ink);
    }

    // When it will ring, so the repeat choice can be checked.
    r.takeTop(14);
    std::string when = "WON'T RING (OFF)";
    if (draft_.on && rtc::timeValid()) {
      const time_t now = time(nullptr);
      const time_t ring = alarms::ringAfter(draft_, now);
      if (ring) {
        tm l;
        localtime_r(&ring, &l);
        char buf[40];
        snprintf(buf, sizeof buf, "RINGS %s %d %s, %02d:%02d", kWeekdayShort[l.tm_wday],
                 l.tm_mday, kMonthNames[l.tm_mon], l.tm_hour, l.tm_min);
        when = buf;
      }
    } else if (draft_.on) {
      when = "CLOCK NOT SET YET";
    }
    drawText(c, {r.x + kAsidePadX, r.y, r.w - 2 * kAsidePadX, kLabelH}, when, kBarLabel);
  }

 private:
  enum class Field : uint8_t { Hour, Minute, Repeat, On };

  void adjust(int delta) {
    switch (field_) {
      case Field::Hour: draft_.hour = (draft_.hour + delta + 24) % 24; break;
      case Field::Minute: draft_.minute = (draft_.minute + delta + 60) % 60; break;
      case Field::Repeat: draft_.days = alarms::stepRepeat(draft_.days, delta); break;
      case Field::On: draft_.on = !draft_.on; break;
    }
  }

  void save() {
    alarms::set(slot_, draft_);
    ui::nav::pop();
  }

  int slot_;
  alarms::Alarm draft_;
  Field field_ = Field::Hour;
};

// --- Ringing --------------------------------------------------------------------------------

constexpr uint32_t kRingForMs = 2 * 60 * 1000;
constexpr int kSnoozeMinutes = 9;
constexpr int kRingSheetH = 170;
constexpr TextStyle kRingTime{&ui::fonts::kMono96ExtraBold, -6};

// A sheet over the bottom of whatever screen is showing (portrait or landscape).
class RingScreen : public ui::Screen {
 public:
  explicit RingScreen(int slot) : slot_(slot), alarm_(alarms::get(slot)) {}

  std::string tag() const override { return {}; }
  bool overlay() const override { return true; }
  int hints(ui::Hint *out) const override {
    out[0] = {ui::glyph::kPress, "STOP"};
    out[1] = {ui::glyph::kHoldPress, "SNOOZE"};
    out[2] = {"BOOT", "STOP"};
    return 3;
  }

  void onEnter() override {
    if (!started_) {
      started_ = millis();
      audio::ring(true);
    }
  }
  void onLeave() override { audio::ring(false); }

  bool onKey(Key key) override {
    switch (key) {
      case Key::SelectLong:
        alarms::snooze(slot_, time(nullptr) + kSnoozeMinutes * 60);
        ui::nav::pop();
        return true;
      case Key::Select: ui::nav::pop(); return true;
      default: return false;  // BOOT pops (and stops); the dial does nothing
    }
  }

  bool onTick() override {
    if (!quiet_ && millis() - started_ > kRingForMs) {
      quiet_ = true;
      audio::ring(false);
      return true;
    }
    return false;
  }

  void draw(ui::Canvas &c, const Rect &body) override {
    using namespace ui;
    const Rect sheet{body.x, body.bottom() - kRingSheetH, body.w, kRingSheetH};
    fill(c, sheet, kInk);
    const Rect p = sheet.inset(20, 16);
    const Rect head{p.x, p.y, p.w, kLabelH};
    drawText(c, head,
             "// ALARM " + std::to_string(slot_ + 1) + " \xC2\xB7 " +
                 upper(alarms::repeatLabel(alarm_.days)),
             style::kSection, Align::Left, kPaper);
    drawText(c, head, quiet_ ? "MISSED" : "RINGING", style::kSection, Align::Right, kPaper);
    const time_t now = time(nullptr);
    const int base = p.bottom() - 8;
    const int after = drawText(c, p.x, base, hhmm(now), kRingTime, kPaper);
    const std::string snooze = "SNOOZE \xE2\x86\x92 " + hhmm(now + kSnoozeMinutes * 60);  // →
    drawText(c, {after + 20, base - capHeight(kRingTime), p.right() - after - 20, kLabelH},
             hhmm(alarm_.hour, alarm_.minute) + " SET", kBarLabel, Align::Right, kPaper);
    drawText(c, {after + 20, base - kLabelH, p.right() - after - 20, kLabelH}, snooze,
             kBarLabel, Align::Right, kPaper);
  }

 private:
  int slot_;
  alarms::Alarm alarm_;
  uint32_t started_ = 0;
  bool quiet_ = false;
};

// --- Clock ----------------------------------------------------------------------------------

class ClockScreen : public ui::Screen {
 public:
  ClockScreen() { cursor_.reset(alarms::kCount); }

  std::string tag() const override { return "06 CLOCK"; }
  std::string context() const override {
    return "RTC PCF85063A \xC2\xB7 " + syncText(liveFace(-1));
  }
  bool statusClock() const override { return false; }
  bool landscape() const override { return true; }
  int hints(ui::Hint *out) const override {
    out[0] = {ui::glyph::kDial, "ALARM"};
    out[1] = {ui::glyph::kPress, "ON/OFF"};
    out[2] = {ui::glyph::kHoldPress, "EDIT"};
    out[3] = {"BOOT", "BACK"};
    return 4;
  }

  bool onKey(Key key) override {
    switch (key) {
      case Key::Up: return cursor_.move(-1, true);
      case Key::Down: return cursor_.move(+1, true);
      case Key::Select: {
        alarms::Alarm a = alarms::get(cursor_.cursor());
        a.on = !a.on;
        alarms::set(cursor_.cursor(), a);
        return true;
      }
      case Key::SelectLong:
        ui::nav::push(std::make_unique<AlarmEditor>(cursor_.cursor()));
        return true;
      default: return false;
    }
  }

  // Redraws once a minute (and when the clock gets set). One-off alarms that rang have
  // switched themselves off by the next minute.
  bool onTick() override { return minuteKey() != drawnMinute_; }

  void draw(ui::Canvas &c, const Rect &body) override {
    drawnMinute_ = minuteKey();
    drawFace(c, body, liveFace(cursor_.cursor()));
  }

 private:
  static long minuteKey() {
    return rtc::timeValid() ? static_cast<long>(time(nullptr) / 60) : -1;
  }

  ui::ListCursor cursor_;
  long drawnMinute_ = -2;
};

// --- Standby --------------------------------------------------------------------------------

// The Clock face as the last frame before a standby power-off: no cursor, and the hint bar
// says how to wake it. Drawn on the next tick after it's on the panel, then off.
class StandbyScreen : public ui::Screen {
 public:
  explicit StandbyScreen(int cycles) : cycles_(cycles), face_(liveFace(-1)) {}

  std::string tag() const override { return "06 CLOCK"; }
  bool chrome() const override { return false; }  // drawStandbyFace() draws its own
  bool landscape() const override { return true; }
  bool onKey(Key) override {  // a key in the second before power-off: stay on
    ui::nav::pop();
    return true;
  }
  bool onTick() override {
    // The first tick can come straight after the refresh; wait for the next, so a key has a
    // second to cancel.
    if (!drawn_ || ++ticks_ < 2) return false;
    saveFace(face_);
    power::off(power::Mode::Standby, face_.now + 60, cycles_);
    ui::nav::pop();  // still here: the power-off failed
    return true;
  }
  void draw(ui::Canvas &c, const Rect &) override {
    drawStandbyFace(c, face_);
    drawn_ = true;
  }

 private:
  int cycles_;
  Face face_;
  bool drawn_ = false;
  int ticks_ = 0;  // since drawn
};

// Landscape without chrome: the Standby or Sleep screen, on its way to a power-off.
bool poweringOff() {
  const ui::Screen *top = ui::nav::top();
  return top && top->landscape() && !top->chrome();
}

}  // namespace

std::unique_ptr<ui::Screen> makeClock() { return std::make_unique<ClockScreen>(); }

void pollAlarms() {
  static time_t last = 0;  // the end of the window already checked
  static bool first = true;
  if (!rtc::timeValid()) {
    last = 0;
    return;
  }
  const time_t now = time(nullptr);
  if (first) {
    first = false;
    // Woken by the RTC for an alarm: check from just before the time it was armed for, since
    // booting took a second or two.
    const time_t armed = power::armedFor();
    if (power::wake() == power::Wake::Alarm && armed && armed <= now && now - armed < 120) {
      last = armed - 1;
    }
  }
  // First look, or the clock was set or stepped a long way: start from now rather than ring
  // for everything in between.
  if (!last || now < last || now - last > 10 * 60) {
    last = now;
    return;
  }
  if (now == last) return;
  const int slot = alarms::due(last, now);
  last = now;
  if (slot < 0) return;
  log_i("alarm %d rings", slot + 1);
  ui::nav::push(std::make_unique<RingScreen>(slot));
}

void enterStandby(int cycles) {
  if (poweringOff()) return;
  if (!rtc::timeValid()) {  // a clock face needs the time
    sleepNow();
    return;
  }
  ui::nav::push(std::make_unique<StandbyScreen>(cycles));
  ui::nav::invalidate(ui::Refresh::Full);
}

bool standbyWake() {
  if (power::wake() != power::Wake::Alarm || power::lastMode() != power::Mode::Standby) {
    return false;
  }
  const int cycles = power::standbyLeft();
  if (cycles == 1 || !rtc::timeValid()) return false;  // dev tool's last cycle, or no time
  // An alarm due now: boot normally, and pollAlarms() rings it.
  const time_t now = time(nullptr);
  const time_t armed = power::armedFor();
  const time_t ring = alarms::next(armed ? armed - 1 : now - 60);
  if (ring && ring <= now + 1) return false;

  // The panel still shows the last face. Redraw it as the "old" image, so this is a partial
  // refresh; a full one every half hour clears the ghosting.
  ui::Canvas &c = screen::canvas();
  Face old;
  const bool haveOld = loadFace(old);
  const Face face = liveFace(-1);
  screen::lock();
  screen::setLandscape(true);
  if (haveOld) {
    screen::clear();
    drawStandbyFace(c, old);
    screen::assumeShown();
  }
  screen::clear();
  drawStandbyFace(c, face);
  tm t;
  localtime_r(&face.now, &t);
  screen::present(!haveOld || t.tm_min % 30 == 0);
  screen::unlock();
  saveFace(face);
  power::off(power::Mode::Standby, face.now + 60, cycles > 0 ? cycles - 1 : 0);
  return false;  // the PMIC didn't power off: boot normally
}

}  // namespace apps
