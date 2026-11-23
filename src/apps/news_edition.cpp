#include "apps/news_edition.h"

#include <Arduino.h>
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "alarms.h"
#include "apps/agent_face.h"
#include "apps/clock.h"
#include "apps/lock.h"
#include "apps/news.h"
#include "apps/off_face.h"
#include "apps/weather.h"
#include "audio.h"
#include "board_pins.h"
#include "forecast.h"
#include "kv_store.h"
#include "netget.h"
#include "news_library.h"
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

constexpr const char *kNvs = "newsface";
constexpr time_t kWindowS = 12 * 3600;    // the stories it cycles: published this recently
constexpr time_t kAheadS = 10 * 60;       // a story dated this far ahead still counts
constexpr time_t kFetchEveryS = 3600;     // fetches at most this often
constexpr time_t kFullEveryS = 30 * 60;   // a full refresh this often at least (ghosting)
constexpr time_t kFullGapS = 4 * 60;      // a new story gets a full refresh, this far apart
constexpr uint32_t kSyncMs = 90 * 1000;   // a fetch from behind the face, at most
constexpr uint32_t kStopMs = 20 * 1000;   // then for the feed under way to finish

// The News Off Face canvas.
constexpr int kW = 480;
constexpr int kStripH = 40;  // its 3 px rule included
constexpr int kDarkH = 128;
constexpr int kFootH = 153;  // its 3 px rule included
constexpr int kPad = 18;
constexpr int kStoryTop = kStripH + kDarkH;
constexpr int kStoryBottom = 800 - kFootH;
constexpr TextStyle kStripTag{&jb::kMono13ExtraBold, 1};
constexpr TextStyle kStrip{&jb::kMono13Bold, 1};
constexpr TextStyle kClock{&jb::kMono80ExtraBold, -4};  // digits and ":-"
constexpr TextStyle kTemp{&jb::kMono40ExtraBold, -1};   // digits, minus, degree
constexpr TextStyle kWx{&jb::kMono14Bold, 1};
constexpr TextStyle kWxBold{&jb::kMono14ExtraBold, 1};
constexpr TextStyle kLabel{&jb::kMono13Bold, 1};
constexpr TextStyle kTag{&jb::kMono13ExtraBold, 1};
constexpr TextStyle kHead{&jb::kMono28ExtraBold, 0};     // capitals
constexpr int kHeadLineH = 36;
constexpr int kHeadLines = 4;
constexpr TextStyle kHeadBig{&jb::kMono38ExtraBold, 0};  // capitals
constexpr int kHeadBigLineH = 46;
constexpr int kHeadBigLines = 6;
constexpr TextStyle kSummary{&jb::kMono15Bold, 0};
constexpr int kSummaryLineH = 23;
constexpr int kSummaryLines = 9;
constexpr int kMaxCells = 16;  // more stories than this: a bar instead of a cell each

// --- The pixel art ---------------------------------------------------------------------------

// The frog, front on: bulging eyes looking down, a wide smile, the paper held up in both hands,
// its feet under it. '#' ink, '+' 50 % dither (its skin), 'o' paper. In a field of 19 x 16
// cells of 8 px, with an unlit dot in every empty cell and a 25 % shadow down and right, as on
// Sleep.
const char *const kFrog[] = {
    "  ###     ###  ",  //
    " #ooo#   #ooo# ",  //
    " #o#o#####o#o# ",  //
    "#+++++++++++++#",  //
    "#+#+++++++++#+#",  //
    "#++#########++#",  //
    " #+++++++++++# ",  //
    " ############# ",  //
    "+#o#########o#+",  //
    "+#ooooo#ooooo#+",  //
    " #o+++o#o+++o# ",  //
    " #o+++o#o+++o# ",  //
    " ############# ",  //
    " # # #   # # # ",  //
};
constexpr int kFrogRows = sizeof kFrog / sizeof *kFrog;
constexpr int kFrogX = 2;  // its place in the field, in cells
constexpr int kFrogY = 1;
constexpr int kFieldCols = 19;
constexpr int kFieldRows = 16;
constexpr int kFieldCell = 8;

// The forecast's icons, 8 x 7 cells of 6 px, paper on the dark block: '#' paper, '+' 50 %.
constexpr int kIconCols = 8;
constexpr int kIconRows = 7;
constexpr int kIconCell = 6;
const char *const kIconSun[] = {"   ++   ", " + ## + ", "  ####  ", "+######+",
                                "  ####  ", " + ## + ", "   ++   "};
const char *const kIconMoon[] = {"  ###   ", " ##     ", "##      ", "##      ",
                                 "##      ", " ##     ", "  ###   "};
const char *const kIconPartly[] = {" ++     ", "++++##  ", "++#  ## ", " #     #",
                                   "#      #", "########", "        "};
const char *const kIconCloud[] = {"        ", "   ##   ", "  #  ## ", " #     #",
                                  "#      #", "########", "        "};
const char *const kIconFog[] = {"   ##   ", "  #  ## ", " #     #", "########",
                                "        ", "+++++++ ", " +++++++"};
const char *const kIconRain[] = {"   ##   ", "  #  ## ", " #     #", "#      #",
                                 "########", "  # # # ", " # # #  "};
const char *const kIconSnow[] = {"   ##   ", "  #  ## ", " #     #", "#      #",
                                 "########", " #   #  ", "   #   #"};
const char *const kIconStorm[] = {"   ##   ", "  #  ## ", " #     #", "#      #",
                                  "########", "   ##   ", "  ##    "};

const char *const *iconFor(weather::Sky sky, bool day) {
  switch (sky) {
    case weather::Sky::Clear: return day ? kIconSun : kIconMoon;
    case weather::Sky::Partly: return kIconPartly;
    case weather::Sky::Fog: return kIconFog;
    case weather::Sky::Showers:
    case weather::Sky::Rain: return kIconRain;
    case weather::Sky::Snow: return kIconSnow;
    case weather::Sky::Storm: return kIconStorm;
    default: return kIconCloud;
  }
}

bool wet(weather::Sky s) {
  return s == weather::Sky::Showers || s == weather::Sky::Rain || s == weather::Sky::Storm ||
         s == weather::Sky::Snow;
}

// --- What the face shows, kept in NVS --------------------------------------------------------

constexpr uint32_t kMagic = 0x4E464331;  // "NFC1"

enum class Kind : uint8_t { Story, Quiet, NoFeeds, NoCard };
enum class Mode : uint8_t { Live, Usb, Stopped };

// The story on the face, and where it is in the cycle. Written when it changes, so the minute
// wakes in between redraw it without the card.
struct Card {
  uint32_t magic;
  uint32_t uid;
  int64_t shown;  // when it came up (the start of that minute)
  int16_t pos;
  int16_t count;
  Kind kind;
  uint8_t fresh;  // not opened yet: NEW
  char feed[40];
  char when[16];
  char len[12];
  char title[240];
  char summary[404];
};

// The rest of the face, each minute.
struct Clock {
  uint32_t magic;
  int64_t now;
  int64_t synced;    // the newest fetch of a feed (news::lastSync())
  int64_t fetchAt;   // when the next one comes; 0 for none
  int64_t lastFull;  // the last full refresh
  Mode mode;
  int8_t battery;    // %, for STOPPED
};

std::atomic<bool> requested{false};

uint32_t loadU32(const char *key) { return kv::loadU32(kNvs, key); }
void saveU32(const char *key, uint32_t v) { kv::saveU32(kNvs, key, v); }

template <typename T>
bool loadBlob(const char *key, T &out) {
  return kv::load(kNvs, key, out) && out.magic == kMagic;
}

template <typename T>
void saveBlob(const char *key, const T &v) {
  kv::save(kNvs, key, v);
}

using power::lowBattery;
using power::pwrHeld;

time_t every() { return std::max(1, settings::newsFaceEvery()) * 60; }

time_t minuteStart(time_t t) { return t - t % 60; }

std::string dot() { return std::string(" ") + glyph::kDot + " "; }

std::string fmt(time_t t, const char *pattern) {
  tm l;
  localtime_r(&t, &l);
  char buf[32];
  strftime(buf, sizeof buf, pattern, &l);
  return ui::upper(buf);
}

// "05:12" today, "YDA 23:50" yesterday, else "18 NOV".
std::string shortWhen(time_t t, time_t now) {
  tm a, b, c;
  localtime_r(&t, &a);
  localtime_r(&now, &b);
  if (timeutil::sameDay(a, b)) return fmt(t, "%H:%M");
  const time_t y = now - 86400;
  localtime_r(&y, &c);
  if (timeutil::sameDay(a, c)) return fmt(t, "YDA %H:%M");
  return fmt(t, "%d %b");
}

// Copies at most n - 1 bytes, not splitting a UTF-8 character.
void put(char *dst, size_t n, const std::string &s) {
  size_t len = std::min(s.size(), n - 1);
  while (len > 0 && len < s.size() && (static_cast<uint8_t>(s[len]) & 0xC0) == 0x80) --len;
  memcpy(dst, s.data(), len);
  dst[len] = 0;
}

// The stories of the last 12 hours, newest first. Not ones dated ahead (BBC News lists
// programmes before they air), beyond a clock's worth of skew.
std::vector<StoryInfo> recent(time_t now) {
  std::vector<StoryInfo> out;
  for (StoryInfo &s : news::stories(-1, news::Show::All)) {
    if (s.published >= now - kWindowS && s.published <= now + kAheadS) out.push_back(std::move(s));
  }
  std::stable_sort(out.begin(), out.end(), [](const StoryInfo &a, const StoryInfo &b) {
    return a.published > b.published;
  });
  return out;
}

// Story `pos` of the list, or why there's none.
Card cardFor(const std::vector<StoryInfo> &list, int pos, time_t now) {
  Card c = {};
  c.magic = kMagic;
  c.shown = minuteStart(now);
  if (list.empty()) {
    c.kind = !sdcard::mounted() ? Kind::NoCard : !news::hasFeeds() ? Kind::NoFeeds : Kind::Quiet;
    return c;
  }
  pos = std::max(0, std::min(pos, static_cast<int>(list.size()) - 1));
  const StoryInfo &s = list[pos];
  c.kind = Kind::Story;
  c.uid = s.uid;
  c.pos = static_cast<int16_t>(pos);
  c.count = static_cast<int16_t>(std::min<size_t>(list.size(), INT16_MAX));
  c.fresh = s.read ? 0 : 1;
  put(c.feed, sizeof c.feed, ui::upper(s.feedTitle));
  put(c.when, sizeof c.when, shortWhen(s.published, now));
  put(c.len, sizeof c.len, std::to_string(std::max<uint32_t>(1, (s.words + 100) / 200)) + " MIN");
  put(c.title, sizeof c.title, s.title);
  put(c.summary, sizeof c.summary, s.summary);
  return c;
}

// The story after `card`; at the end of a lap the newest again, with *lapEnd set.
Card nextCard(const Card &card, time_t now, bool *lapEnd) {
  const std::vector<StoryInfo> list = recent(now);
  int next = 0;
  bool end = card.kind != Kind::Story;
  if (!end) {
    int at = -1;
    for (size_t i = 0; i < list.size(); ++i) {
      if (list[i].uid == card.uid) at = static_cast<int>(i);
    }
    // Gone from the list: it aged out at the old end, so the lap is over.
    next = at >= 0 ? at + 1 : static_cast<int>(list.size());
    end = next >= static_cast<int>(list.size());
  }
  if (lapEnd) *lapEnd = end;
  return cardFor(list, end ? 0 : next, now);
}

time_t lastTry(time_t synced) { return std::max<time_t>(synced, loadU32("tried")); }

// When the next fetch comes: the end of the first lap at least an hour after the last try. With
// nothing to cycle, an hour after it. 0 with no feeds or no card.
time_t fetchAtFor(const Card &card, time_t tried, time_t now) {
  if (card.kind == Kind::NoFeeds || card.kind == Kind::NoCard) return 0;
  const time_t earliest = tried + kFetchEveryS;
  if (card.kind != Kind::Story) return std::max(earliest, minuteStart(now) + 60);
  const time_t lap = static_cast<time_t>(card.count) * every();
  time_t end = card.shown + static_cast<time_t>(card.count - card.pos) * every();
  if (lap > 0 && end < earliest) end += ((earliest - end + lap - 1) / lap) * lap;
  return end;
}

Clock clockFor(const Card &card, Mode mode, time_t now, time_t synced, time_t lastFull) {
  Clock k = {};
  k.magic = kMagic;
  k.now = now;
  k.mode = mode;
  k.synced = synced;
  k.fetchAt = mode == Mode::Live ? fetchAtFor(card, lastTry(synced), now) : 0;
  k.lastFull = lastFull;
  k.battery = static_cast<int8_t>(pmic::status().percent);
  return k;
}

// --- Drawing ---------------------------------------------------------------------------------

std::string hhmm(time_t t) { return rtc::timeValid() ? fmt(t, "%H:%M") : "--:--"; }


// Paper pixel art on ink: '#' paper, '+' a 50 % tone.
void drawIcon(Canvas &c, const char *const *rows, int x0, int y0) {
  for (int y = 0; y < kIconRows; ++y) {
    for (int x = 0; rows[y][x] && x < kIconCols; ++x) {
      const Rect r{x0 + x * kIconCell, y0 + y * kIconCell, kIconCell, kIconCell};
      if (rows[y][x] == '#') {
        ui::fill(c, r, ui::kPaper);
      } else if (rows[y][x] == '+') {
        ui::fill(c, r, ui::kPaper);  // an ink checkerboard over paper
        ui::dither50(c, r);
      }
    }
  }
}

char frogAt(int x, int y) {
  x -= kFrogX;
  y -= kFrogY;
  if (y < 0 || y >= kFrogRows || x < 0 || x >= static_cast<int>(strlen(kFrog[y]))) return 0;
  return kFrog[y][x] == ' ' ? 0 : kFrog[y][x];
}

void drawField(Canvas &c, int x0, int y0) {
  ui::drawDotField(c, x0, y0, kFieldCols, kFieldRows, kFieldCell, frogAt);
}

void drawStrip(Canvas &c, const Clock &k) {
  using namespace ui;
  const Rect top{0, 0, kW, kStripH - kRule};
  const char *tag = "INKDECK/OS";
  const int tagW = textWidth(kStripTag, tag) + 2 * kPadX;
  fill(c, {top.x, top.y, tagW, top.h}, kInk);
  drawText(c, {top.x + kPadX, top.y, tagW - 2 * kPadX, top.h}, tag, kStripTag, Align::Left,
           kPaper);
  std::string text = "NEWS";
  if (rtc::timeValid()) text += dot() + fmt(k.now, "%a %d %b");
  if (k.synced) text += dot() + "SYNCED " + shortWhen(k.synced, k.now);
  drawText(c, {top.x + tagW + kPadX, top.y, top.w - tagW - 2 * kPadX, top.h}, text, kStrip);
  hrule(c, 0, top.bottom(), kW);
}

// The dark block: the time, and the forecast for now beside it.
void drawDark(Canvas &c, const Clock &k) {
  using namespace ui;
  const Rect area{0, kStripH, kW, kDarkH};
  fill(c, area, kInk);
  const bool stopped = k.mode == Mode::Stopped;
  const int capH = capHeight(kClock);
  constexpr int kTagH = 19, kTagGap = 8;
  int y = area.y + (area.h - ((stopped ? kTagH + kTagGap : 0) + capH)) / 2;
  if (stopped) {
    drawTag(c, kPad, y + kTagH / 2, kTagH, "STOPPED AT", kTag, TagKind::Inverted, 6, kPaper);
    y += kTagH + kTagGap;
  }
  const int clockRight = drawText(c, kPad, y + capH, hhmm(k.now), kClock, kPaper);

  // The forecast: icon and temperature, the sky, high and low, the next rain.
  if (!rtc::timeValid()) return;
  std::unique_ptr<weather::Forecast> f(new weather::Forecast);
  if (!weather::get(*f)) return;
  const weather::Hour *h = weather::at(*f, k.now);
  if (!h) return;
  const weather::Sky sky = weather::sky(h->code);
  std::vector<std::pair<std::string, const TextStyle *>> rows;
  rows.push_back({weather::condition(h->code), &kWxBold});
  const weather::Day *days[1] = {};
  if (weather::daysFrom(*f, k.now, days, 1) > 0) {
    rows.push_back({"HI " + temperatureText(days[0]->hiC) + dot() + "LO " + temperatureText(days[0]->loC), &kWx});
  }
  if (!wet(sky)) {
    const weather::Hour *next[12] = {};
    const int n = weather::hoursAfter(*f, k.now, next, 12);
    for (int i = 0; i < n; ++i) {
      const weather::Sky s = weather::sky(next[i]->code);
      if (!wet(s)) continue;
      rows.push_back({std::string(s == weather::Sky::Snow ? "SNOW " : "RAIN ") +
                          fmt(next[i]->time, "%H:%M"),
                      &kWx});
      break;
    }
  }
  const std::string temp = temperatureText(h->tempC);
  constexpr int kIconW = kIconCols * kIconCell, kIconH = kIconRows * kIconCell;
  int colW = kIconW + 12 + textWidth(kTemp, temp);
  for (const auto &r : rows) colW = std::max(colW, textWidth(*r.second, r.first));
  const int colX = std::max(clockRight + 16, area.right() - kPad - colW);
  const int w = area.right() - kPad - colX;
  if (w < kIconW) return;
  constexpr int kRowH = 17, kRowGap = 3;
  const int rowsH = kIconH + 6 + static_cast<int>(rows.size()) * (kRowH + kRowGap) - kRowGap;
  int ry = area.y + (area.h - rowsH) / 2;
  drawIcon(c, iconFor(sky, h->day), colX, ry);
  drawText(c, colX + kIconW + 12, ry + (kIconH + capHeight(kTemp)) / 2, temp, kTemp, kPaper);
  ry += kIconH + 6;
  for (const auto &r : rows) {
    drawText(c, {colX, ry, w, kRowH}, r.first, *r.second, Align::Left, kPaper);
    ry += kRowH + kRowGap;
  }
}

// The story, ink on paper; or why there's none.
void drawStory(Canvas &c, const Card &card, const Clock &k) {
  using namespace ui;
  const int x = kPad, w = kW - 2 * kPad;
  int y = kStoryTop + 18;
  auto drawLines = [&](const std::vector<std::string> &lines, const TextStyle &s, int lineH) {
    for (const std::string &line : lines) {
      drawText(c, x, y + (lineH + capHeight(s)) / 2 - 1, line, s);
      y += lineH;
    }
  };
  auto shortRule = [&] {
    y += 10;
    hrule(c, x, y, 96);
    y += kRule + 14;
  };
  if (card.kind != Kind::Story) {
    const char *label = card.kind == Kind::Quiet ? "// LAST 12 HOURS" : "// NEWS";
    const char *big = card.kind == Kind::Quiet     ? "NOTHING NEW"
                      : card.kind == Kind::NoFeeds ? "NO FEEDS"
                                                   : "NO CARD";
    std::string text;
    if (card.kind == Kind::Quiet) {
      text = "No story in your feeds since " + hhmm(k.now - kWindowS) + ".";
      if (k.fetchAt) text += " The frog checks again at " + hhmm(k.fetchAt) + ".";
    } else if (card.kind == Kind::NoFeeds) {
      text = "Add feeds to SD:/News/feeds.txt, one address a line.";
    } else {
      text = "News keeps its feeds on the microSD card.";
    }
    drawText(c, {x, y, w, 18}, label, kLabel);
    y += 18 + 12;
    drawLines({big}, kHeadBig, kHeadBigLineH);
    shortRule();
    drawLines(wrap(kSummary, text, w, kSummaryLines), kSummary, kSummaryLineH);
    return;
  }

  // Feed and time; NEW and the reading time on the right.
  const std::string len = card.len;
  const int lenW = textWidth(kLabel, len);
  int right = x + w - lenW;
  drawText(c, {right, y, lenW, 18}, len, kLabel);
  if (card.fresh) {
    right -= 8 + tagWidth("NEW", kTag, 6);
    drawTag(c, right, y + 9, 19, "NEW", kTag, TagKind::Inverted, 6);
  }
  drawText(c, {x, y, right - 10 - x, 18}, std::string(card.feed) + dot() + card.when, kLabel);
  y += 18 + 12;

  const std::string title = upper(card.title);
  if (!card.summary[0]) {  // a feed with no summary: the headline larger
    drawLines(wrap(kHeadBig, title, w, kHeadBigLines), kHeadBig, kHeadBigLineH);
    shortRule();
    drawText(c, {x, y, w, 18}, "NO SUMMARY IN THE FEED", kLabel);
    return;
  }
  drawLines(wrap(kHead, title, w, kHeadLines), kHead, kHeadLineH);
  shortRule();
  const int lines = std::min(kSummaryLines, (kStoryBottom - 16 - y) / kSummaryLineH);
  if (lines > 0) drawLines(wrap(kSummary, card.summary, w, lines), kSummary, kSummaryLineH);
}

// The foot: the frog, the cycle, the next fetch, the way back.
// Live on USB with the comms mic on (it stays on there; issue #66): the foot's band says MIC ON.
// Set by the face before it draws.
bool micShown = false;

void drawFoot(Canvas &c, const Card &card, const Clock &k) {
  using namespace ui;
  const int top = kStoryBottom;
  hrule(c, 0, top, kW);
  const int inY = top + kRule + 11, inH = kFootH - kRule - 22;
  drawField(c, kPad, inY + (inH - kFieldRows * kFieldCell) / 2);
  const int x = kPad + kFieldCols * kFieldCell + kPad, w = kW - kPad - x;
  int y = inY + 4;
  constexpr int kRowH = 17, kGap = 10;

  // Where the cycle is: a cell a story (the one up taller), or a bar for many.
  if (card.kind == Kind::Story) {
    int tx = x;
    if (card.count <= kMaxCells) {
      for (int i = 0; i < card.count; ++i) {
        if (i < card.pos) {
          fill(c, {tx, y + 4, 9, 9}, kInk);
        } else if (i == card.pos) {
          fill(c, {tx, y, 9, kRowH}, kInk);
        } else {
          frame(c, {tx, y + 4, 9, 9}, 2);
        }
        tx += 12;
      }
    } else {
      drawProgress(c, {tx, y + 3, 150, 11}, (card.pos + 1) / static_cast<float>(card.count));
      tx += 150 + 3;
    }
    char pos[16];
    snprintf(pos, sizeof pos, "%02d/%02d", card.pos + 1, card.count);
    drawText(c, {tx + 7, y, x + w - tx - 7, kRowH}, pos, kLabel);
  } else if (card.kind == Kind::Quiet) {
    drawText(c, {x, y, w, kRowH}, "NONE IN 12 H", kLabel);
  }
  y += kRowH + kGap;

  std::string next;
  if (k.mode == Mode::Stopped) {
    next = "NO MORE FETCHES";
  } else if (k.mode == Mode::Usb) {
    next = "FETCHES HOURLY";
  } else if (k.fetchAt) {
    next = "FETCH " + hhmm(k.fetchAt);
  }
  if (!next.empty()) drawText(c, {x, y, w, kRowH}, next, kLabel);
  y += kRowH + kGap;

  std::string band;
  if (k.mode == Mode::Usb) band = (micShown ? "MIC ON" : "LIVE") + dot() + "USB";
  if (k.mode == Mode::Stopped && k.battery >= 0) {
    band = "BATTERY " + std::to_string(k.battery) + "%";
  }
  if (!band.empty()) drawTag(c, x, y + 9, 19, band, kTag, TagKind::Inverted, 6);

  // HOLD [PWR] TO READ, at the bottom.
  drawHoldPwr(c, {x, inY + inH - 24, w, 24}, "TO READ");
}

void drawFace(Canvas &c, const Clock &k, const Card &card) {
  drawStrip(c, k);
  drawDark(c, k);
  drawStory(c, card, k);
  drawFoot(c, card, k);
}

bool sameStory(const Card &a, const Card &b) { return a.kind == b.kind && a.uid == b.uid; }

// --- The face, awake -------------------------------------------------------------------------

class FaceScreen;
FaceScreen *face = nullptr;

class FaceScreen : public OffFaceScreen {
 public:
  FaceScreen(bool usb, bool devWake) : usb_(usb), devWake_(devWake) {
    face = this;
    const time_t now = time(nullptr);
    card_ = cardFor(recent(now), 0, now);
    clock_ = clockFor(card_, mode(), now, news::lastSync(), now);
    gen_ = news::generation();
  }
  ~FaceScreen() override {
    if (face == this) face = nullptr;
  }

  std::string tag() const override { return "NEWS"; }

  bool onTick() override {
    const time_t now = time(nullptr);
    if (usb_) {
      const pmic::Status p = pmic::status();
      if (p.ok && !p.vbus) {  // unplugged: off behind it, as on battery
        usb_ = false;
        restartTicks();
        clock_ = clockFor(card_, mode(), now, news::lastSync(), now);
        ui::nav::invalidate(ui::Refresh::Full);
        return true;
      }
      // Live: the clock each minute, the next story when it's time, NOTHING NEW until a sync
      // brings something.
      const uint32_t gen = news::generation();
      const bool minute = minuteStart(now) != minuteStart(clock_.now);
      const bool mic = ui::nav::micMark() != ui::MicMark::None;
      if (mic != micShown) return true;  // the band: MIC ON or LIVE
      if (!minute && gen == gen_) return false;
      Card next = card_;
      if (card_.kind != Kind::Story && gen != gen_) {
        next = cardFor(recent(now), 0, now);
      } else if (card_.kind == Kind::Story && now >= card_.shown + every() - 30) {
        next = nextCard(card_, now, nullptr);
      }
      gen_ = gen;
      const bool changed = !sameStory(next, card_);
      if (!minute && !changed) return false;
      card_ = next;
      time_t lastFull = clock_.lastFull;
      if ((changed && now - lastFull >= kFullGapS) || now - lastFull >= kFullEveryS) {
        lastFull = now;
        ui::nav::invalidate(ui::Refresh::Full);
      }
      clock_ = clockFor(card_, mode(), now, news::lastSync(), lastFull);
      return true;
    }
    // On battery: the second tick after it's drawn, off, with a wake the next minute.
    return offTick();
  }

  void powerOff() override {
    const time_t now = time(nullptr);
    const time_t alarm = rtc::timeValid() ? alarms::next(now) : 0;
    const time_t wakeAt =
        clock_.mode == Mode::Stopped || !rtc::timeValid() ? 0 : minuteStart(now) + 60;
    saveBlob("card", card_);
    saveBlob("clock", clock_);
    saveU32("next", static_cast<uint32_t>(wakeAt));
    if (devWake_) saveU32("dev", 3);  // the dev tool's: three wakes, then into News
    power::off(power::Mode::Edition, withAgentCheck(power::earliest(alarm, wakeAt)));
  }

  void draw(Canvas &c, const Rect &) override {
    micShown = usb_ && ui::nav::micMark() != ui::MicMark::None;
    drawFace(c, clock_, card_);
    drawn();
  }

  uint32_t story() const { return card_.kind == Kind::Story ? card_.uid : 0; }

 private:
  Mode mode() const { return usb_ ? Mode::Usb : lowBattery() ? Mode::Stopped : Mode::Live; }

  bool usb_;
  bool devWake_;
  Card card_ = {};
  Clock clock_ = {};
  uint32_t gen_ = 0;
};

// A fetch from behind the face: the feeds, then the forecast. True if PWR was pressed (boot
// into News; the sync finishes in the background).
bool fetchNow() {
  saveU32("tried", static_cast<uint32_t>(time(nullptr)));
  if (!news::hasFeeds() || !wlan::configured()) return false;
  net::begin();
  news::sync();
  const uint32_t start = millis();
  bool stopping = false;
  while (news::busy()) {
    if (pwrHeld()) return true;
    const uint32_t took = millis() - start;
    if (!stopping && took > kSyncMs) {
      news::cancel();
      stopping = true;
    }
    if (took > kSyncMs + kStopMs) break;
    delay(100);
  }
  log_i("news face: fetched in %lu ms (%s)", static_cast<unsigned long>(millis() - start),
        news::activity().text.c_str());
  if (pwrHeld()) return true;
  if (weather::configured()) weather::syncInStandby(1);
  return pwrHeld();
}

}  // namespace

bool editionWanted() { return newsOnScreen(); }

void showEdition(bool usb, bool devWake) {
  const ui::Screen *top = ui::nav::top();
  if ((top && top->offFace()) || deskShowing() || face) return;
  if (audio::status().state == audio::State::Playing) audio::togglePause();  // "off" is quiet
  lockNow();  // under the face: waking it lands on the lock (power_on)
  ui::nav::push(std::make_unique<FaceScreen>(usb, devWake));
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

uint32_t faceStory() {
  if (face) return face->story();
  Card card;
  return loadBlob("card", card) && card.kind == Kind::Story ? card.uid : 0;
}

bool editionWake() {
  if (power::wake() != power::Wake::Alarm || !rtc::timeValid()) return false;
  if (power::lastMode() != power::Mode::Edition) return false;
  const time_t now = time(nullptr);
  const time_t ring = alarms::nextAtWake(now);
  if (ring && ring <= now + 1) return false;  // an alarm: boot, and it rings
  const time_t due = static_cast<time_t>(loadU32("next"));
  if (due == 0 || now + 30 < due) {
    // Woken for something else (an alarm snoozed or deleted since): back off until it's time.
    power::off(power::Mode::Edition, withAgentCheck(power::earliest(ring, due)));
    return false;
  }
  pinMode(board::PWR_KEY_SENSE, INPUT);
  if (pwrHeld()) return true;

  Card oldCard = {};
  Clock old = {};
  const bool haveCard = loadBlob("card", oldCard);
  const bool haveOld = haveCard && loadBlob("clock", old);
  Card card = oldCard;
  const bool low = lowBattery();
  time_t synced = haveOld ? old.synced : 0;

  // The next story when it's time, or with nothing to show an hourly fetch. Only then the card:
  // the minutes in between redraw from NVS.
  const bool due2 = !haveCard ||
                    (card.kind == Kind::Story && now >= card.shown + every() - 30) ||
                    (card.kind == Kind::Quiet && now >= lastTry(synced) + kFetchEveryS - 30);
  const bool library = !haveCard || (due2 && !low);
  if (library) {
    sdcard::mount();
    news::load(false);
    bool lapEnd = false;
    card = haveCard ? nextCard(oldCard, now, &lapEnd) : cardFor(recent(now), 0, now);
    if (!low && lapEnd && now >= lastTry(news::lastSync()) + kFetchEveryS - 30) {
      if (fetchNow()) return true;
      const time_t t = time(nullptr);
      card = cardFor(recent(t), 0, t);
    }
    synced = news::lastSync();
  }
  if (pwrHeld()) return true;

  const time_t t = time(nullptr);
  const bool changed = !haveCard || !sameStory(card, oldCard);
  time_t lastFull = haveOld ? old.lastFull : 0;
  const bool full =
      !haveOld || (changed && t - lastFull >= kFullGapS) || t - lastFull >= kFullEveryS;
  if (full) lastFull = t;
  const Clock k = clockFor(card, low ? Mode::Stopped : Mode::Live, t, synced, lastFull);

  // The panel still shows the last face: draw it as the base, so this is a partial refresh.
  Canvas &c = screen::canvas();
  screen::presentOver(false, full, [&] { drawFace(c, old, oldCard); },
                      [&] { drawFace(c, k, card); }, pmic::status().vbus);
  if (changed) saveBlob("card", card);
  saveBlob("clock", k);

  const uint32_t dev = loadU32("dev");
  if (dev == 1) {  // the dev tool's last wake: as HOLD PWR, into News on the story
    saveU32("dev", 0);
    saveU32("next", 0);
    log_i("news face: dev wakes done, opening the story");
    return true;
  }
  if (dev > 1) saveU32("dev", dev - 1);
  const time_t next = low ? 0 : minuteStart(t) + 60;
  saveU32("next", static_cast<uint32_t>(next));
  power::off(power::Mode::Edition, withAgentCheck(power::earliest(ring, next)));
  return false;
}

void requestEdition() { requested = true; }

void pollEdition() {
  if (requested.exchange(false)) showEdition(false, true);
}

}  // namespace apps
