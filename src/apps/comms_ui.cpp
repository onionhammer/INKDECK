#include "features.h"

#if INKDECK_COMMS  // the whole file (features.h)

#include "apps/comms_ui.h"

#include <Arduino.h>
#include <stdlib.h>
#include <time.h>

#include <algorithm>
#include <atomic>
#include <vector>

#include "apps/home.h"
#include "rtc.h"
#include "timeutil.h"
#include "ui/menu.h"

namespace apps {

namespace commsui {
namespace {

constexpr uint32_t kErrorShowMs = 8000;  // a failed talk's notice
std::atomic<uint32_t> attemptMs{0};      // the last talk or PLAY LAST from these screens

constexpr ui::TextStyle kLogText{&ui::fonts::kMono14Bold, 0};    // the log's who → whom
constexpr ui::TextStyle kLogTime{&ui::fonts::kMono14Medium, 0};  // its time

}  // namespace

void noteAttempt() { attemptMs = millis() | 1; }

std::string attemptError() {
  const uint32_t at = attemptMs.load();
  if (!at || millis() - at > kErrorShowMs || comms::talking()) return {};
  const comms::Status s = comms::status();
  if (s.state == comms::State::Talking || s.state == comms::State::Hearing) return {};
  return s.error;
}

const char *stateWord(comms::State s) {
  switch (s) {
    case comms::State::Off: return "OFF";
    case comms::State::Quiet: return "QUIET";
    case comms::State::Talking: return "TALKING";
    case comms::State::Hearing: return "HEARING";
  }
  return "";
}

int meterFill(float levelDb) { return std::clamp(static_cast<int>((levelDb + 60) / 3), 0, 20); }

std::string lengthText(uint32_t ms) { return timeutil::clockText((ms + 500) / 1000); }

std::string whenText(time_t at, uint32_t agoS) {
  if (at && rtc::timeValid()) {
    const time_t now = time(nullptr);
    if (timeutil::sameLocalDay(at, now)) return timeutil::hhmm(at);
    tm t;
    localtime_r(&at, &t);
    return std::to_string(t.tm_mday) + " " + timeutil::kMonthNames[t.tm_mon];
  }
  if (agoS < 60) return "NOW";
  if (agoS < 3600) return std::to_string(agoS / 60) + " MIN";
  return std::to_string(agoS / 3600) + " H";
}

std::string boardName(const std::string &name, uint32_t id) {
  if (!name.empty()) return name;
  char buf[16];
  snprintf(buf, sizeof buf, "%08lX", static_cast<unsigned long>(id));
  return buf;
}

std::string whoText(const comms::Call &c) {
  if (c.outgoing) return "YOU \xE2\x86\x92 " + (c.to ? boardName(c.toName, c.to) : "ALL");
  return boardName(c.fromName, c.from) + " \xE2\x86\x92 " + (c.to ? "YOU" : "ALL");
}

std::string noteText(const comms::Call &c) {
  if (c.live) return "LIVE";
  if (c.outgoing) return c.heardBy > 0 ? std::to_string(c.heardBy) + " HEARD" : "";
  if (!c.played) return "NOT PLAYED";
  if (c.route == comms::Route::Relayed) {
    if (!c.relayName.empty()) return "VIA " + c.relayName;
    return c.hops == 1 ? "1 HOP" : std::to_string(c.hops) + " HOPS";
  }
  return "";
}

std::string countText(const std::vector<comms::Call> &calls) {
  if (calls.empty()) return {};
  bool today = rtc::timeValid();
  const time_t now = time(nullptr);
  for (const comms::Call &c : calls) today = today && c.at && timeutil::sameLocalDay(c.at, now);
  const std::string n = std::to_string(calls.size());
  return today ? "TODAY \xC2\xB7 " + n : n + (calls.size() == 1 ? " CALL" : " CALLS");
}

void drawCallRow(ui::Canvas &c, const ui::Rect &r, const comms::Call &call, bool noteColumn) {
  using namespace ui;
  dashedH(c, r.x, r.bottom() - 1, r.w);
  const Rect band{r.x + kPadX, r.y, r.w - 2 * kPadX, r.h - 1};
  constexpr int kTimeW = 58, kLenW = 48;
  drawText(c, {band.x, band.y, kTimeW, band.h}, whenText(call.at, call.agoS), kLogTime);
  drawText(c, {band.right() - kLenW, band.y, kLenW, band.h}, lengthText(call.ms), kLogText,
           Align::Right);
  int right = band.right() - kLenW;
  std::string who = whoText(call);
  const std::string note = noteText(call);
  if (!note.empty()) {
    if (noteColumn) {
      const int w = textWidth(style::kKey, note);
      drawText(c, {right - w, band.y, w, band.h}, note, style::kKey);
      right -= w + 8;
    } else {
      who += std::string(" ") + glyph::kDot + " " + note;
    }
  }
  drawText(c, {band.x + kTimeW, band.y, right - band.x - kTimeW, band.h}, who, kLogText);
}

}  // namespace commsui

void commsTalk(int group, uint32_t board) {
  commsui::noteAttempt();
  comms::talkStart(group, board);
}

void commsTalkStop() { comms::talkStop(); }

namespace {

using namespace commsui;
using ui::Key;
using ui::Rect;

const std::string kDot = std::string(" ") + ui::glyph::kDot + " ";

constexpr int kChannelH = 156;   // under the CHANNEL header
constexpr int kMaxTalkRows = 5;  // TALK TO's rows on screen; more page
constexpr int kLogRowH = 30;
constexpr int kMaxLog = 24;
constexpr uint32_t kStartingMs = 6000;  // OFF at first says the radio is starting

constexpr ui::TextStyle kMapText{&ui::fonts::kMono13Bold, 0};

// --- Boards and groups as the rows say them ---------------------------------------------------

std::string plural(int n, const char *one, const char *many) {
  return std::to_string(n) + " " + (n == 1 ? one : many);
}

std::string dbm(int rssi) { return ui::signedNumber(static_cast<float>(rssi), 0) + " dBm"; }

std::string routeTag(const comms::Board &b) {
  if (b.route == comms::Route::Away) return "AWAY";
  if (b.flags & comms::kFlagRoom) return "ROOM";
  switch (b.route) {
    case comms::Route::Air: return "AIR";
    case comms::Route::Relayed: return plural(b.hops, "HOP", "HOPS");
    case comms::Route::Net: return "NET";
    case comms::Route::Away: break;
  }
  return "AWAY";
}

// How a board is reached, for its row's second line.
std::string routeLine(const comms::Board &b) {
  std::string s;
  switch (b.route) {
    case comms::Route::Air: s = "ON AIR" + kDot + dbm(b.rssi); break;
    case comms::Route::Relayed:
      s = b.relayName.empty() ? "ON AIR THROUGH " + plural(b.hops, "RELAY", "RELAYS")
                              : "ON AIR VIA " + b.relayName;
      break;
    case comms::Route::Net: s = "VIA INKHARNESS"; break;
    case comms::Route::Away: {
      if (b.heardAt && rtc::timeValid()) return "NOT HEARD SINCE " + whenText(b.heardAt, 0);
      const uint32_t m = b.heardAgoS / 60;
      return m < 60 ? "NOT HEARD FOR " + std::to_string(m) + " MIN"
                    : "NOT HEARD FOR " + std::to_string(m / 60) + " H";
    }
  }
  if (b.flags & comms::kFlagRoom) return s + kDot + "MONITORING THE ROOM";
  if (b.talkedAt) s += kDot + "TALKED " + whenText(b.talkedAt, 0);
  return s;
}

// "7F3A · 3 ON AIR · 1 THROUGH INKHARNESS", with "1 NEW CALL" when it has calls not heard.
std::string groupLine(const comms::Group &g) {
  std::string s = g.tag;
  if (g.unheard > 0) s += kDot + plural(g.unheard, "NEW CALL", "NEW CALLS");
  if (g.via != comms::Via::Net) {
    s += kDot + (g.onAir ? std::to_string(g.onAir) : "NONE") + " ON AIR";
  }
  if (g.via == comms::Via::Air) {
    s += kDot + "NEVER LEAVES THE RADIO";
  } else if (g.onNet > 0) {
    s += kDot + std::to_string(g.onNet) + " THROUGH INKHARNESS";
  } else if (g.via == comms::Via::Net) {
    s += kDot + (comms::netUp() ? "NOBODY THROUGH INKHARNESS" : "NET ONLY: NO LINK YET");
  }
  return s;
}

// "AIR CH 6 · NET UP", "AIR CH 6", "NET DOWN", "OFF".
std::string channelText(const comms::Status &s, comms::Via via) {
  if (s.state == comms::State::Off || s.channel == 0) return "OFF";
  const std::string air = "AIR CH " + std::to_string(s.channel);
  switch (via) {
    case comms::Via::Air: return air;
    case comms::Via::Net: return s.netUp ? "NET UP" : "NET DOWN";
    case comms::Via::Both: return s.netUp ? air + kDot + "NET UP" : air;
  }
  return air;
}

// --- Drawing helpers --------------------------------------------------------------------------

// A line 2 px thick: solid (off 0), or `on` px of ink then `off` of paper along it.
void patternLine(ui::Canvas &c, int x0, int y0, int x1, int y1, int on, int off) {
  const int dx = abs(x1 - x0), dy = -abs(y1 - y0);
  const int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
  const bool steep = -dy > dx;
  int err = dx + dy;
  for (int step = 0;; ++step) {
    if (off == 0 || step % (on + off) < on) {
      c.drawPixel(x0, y0, ui::kInk);
      c.drawPixel(steep ? x0 + 1 : x0, steep ? y0 : y0 + 1, ui::kInk);
    }
    if (x0 == x1 && y0 == y1) break;
    const int e2 = 2 * err;
    if (e2 >= dy) {
      err += dy;
      x0 += sx;
    }
    if (e2 <= dx) {
      err += dx;
      y0 += sy;
    }
  }
}

// How a route is drawn on the map: solid over the air, dashed through relays, dotted over the net.
void routePattern(comms::Route r, int &on, int &off) {
  on = 1;
  off = 0;
  if (r == comms::Route::Relayed) {
    on = 4;
    off = 3;
  } else if (r == comms::Route::Net) {
    on = 1;
    off = 4;
  }
}

void patternBox(ui::Canvas &c, const Rect &b, comms::Route route) {
  int on, off;
  routePattern(route, on, off);
  if (off == 0) {
    ui::frame(c, b, 2);
    return;
  }
  patternLine(c, b.x, b.y, b.right() - 1, b.y, on, off);
  patternLine(c, b.x, b.bottom() - 2, b.right() - 1, b.bottom() - 2, on, off);
  patternLine(c, b.x, b.y, b.x, b.bottom() - 1, on, off);
  patternLine(c, b.right() - 2, b.y, b.right() - 2, b.bottom() - 1, on, off);
}

// A name for the map's small boxes: a default name (INKDECK-7F3A) that doesn't fit loses its
// common start rather than the part that tells boards apart.
std::string mapName(const std::string &name, int maxW) {
  static const std::string kDefault = "INKDECK-";
  if (ui::textWidth(kMapText, name) <= maxW) return name;
  if (name.size() > kDefault.size() && name.compare(0, kDefault.size(), kDefault) == 0) {
    return ui::ellipsize(kMapText, name.substr(kDefault.size()), maxW);
  }
  return ui::ellipsize(kMapText, name, maxW);
}

// The boards reached, as a small map: this board inverted on the left, up to four others in
// fixed slots on the right, each joined to it by its route's line (more than four: the last slot
// says how many more).
void drawMap(ui::Canvas &c, const Rect &m, const std::vector<comms::Board> &boards) {
  using namespace ui;
  constexpr int kSlots = 4, kSlotH = 22, kSelfH = 28, kMaxSelfW = 80, kLineGap = 32;
  std::vector<const comms::Board *> reached;
  for (const comms::Board &b : boards) {
    if (b.route != comms::Route::Away) reached.push_back(&b);
  }

  const std::string self = mapName(comms::boardName(), kMaxSelfW - 12);
  const int selfW = textWidth(kMapText, self) + 12;
  const Rect me{m.x, m.y + (m.h - kSelfH) / 2, selfW, kSelfH};
  fill(c, me, kInk);
  drawText(c, me.inset(4, 0), self, kMapText, Align::Center, kPaper);

  const int slotX = me.right() + kLineGap;
  const int slotMaxW = m.right() - slotX;
  const int pitch = (m.h - kSlotH) / (kSlots - 1);
  if (reached.empty()) {
    const int mid = me.centerY();
    drawText(c, {slotX - 8, mid - 19, slotMaxW + 8, 18}, "NOBODY", kMapText, Align::Center);
    drawText(c, {slotX - 8, mid + 1, slotMaxW + 8, 18}, "IN REACH", kMapText, Align::Center);
    return;
  }
  const int n = static_cast<int>(reached.size());
  const int shown = n > kSlots ? kSlots - 1 : n;
  // Fewer than four: centred on this board's height.
  const int slots = n > kSlots ? kSlots : n;
  const int firstY = m.y + (m.h - kSlotH) / 2 - (slots - 1) * pitch / 2;
  for (int k = 0; k < shown; ++k) {
    const comms::Board &b = *reached[k];
    const std::string name = mapName(boardName(b.name, b.id), slotMaxW - 14);
    const int w = textWidth(kMapText, name) + 14;
    const Rect box{slotX, firstY + k * pitch, w, kSlotH};
    int on, off;
    routePattern(b.route, on, off);
    const int fromY = me.y + 6 + (shown > 1 ? k * (kSelfH - 12) / (shown - 1) : (kSelfH - 12) / 2);
    patternLine(c, me.right(), fromY, box.x - 1, box.centerY(), on, off);
    patternBox(c, box, b.route);
    drawText(c, box.inset(5, 0), name, kMapText, Align::Center);
  }
  if (n > shown) {
    const Rect slot{slotX, firstY + shown * pitch, slotMaxW, kSlotH};
    drawText(c, slot, "+" + std::to_string(n - shown) + " MORE", kMapText);
  }
}

// "─ AIR  ┄ RELAYED  ··· NET": the map's lines, drawn, each before its word.
void drawLegend(ui::Canvas &c, int x, int baseline) {
  using namespace ui;
  const int midY = baseline - capHeight(style::kLabel) / 2 - 1;
  const struct {
    comms::Route route;
    const char *word;
  } items[] = {{comms::Route::Air, "AIR"}, {comms::Route::Relayed, "RELAYED"},
               {comms::Route::Net, "NET"}};
  for (const auto &it : items) {
    int on, off;
    routePattern(it.route, on, off);
    patternLine(c, x, midY, x + 15, midY, on, off);
    x = drawText(c, x + 21, baseline, it.word, style::kLabel) + 14;
  }
}

// The 20-segment meter with its label.
void drawMeter(ui::Canvas &c, const Rect &r, int filled) {
  using namespace ui;
  drawText(c, {r.x, r.y, 22, r.h}, "IN", style::kCap);
  drawSegments(c, {r.x + 28, r.y, r.w - 28, r.h}, 20, filled, 2);
}

// --- The Groups sheet ---------------------------------------------------------------------------

class GroupsSheet : public ui::Screen {
 public:
  GroupsSheet() {
    load();
    cursor_.reset(static_cast<int>(groups_.size()), comms::activeGroup());
  }

  std::string tag() const override { return {}; }  // the Comms screen's chrome
  bool overlay() const override { return true; }
  int hints(ui::Hint *out) const override {
    out[0] = {ui::glyph::kDial, "MOVE"};
    out[1] = {ui::glyph::kPress, "SWITCH"};
    out[2] = {ui::glyph::kHoldPress, "VIA"};
    out[3] = {"BOOT", "CLOSE"};
    return 4;
  }

  bool onKey(Key key) override {
    switch (key) {
      case Key::Up: return cursor_.move(-1, true);
      case Key::Down: return cursor_.move(+1, true);
      case Key::Select:
        comms::setActiveGroup(cursor_.cursor());
        ui::nav::pop();  // the Comms screen comes back on that group
        return true;
      case Key::SelectLong:
        comms::cycleVia(cursor_.cursor());
        load();
        return true;
      default: return false;  // BOOT closes
    }
  }

  bool onTick() override {
    if (comms::generation() == gen_) return false;
    load();
    return true;
  }

  void draw(ui::Canvas &c, const Rect &body) override {
    using namespace ui;
    constexpr int kFootH = 2 * 20 + 9 + 6;
    const int count = static_cast<int>(groups_.size());
    const int fit = std::max(1, (body.h - kRule - kSectionH - kFootH) / kRowTallH);
    const int rows = std::max(1, std::min(count, fit));
    Rect sheet{body.x, body.bottom() - (kRule + kSectionH + rows * kRowTallH + kFootH), body.w, 0};
    sheet.h = body.bottom() - sheet.y;
    fill(c, sheet, kPaper);
    hrule(c, sheet.x, sheet.y, sheet.w);
    Rect r = sheet;
    r.takeTop(kRule);
    drawSectionHeader(c, r.takeTop(kSectionH), "GROUPS", count ? cursor_.label() : "");
    const Rect foot = r.takeBottom(kFootH);
    drawList(c, r, kRowTallH, cursor_, [this](int i) {
      const comms::Group &g = groups_[i];
      ui::Row row;
      row.title = g.name;
      row.subtitle = groupLine(g);
      row.tag = comms::viaName(g.via);
      if (g.active) row.mark = "\xE2\x9C\x93";  // ✓
      return row;
    });
    int y = foot.y + 6 + capHeight(style::kLabel) + 4;
    drawText(c, foot.x + kPadX, y,
             "\xE2\x9C\x93 ON SCREEN" + kDot + "CALLS FROM EVERY GROUP STILL COME IN",
             style::kLabel);
    y += 20;
    drawText(c, foot.x + kPadX, y, "VIA: AIR+NET \xE2\x86\x92 AIR \xE2\x86\x92 NET", style::kLabel);
  }

 private:
  void load() {
    gen_ = comms::generation();
    groups_ = comms::groups();
  }

  std::vector<comms::Group> groups_;
  ui::ListCursor cursor_;
  uint32_t gen_ = 0;
};

// --- The talk screen ----------------------------------------------------------------------------

class CommsScreen : public ui::Screen {
 public:
  CommsScreen() : openedMs_(millis()) {
    comms::hold();  // calls come in while it's up, on battery too
    load();
  }
  ~CommsScreen() override { comms::release(); }

  std::string tag() const override { return moduleTag("COMMS"); }
  std::string context() const override {
    if (!comms::configured()) return "NOT SET UP";
    if (groups_.empty()) return {};
    // The canvas adds "· 5 BOARDS", which the status bar's WIFI leaves no room for: ALL OF's
    // row says it.
    const comms::Group &g = groups_[group_];
    return g.name + kDot + g.tag;
  }
  int hints(ui::Hint *out) const override {
    if (!comms::configured()) {
      out[0] = {"BOOT", "BACK"};
      return 1;
    }
    out[0] = {"HOLD BOOT", "TALK"};
    out[1] = {ui::glyph::kHoldPress, "GROUP"};
    out[2] = {ui::glyph::kPress, "MENU"};
    out[3] = {"BOOT", "BACK"};
    return 4;
  }
  // HOLD BOOT talks to the row selected in TALK TO.
  ui::TalkTo talkTo() const override {
    ui::TalkTo to;
    to.comms = true;
    to.group = group_;
    to.board = selected_;
    return to;
  }

  // Back from a sheet: the Groups sheet may have switched the group.
  void onEnter() override { load(); }

  bool onKey(Key key) override {
    if (!comms::configured()) return false;
    switch (key) {
      case Key::Up:
      case Key::Down: {
        if (!cursor_.move(key == Key::Up ? -1 : 1)) return false;
        const int i = cursor_.cursor();
        selected_ = i == 0 ? 0 : boards_[i - 1].id;
        ui::nav::talkChanged();
        return true;
      }
      case Key::Select: openMenu(); return true;
      case Key::SelectLong: ui::nav::push(std::make_unique<GroupsSheet>()); return true;
      default: return false;
    }
  }

  bool onTick() override {
    if (comms::generation() == gen_ && extra() == drawnExtra_) return false;
    const uint32_t was = selected_;
    load();
    if (selected_ != was) ui::nav::talkChanged();
    return true;
  }

  void draw(ui::Canvas &c, const Rect &body) override {
    using namespace ui;
    drawnExtra_ = extra();
    Rect r = body;
    if (!comms::configured()) {
      drawNotSetUp(c, r);
      return;
    }
    if (groups_.empty()) return;  // the task hasn't loaded them yet
    drawGroupBand(c, r.takeTop(kSectionH));
    drawChannel(c, r.takeTop(kSectionH + kChannelH));

    hrule(c, r.x, r.y, r.w);
    r.takeTop(kRule);
    const int rows = std::clamp(cursor_.count(), 1, kMaxTalkRows);
    Rect talk = r.takeTop(kSectionH + rows * kRowTallH);
    drawSectionHeader(c, talk.takeTop(kSectionH), "TALK TO", cursor_.label());
    drawList(c, talk, kRowTallH, cursor_, [this](int i) { return talkRow(i); });

    hrule(c, r.x, r.y, r.w);
    r.takeTop(kRule);
    drawSectionHeader(c, r.takeTop(kSectionH), "HEARD", countText(calls_));
    if (calls_.empty()) {
      drawText(c, {r.x, r.y + 12, r.w, 24}, "NO CALLS YET" + kDot + "HOLD BOOT TO TALK",
               style::kLabel, Align::Center);
      return;
    }
    for (size_t i = 0; i < calls_.size() && r.h >= kLogRowH; ++i) {
      drawCallRow(c, r.takeTop(kLogRowH), calls_[i], true);
    }
  }

 private:
  // What changes the screen besides comms::generation(): a talk's error notice, and the radio
  // still starting.
  std::string extra() const {
    const bool starting = status_.state == comms::State::Off && millis() - openedMs_ < kStartingMs;
    return attemptError() + (starting ? "\x01" : "");
  }

  void load() {
    gen_ = comms::generation();
    groups_ = comms::groups();
    status_ = comms::status();
    if (groups_.empty()) {
      boards_.clear();
      calls_.clear();
      cursor_.reset(1);
      selected_ = 0;
      return;
    }
    const int active = std::clamp(comms::activeGroup(), 0, static_cast<int>(groups_.size()) - 1);
    if (active != group_) {
      group_ = active;
      selected_ = 0;  // a new group: ALL OF it
    }
    boards_ = comms::boards(group_);
    calls_ = comms::calls(group_, kMaxLog);
    // The cursor stays on its board as the list reorders; ALL if it has gone.
    int at = 0;
    for (size_t i = 0; i < boards_.size(); ++i) {
      if (boards_[i].id == selected_) at = static_cast<int>(i) + 1;
    }
    if (at == 0) selected_ = 0;
    cursor_.reset(static_cast<int>(boards_.size()) + 1, at);
    // On screen, the group's calls count as heard.
    if (groups_[group_].unheard > 0) comms::markHeard(group_);
  }

  std::string groupName(int i) const {
    return i >= 0 && i < static_cast<int>(groups_.size()) ? groups_[i].name : std::string();
  }

  ui::Row talkRow(int i) const {
    ui::Row row;
    const comms::Group &g = groups_[group_];
    if (i == 0) {
      row.title = "ALL OF " + g.name;
      if (g.boards == 0) {
        row.subtitle = "NO OTHER BOARD HEARD YET";
      } else {
        row.subtitle = plural(g.boards + 1, "BOARD", "BOARDS") + kDot + std::to_string(g.onAir) +
                       " ON AIR";
        if (g.onNet > 0) row.subtitle += kDot + std::to_string(g.onNet) + " ON NET";
      }
      row.tag = "ALL";
      return row;
    }
    const comms::Board &b = boards_[i - 1];
    row.title = boardName(b.name, b.id);
    row.subtitle = routeLine(b);
    row.tag = routeTag(b);
    return row;
  }

  void openMenu() {
    std::vector<ui::MenuItem> items;
    items.push_back({"PLAY LAST",
                     []() -> std::string {
                       const comms::LastCall l = comms::lastCall();
                       if (!l.have) return "NONE";
                       return boardName(l.fromName, l.from) + kDot + lengthText(l.ms);
                     },
                     [] {
                       ui::nav::pop();  // the screen shows it playing
                       noteAttempt();
                       comms::playLast();
                     }});
    items.push_back({"RELAY FOR OTHERS",
                     []() -> std::string { return comms::relayForOthers() ? "ON" : "OFF"; },
                     [] { comms::setRelayForOthers(!comms::relayForOthers()); }});
    items.push_back({"CALL CHIRP",
                     []() -> std::string { return comms::callChirp() ? "ON" : "OFF"; },
                     [] { comms::setCallChirp(!comms::callChirp()); }});
    items.push_back({"GROUPS",
                     []() -> std::string {
                       const std::vector<comms::Group> gs = comms::groups();
                       const int a = comms::activeGroup();
                       if (a < 0 || a >= static_cast<int>(gs.size())) return {};
                       return gs[a].name + kDot + std::to_string(a + 1) + " OF " +
                              std::to_string(gs.size());
                     },
                     [] { ui::nav::replace(std::make_unique<GroupsSheet>()); }});
    ui::nav::push(std::make_unique<ui::MenuScreen>("COMMS" + kDot + comms::boardName(),
                                                   std::move(items)));
  }

  // The groups' names, the one on screen in a pill, a boxed count after one with calls not
  // heard; on the right VIA and this group's via, boxed.
  void drawGroupBand(ui::Canvas &c, const Rect &r) const {
    using namespace ui;
    const Rect band{r.x + kPadX, r.y, r.w - 2 * kPadX, r.h - kRule};
    hrule(c, r.x, band.bottom(), r.w);
    const int cy = band.centerY();
    constexpr int kH = 19;
    const std::string via = comms::viaName(groups_[group_].via);
    int right = band.right() - tagWidth(via, style::kCap, 5);
    drawTag(c, right, cy, kH, via, style::kCap, TagKind::Outlined, 5);
    right -= 6 + textWidth(style::kSection, "VIA");
    drawText(c, right, baselineCentered(style::kSection, band.y, band.h), "VIA", style::kSection);
    right -= 16;

    const int n = static_cast<int>(groups_.size());
    auto count = [this](int i) {
      return groups_[i].unheard > 0 ? std::to_string(std::min(groups_[i].unheard, 99))
                                    : std::string();
    };
    // Each name 6 px either side (the count box 6 px after it), 6 px between groups. Names are
    // cut to an even share when they don't all fit.
    int fixed = 0, names = 0;
    for (int i = 0; i < n; ++i) {
      const std::string k = count(i);
      fixed += 12 + 6 + (k.empty() ? 0 : tagWidth(k, style::kCap, 4) + 6);
      names += textWidth(i == group_ ? style::kCap : style::kSection, groups_[i].name);
    }
    const int avail = right - band.x;
    const int maxName = fixed + names > avail ? std::max(24, (avail - fixed) / n) : avail;
    int x = band.x;
    for (int i = 0; i < n; ++i) {
      const bool on = i == group_;
      const TextStyle &s = on ? style::kCap : style::kSection;
      const std::string name = ellipsize(s, groups_[i].name, maxName);
      if (on) {
        x += drawTag(c, x, cy, kH, name, s, TagKind::Inverted, 4);
      } else {
        x = drawText(c, x + 6, baselineCentered(s, band.y, band.h), name, s) + 6;
      }
      const std::string k = count(i);
      if (!k.empty()) x += drawTag(c, x + 6, cy, kH, k, style::kCap, TagKind::Outlined, 4) + 6;
      x += 6;
    }
  }

  // CHANNEL: the state, a line under it, the meter and the legend on the left; the map right.
  void drawChannel(ui::Canvas &c, Rect r) const {
    using namespace ui;
    const comms::Status &s = status_;
    const std::string error = attemptError();
    const Rect head = r.takeTop(kSectionH);
    if (!error.empty()) {
      drawNotice(c, head, error);
    } else {
      drawSectionHeader(c, head, "CHANNEL", channelText(s, groups_[group_].via));
    }
    constexpr int kMapW = 190, kMapH = 132, kGap = 14;
    const int top = r.y + 12;
    const int x = r.x + kPadX;
    const int w = r.w - 2 * kPadX - kGap - kMapW;
    const int base = top + 33;
    drawText(c, x, base, stateWord(s.state), style::kTitle);
    drawText(c, {x, top + 46, w, 17}, stateLine(s), style::kLabel);
    const bool live = s.state == comms::State::Talking || s.state == comms::State::Hearing;
    drawMeter(c, {x, top + 75, w, 14}, live ? meterFill(s.levelDb) : 0);
    drawLegend(c, x, r.bottom() - 16);
    drawMap(c, {r.right() - kPadX - kMapW, top, kMapW, kMapH}, boards_);
  }

  // The line under the state: the last call; who is heard or talked to, and for how long.
  std::string stateLine(const comms::Status &s) const {
    const std::string len = lengthText(s.ms);
    const std::string inGroup =
        s.group != group_ && !groupName(s.group).empty() ? kDot + "IN " + groupName(s.group) : "";
    switch (s.state) {
      case comms::State::Off:
        return millis() - openedMs_ < kStartingMs ? "THE RADIO IS STARTING\xE2\x80\xA6"
                                                  : "THE RADIO WON'T START";
      // As the log says it (KITCHEN → YOU), so it fits beside the map.
      case comms::State::Talking:
        return "YOU \xE2\x86\x92 " +
               (s.toAll ? std::string("ALL") : boardName(s.peerName, s.peer)) + kDot + len +
               inGroup;
      case comms::State::Hearing:
        if (s.replay) return "REPLAY" + kDot + boardName(s.peerName, s.peer) + kDot + len;
        return boardName(s.peerName, s.peer) + " \xE2\x86\x92 " + (s.toAll ? "ALL" : "YOU") + kDot +
               len + inGroup;
      case comms::State::Quiet:
        break;
    }
    for (const comms::Call &c : calls_) {
      if (c.live) continue;
      return "LAST" + kDot + (c.outgoing ? std::string("YOU") : boardName(c.fromName, c.from)) +
             kDot + whenText(c.at, c.agoS);
    }
    return "NO CALLS YET";
  }

  void drawNotSetUp(ui::Canvas &c, Rect r) const {
    using namespace ui;
    drawNotice(c, r.takeTop(kSectionH), "NO GROUP SET");
    const int x = r.x + 20, w = r.w - 40;
    int y = r.y + 16;
    auto para = [&](const std::string &text, const TextStyle &s, int maxLines) {
      for (const std::string &line : wrap(s, text, w, maxLines)) {
        y += 22;
        drawText(c, x, y, line, s);
      }
      y += 10;
    };
    para("Boards with the same group line hear each other. Add one to SD:/inkdeck.ini on every "
         "board of the group, under [comms]:",
         style::kBody, 4);
    para("mesh_group = a-long-random-secret?name=Family", style::kRowSmall, 2);
    para("and give each board its name:", style::kBody, 2);
    para("mesh_name = KITCHEN", style::kRowSmall, 1);
    para("A board with an agent_token is in a HOME group with the other boards of that token, "
         "without a line. Then restart the board.",
         style::kBody, 4);
  }

  uint32_t openedMs_;
  uint32_t gen_ = 0;
  std::string drawnExtra_;
  std::vector<comms::Group> groups_;
  std::vector<comms::Board> boards_;
  std::vector<comms::Call> calls_;
  comms::Status status_;
  int group_ = -1;        // the group on screen
  uint32_t selected_ = 0;  // TALK TO's row: a board's id, 0 for all
  ui::ListCursor cursor_;
};

}  // namespace

std::unique_ptr<ui::Screen> makeComms() { return std::make_unique<CommsScreen>(); }

}  // namespace apps

#endif  // INKDECK_COMMS
