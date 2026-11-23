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
std::atomic<uint32_t> attemptGen{0};     // comms::generation() then

constexpr ui::TextStyle kLogText{&ui::fonts::kMono14Bold, 0};    // the log's who → whom
constexpr ui::TextStyle kLogTime{&ui::fonts::kMono14Medium, 0};  // its time

}  // namespace

void noteAttempt() {
  attemptGen = comms::generation();
  attemptMs = millis() | 1;
}

std::string attemptError() {
  const uint32_t at = attemptMs.load();
  if (!at || millis() - at > kErrorShowMs || comms::talking()) return {};
  // Not before the engine has acted on it (it bumps the generation when it fails, and clears
  // the error when it doesn't): until then status().error is an older attempt's.
  if (comms::generation() == attemptGen.load()) return {};
  const comms::Status s = comms::status();
  if (s.state == comms::State::Talking || s.state == comms::State::Hearing) return {};
  return s.error;
}

bool playing() { return comms::status().replay; }

bool stopIfPlaying() {
  if (!playing()) return false;
  comms::stopPlaying();
  return true;
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
  // Rings and messages share the log with calls (comms.h, Kind).
  if (c.kind == comms::Kind::Ring) {
    if (c.live) return "RINGING";
    if (c.answered) return "ANSWERED";
    if (c.outgoing) return "NO ANSWER";
    return c.missed ? "MISSED" : "RANG";  // RANG: ignored here, or a message instead
  }
  if (c.kind == comms::Kind::Message) {
    if (c.outgoing) {
      if (c.live) return "RECORDING";
      return c.heardBy > 0 ? "MSG" + std::string(" ") + ui::glyph::kDot + " " +
                                 std::to_string(c.heardBy) + " GOT IT"
                           : "MSG SENT";
    }
    if (c.live) return "MSG COMING";
    if (!c.played) return "NEW MSG";
    return c.complete ? "MSG" : "MSG PART";
  }
  if (c.live) return "LIVE";
  if (c.outgoing) return c.heardBy > 0 ? std::to_string(c.heardBy) + " HEARD" : "";
  if (!c.played) return "NOT PLAYED";
  if (c.route == comms::Route::Relayed) {
    if (!c.relayName.empty()) return "VIA " + c.relayName;
    return c.hops == 1 ? "1 HOP" : std::to_string(c.hops) + " HOPS";
  }
  return "";
}

bool noteNeedsYou(const comms::Call &c) {
  if (c.outgoing || c.live) return false;
  if (c.kind == comms::Kind::Ring) return c.missed && !c.heard;
  if (c.kind == comms::Kind::Message) return !c.played;
  return false;
}

std::string countText(const std::vector<comms::Call> &calls) {
  if (calls.empty()) return {};
  bool today = rtc::timeValid();
  const time_t now = time(nullptr);
  for (const comms::Call &c : calls) today = today && c.at && timeutil::sameLocalDay(c.at, now);
  const std::string n = std::to_string(calls.size());
  return today ? "TODAY \xC2\xB7 " + n : n + (calls.size() == 1 ? " CALL" : " CALLS");
}

std::string boardsText(int n) { return std::to_string(n) + (n == 1 ? " BOARD" : " BOARDS"); }

std::string membersText(const comms::Conversation &c) {
  std::string s = c.in ? "YOU" : "";
  for (size_t i = 0; i < c.members.size(); ++i) {
    if (!s.empty()) s += " + ";
    s += boardName(i < c.names.size() ? c.names[i] : std::string(), c.members[i]);
  }
  return s;
}

std::string forText(uint32_t ms) {
  const uint32_t m = ms / 60000;
  if (m == 0) return "JUST BEGUN";
  if (m < 60) return "FOR " + std::to_string(m) + " MIN";
  char buf[24];
  snprintf(buf, sizeof buf, "FOR %lu H %02lu", static_cast<unsigned long>(m / 60),
           static_cast<unsigned long>(m % 60));
  return buf;
}

std::string groupName(int group) {
  const std::vector<comms::Group> gs = comms::groups();
  return group >= 0 && group < static_cast<int>(gs.size()) ? gs[group].name : std::string();
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
    if (noteColumn && noteNeedsYou(call)) {  // a missed ring, a message not played: inverted
      const int w = tagWidth(note, style::kCap, 4);
      drawTag(c, right - w, band.centerY(), 19, note, style::kCap, TagKind::Inverted, 4);
      right -= w + 8;
    } else if (noteColumn) {
      const int w = textWidth(style::kKey, note);
      drawText(c, {right - w, band.y, w, band.h}, note, style::kKey);
      right -= w + 8;
    } else {
      who += std::string(" ") + glyph::kDot + " " + note;
    }
  }
  drawText(c, {band.x + kTimeW, band.y, right - band.x - kTimeW, band.h}, who, kLogText);
}

void drawStateRow(ui::Canvas &c, const ui::Rect &row, const std::string &word, int filled,
                  const std::string &right) {
  using namespace ui;
  dashedH(c, row.x, row.bottom() - 1, row.w);
  const int base = baselineCentered(style::kReading, row.y, row.h - 1);
  const int end = drawText(c, row.x + kPadX, base, word, style::kReading);
  if (filled >= 0) {
    const int meterX = std::max(row.x + kPadX + 120 + 14, end + 14);
    drawSegments(c, {meterX, row.y + (row.h - 16) / 2, row.right() - kPadX - meterX, 16}, 20,
                 filled, 2);
  } else if (!right.empty()) {
    drawText(c, row.right() - kPadX - textWidth(style::kReading, right), base, right,
             style::kReading);
  }
}

int drawNameBlock(ui::Canvas &c, const ui::Rect &r, const std::string &label,
                  const std::string &name, const std::string &line, const std::string &line2) {
  using namespace ui;
  const int x = r.x + kPadX, w = r.w - 2 * kPadX;
  int y = r.y + 14;
  drawText(c, {x, y, w, 17}, label, style::kKey);
  y += 17 + 6;
  drawText(c, {x, y, w, 38}, name, style::kTitle);
  y += 38 + 6;
  drawText(c, {x, y, w, 17}, line, style::kLabel);
  y += 17;
  if (!line2.empty()) {
    y += 4;
    drawText(c, {x, y, w, 17}, line2, style::kLabel);
    y += 17;
  }
  return y + 12;
}

void drawEarlier(ui::Canvas &c, ui::Rect r, const std::vector<comms::Call> &calls,
                 const std::string &foot) {
  using namespace ui;
  constexpr int kEarlierRowH = 30;
  hrule(c, r.x, r.y, r.w);
  r.takeTop(kRule);
  drawSectionHeader(c, r.takeTop(kSectionH), "EARLIER", calls.empty() ? "NONE" : countText(calls));
  const Rect f = r.takeBottom(8 + 17);
  for (const comms::Call &k : calls) {
    if (r.h < kEarlierRowH + 6) break;  // a gap over the foot
    drawCallRow(c, r.takeTop(kEarlierRowH), k, false);
  }
  drawText(c, {f.x + kPadX, f.y, f.w - 2 * kPadX, 17}, foot, style::kLabel);
}

}  // namespace commsui

namespace {
// What the last HOLD BOOT began, so its release ends that: a talk, or a message (the ringing
// sheet). The input timer calls both ends; the UI task may undo a hold (hold_boot.cpp).
enum class Held : uint8_t { None, Talk, Message };
std::atomic<Held> held{Held::None};
}  // namespace

void commsTalk(const ui::TalkTo &to) {
  commsui::noteAttempt();
  if (to.message) {  // the ring nobody answered: the message goes to whom it rang
    held = Held::Message;
    comms::messageStart();
    return;
  }
  if (to.answer) comms::answer();  // the ring sheet: answered, then the talk goes into it
  held = Held::Talk;
  comms::talkStart(to.group, to.board);
}

void commsTalkStop() {
  switch (held.exchange(Held::None)) {
    case Held::Message: comms::messageSend(); break;
    case Held::Talk: comms::talkStop(); break;
    case Held::None: break;
  }
}

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

// "7F3A · 3 ON AIR · 1 THROUGH INKHARNESS", with "1 NEW" when it has calls, missed rings or
// messages not heard.
std::string groupLine(const comms::Group &g) {
  std::string s = g.tag;
  if (g.unheard > 0) s += kDot + std::to_string(g.unheard) + " NEW";
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

// --- The Messages sheet -------------------------------------------------------------------------

// The group's messages, newest first: ● plays one (or stops the one playing), BOOT closes (or
// stops). Over the talk screen, grown to fit like the Groups sheet.
class MessagesSheet : public ui::Screen {
 public:
  explicit MessagesSheet(int group) : group_(group) {
    load();
    cursor_.reset(static_cast<int>(msgs_.size()));
  }

  std::string tag() const override { return {}; }  // the Comms screen's chrome
  bool overlay() const override { return true; }
  int hints(ui::Hint *out) const override {
    int n = 0;
    if (!msgs_.empty()) {
      out[n++] = {ui::glyph::kDial, "MOVE"};
      const comms::Message &m = msgs_[cursor_.cursor()];
      if (!m.receiving) out[n++] = {ui::glyph::kPress, m.id == playingId_ ? "STOP" : "PLAY"};
    }
    out[n++] = {"BOOT", playingId_ ? "STOP" : "CLOSE"};
    return n;
  }

  bool onKey(Key key) override {
    switch (key) {
      case Key::Up: return cursor_.move(-1, true);
      case Key::Down: return cursor_.move(+1, true);
      case Key::Select: {
        if (msgs_.empty()) return false;
        const comms::Message &m = msgs_[cursor_.cursor()];
        if (m.receiving) return true;  // still coming
        if (m.id == playingId_) {
          comms::stopPlaying();
          return true;
        }
        if (playing()) comms::stopPlaying();  // the task stops it before it plays this one
        noteAttempt();
        comms::playMessage(m.id);
        return true;
      }
      case Key::Back: return stopIfPlaying();  // else it closes
      default: return false;
    }
  }

  bool onTick() override {
    if (comms::generation() == gen_ && attemptError() == drawnError_) return false;
    load();
    return true;
  }

  void draw(ui::Canvas &c, const Rect &body) override {
    using namespace ui;
    drawnError_ = attemptError();
    constexpr int kFootH = 2 * 20 + 9 + 6;
    const int count = static_cast<int>(msgs_.size());
    const int fit = std::max(1, (body.h - kRule - kSectionH - kFootH) / kRowTallH);
    const int rows = std::max(1, std::min(count, fit));
    Rect sheet{body.x, body.bottom() - (kRule + kSectionH + rows * kRowTallH + kFootH), body.w, 0};
    sheet.h = body.bottom() - sheet.y;
    fill(c, sheet, kPaper);
    hrule(c, sheet.x, sheet.y, sheet.w);
    Rect r = sheet;
    r.takeTop(kRule);
    const Rect head = r.takeTop(kSectionH);
    if (!drawnError_.empty()) {
      drawNotice(c, head, drawnError_);
    } else {
      drawSectionHeader(c, head, "MESSAGES" + kDot + groupName(group_),
                        count ? cursor_.label() : "");
    }
    const Rect foot = r.takeBottom(kFootH);
    if (msgs_.empty()) {
      drawText(c, {r.x + kPadX, r.y, r.w - 2 * kPadX, r.h}, "NO MESSAGES YET", style::kLabel,
               Align::Center);
    } else {
      drawList(c, r, kRowTallH, cursor_, [this](int i) { return messageRow(msgs_[i]); });
    }
    bool inRam = false;
    for (const comms::Message &m : msgs_) inRam = inRam || !m.onCard;
    int y = foot.y + 6 + capHeight(style::kLabel) + 4;
    drawText(c, foot.x + kPadX, y,
             inRam ? "KEPT IN MEMORY: A POWER-OFF LOSES THEM" : "KEPT UNTIL PLAYED, AND A DAY AFTER",
             style::kLabel);
    y += 20;
    drawText(c, foot.x + kPadX, y, "LEFT FROM 5 S INTO A RING NOBODY ANSWERS", style::kLabel);
  }

 private:
  void load() {
    gen_ = comms::generation();
    const uint32_t was = msgs_.empty() ? 0 : msgs_[cursor_.cursor()].id;
    msgs_ = comms::messages(group_);
    const comms::Status s = comms::status();
    playingId_ = s.playingMessage;
    // The cursor stays on its message as new ones come in on top.
    int at = 0;
    for (size_t i = 0; i < msgs_.size(); ++i) {
      if (msgs_[i].id == was) at = static_cast<int>(i);
    }
    cursor_.reset(static_cast<int>(msgs_.size()), at);
  }

  // KITCHEN · 14:08 over 0:12 · TO ALL, its state boxed: NEW (inverted), PLAYED, COMING,
  // PLAYING (inverted).
  ui::Row messageRow(const comms::Message &m) const {
    ui::Row row;
    row.title = boardName(m.fromName, m.from) + kDot + whenText(m.at, m.agoS);
    row.subtitle = lengthText(m.ms) + kDot + (m.to ? "TO YOU" : "TO ALL");
    if (!m.complete && !m.receiving) row.subtitle += kDot + "PART: SOME OF IT LOST";
    if (m.receiving) {
      row.tag = "COMING";
    } else if (m.id == playingId_) {
      row.tag = "PLAYING";
      row.tagInverted = true;
      row.subtitle = lengthText(m.ms) + kDot + "PLAYING";
    } else if (!m.played) {
      row.tag = "NEW";
      row.tagInverted = true;
    } else {
      row.tag = "PLAYED";
    }
    return row;
  }

  int group_;
  std::vector<comms::Message> msgs_;
  ui::ListCursor cursor_;
  uint32_t playingId_ = 0;
  uint32_t gen_ = 0;
  std::string drawnError_;
};

// --- The talk screen ----------------------------------------------------------------------------

// CONVERSATION fits beside the map at 28 px; 38 px (the state words' size) doesn't.
constexpr ui::TextStyle kConversationWord{&ui::fonts::kMono28ExtraBold, 0};

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
    out[1] = {ui::glyph::kPress, !conv_.on ? "RING" : conv_.in ? "LEAVE" : "JOIN"};
    out[2] = {ui::glyph::kHoldPress, "MENU"};
    out[3] = {"BOOT", playing() ? "STOP" : "BACK"};
    return 4;
  }
  // HOLD BOOT talks to the row selected in TALK TO, or to the conversation this board is in.
  ui::TalkTo talkTo() const override {
    ui::TalkTo to;
    to.comms = true;
    to.group = group_;
    to.board = talkBoard();
    return to;
  }

  // Back from a sheet: the Groups sheet may have switched the group.
  void onEnter() override {
    const uint32_t was = talkBoard();
    load();
    if (talkBoard() != was) ui::nav::talkChanged();
  }

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
      case Key::Select: mainAction(); return true;
      case Key::SelectLong: openMenu(); return true;
      case Key::Back: return stopIfPlaying();  // else back
      default: return false;
    }
  }

  bool onTick() override {
    if (comms::generation() == gen_ && extra() == drawnExtra_) return false;
    const uint32_t was = talkBoard();
    load();
    if (talkBoard() != was) ui::nav::talkChanged();
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
  // In the group's conversation, HOLD BOOT talks to all of it.
  uint32_t talkBoard() const { return conv_.on && conv_.in ? 0 : selected_; }

  // A failed attempt's notice, in place of CHANNEL's header; a sheet over the screen (the
  // ringing sheet, Messages) shows its own.
  std::string notice() const { return ui::nav::top() == this ? attemptError() : std::string(); }

  // ●: rings the row selected while the group has no conversation, else joins it or leaves it.
  void mainAction() {
    if (conv_.on && conv_.in) {
      comms::leave(group_);
    } else if (conv_.on) {
      if (!comms::join(group_)) noteAttempt();  // a join doesn't clear an older error
    } else if (comms::ringStart(group_, selected_)) {
      ui::nav::push(makeRingingSheet(group_, selected_));  // it says if the ring won't start
    } else {
      noteAttempt();
    }
  }

  // What changes the screen besides comms::generation(): a talk's error notice, the radio still
  // starting, the conversation's minutes, PLAY LAST or a message playing (BOOT's verb).
  std::string extra() const {
    const bool starting = status_.state == comms::State::Off && millis() - openedMs_ < kStartingMs;
    std::string e = notice() + (starting ? "\x01" : "");
    if (conv_.on) e += forText(comms::conversation(group_).ms);
    if (playing()) e += "\x02";
    return e;
  }

  void load() {
    gen_ = comms::generation();
    groups_ = comms::groups();
    status_ = comms::status();
    if (groups_.empty()) {
      boards_.clear();
      calls_.clear();
      conv_ = comms::Conversation();
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
    conv_ = comms::conversation(group_);
    // The cursor stays on its board as the list reorders; ALL if it has gone.
    int at = 0;
    for (size_t i = 0; i < boards_.size(); ++i) {
      if (boards_[i].id == selected_) at = static_cast<int>(i) + 1;
    }
    if (at == 0) selected_ = 0;
    cursor_.reset(static_cast<int>(boards_.size()) + 1, at);
    // On screen, the group's calls and rings count as heard (a message once it has played).
    if (groups_[group_].unheard > 0) comms::markHeard(group_);
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
    const int group = group_;
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
    items.push_back({"MESSAGES",
                     [group]() -> std::string {
                       const std::vector<comms::Message> ms = comms::messages(group);
                       int fresh = 0;
                       for (const comms::Message &m : ms) fresh += !m.played && !m.receiving;
                       if (fresh) return std::to_string(fresh) + " NEW";
                       return ms.empty() ? "NONE" : std::to_string(ms.size()) + " KEPT";
                     },
                     [group] { ui::nav::replace(makeMessagesSheet(group)); }});
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

  // CHANNEL: the state (or CONVERSATION while the group has one and nobody speaks), a line or
  // two under it, the meter and the legend on the left; the map right.
  void drawChannel(ui::Canvas &c, Rect r) const {
    using namespace ui;
    const comms::Status &s = status_;
    const std::string error = notice();
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
    const bool live = s.state == comms::State::Talking || s.state == comms::State::Hearing;
    std::string line1 = stateLine(s), line2;
    if (conv_.on && !live) {
      // Capitals at the same height as the 38 px words'.
      const int capTop = base - capHeight(style::kTitle);
      drawText(c, x, capTop + capHeight(kConversationWord), "CONVERSATION", kConversationWord);
      line1 = membersText(conv_);
      line2 = forText(conv_.ms) + kDot +
              (conv_.in ? "YOU'RE IN IT" : conv_.left ? "YOU LEFT IT" : "NOT IN IT");
    } else {
      drawText(c, x, base, stateWord(s.state), style::kTitle);
      if (conv_.on) line2 = "CONVERSATION" + kDot + membersText(conv_);
    }
    drawText(c, {x, top + 44, w, 17}, line1, style::kLabel);
    if (!line2.empty()) drawText(c, {x, top + 64, w, 17}, line2, style::kLabel);
    drawMeter(c, {x, top + 91, w, 14}, live ? meterFill(s.levelDb) : 0);
    drawLegend(c, x, r.bottom() - 16);
    drawMap(c, {r.right() - kPadX - kMapW, top, kMapW, kMapH}, boards_);
  }

  // The line under the state: the last call, ring or message; who is heard or talked to, and for
  // how long.
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
        if (s.playingMessage) return "MESSAGE" + kDot + boardName(s.peerName, s.peer) + kDot + len;
        if (s.replay) return "REPLAY" + kDot + boardName(s.peerName, s.peer) + kDot + len;
        return boardName(s.peerName, s.peer) + " \xE2\x86\x92 " + (s.toAll ? "ALL" : "YOU") + kDot +
               len + inGroup;
      case comms::State::Quiet:
        break;
    }
    for (const comms::Call &c : calls_) {
      if (c.live) continue;
      const char *what = c.kind == comms::Kind::Ring      ? "RING"
                         : c.kind == comms::Kind::Message ? "MSG"
                                                          : "LAST";
      return what + kDot + (c.outgoing ? std::string("YOU") : boardName(c.fromName, c.from)) +
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
  comms::Conversation conv_;
  int group_ = -1;        // the group on screen
  uint32_t selected_ = 0;  // TALK TO's row: a board's id, 0 for all
  ui::ListCursor cursor_;
};

}  // namespace

std::unique_ptr<ui::Screen> commsui::makeMessagesSheet(int group) {
  return std::make_unique<MessagesSheet>(group);
}

std::unique_ptr<ui::Screen> makeComms() { return std::make_unique<CommsScreen>(); }

}  // namespace apps

#endif  // INKDECK_COMMS
