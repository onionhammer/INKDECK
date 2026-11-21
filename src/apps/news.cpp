#include "apps/news.h"

#include <Arduino.h>
#include <Preferences.h>
#include <ctype.h>
#include <stdio.h>
#include <time.h>

#include <algorithm>

#include "apps/home.h"
#include "apps/idle.h"
#include "apps/news_edition.h"
#include "fonts.h"
#include "input.h"
#include "news_library.h"
#include "notedoc.h"
#include "pmic.h"
#include "screen.h"
#include "sdcard.h"
#include "ui/menu.h"
#include "wlan.h"

namespace apps {
namespace {

using news::StoryInfo;
using news::Uid;
using ui::Align;
using ui::Canvas;
using ui::Hint;
using ui::Key;
using ui::Rect;
using ui::TextStyle;
namespace glyph = ui::glyph;
namespace jb = ui::fonts;

// Sizes and weights from the News artboards.
constexpr TextStyle kTitleNew{&jb::kMono15ExtraBold, 0};  // a story not opened yet
constexpr TextStyle kTitleRead{&jb::kMono15Medium, 0};
constexpr TextStyle kMeta{&jb::kMono13Medium, 0};
constexpr TextStyle kRight{&jb::kMono13Bold, 0};
constexpr TextStyle kRightBold{&jb::kMono13ExtraBold, 0};
constexpr TextStyle kCardLine{&jb::kMono13Bold, 1};
constexpr TextStyle kCardTitle{&jb::kMono18ExtraBold, 0};
constexpr TextStyle kCardText{&jb::kMono14Medium, 0};
constexpr TextStyle kFeedName{&jb::kMono15Bold, 0};
constexpr TextStyle kFeedNameSelected{&jb::kMono15ExtraBold, 0};

constexpr int kRowH = 46;        // a two-line row: stories, feeds
constexpr int kStripH = 30;      // the strip at the foot, its 3 px rule included
constexpr int kCardMargin = 14;
constexpr int kStoryCardH = 196;
constexpr int kMarkW = 14;       // the ▶ / new-square column
constexpr int kFeedRowsMax = 9;
constexpr int kFeedCardMaxH = 228;
constexpr uint32_t kOpenSyncS = 30 * 60;  // opening News syncs feeds older than this
constexpr uint32_t kWordsPerMinute = 200;
constexpr uint32_t kBusyRedrawMs = 5000;  // while syncing, redraw at most this often

int liveScreens = 0;                    // News screens in existence
Uid lastOpened = 0;                     // the story read last, for the list to come back to
news::Show lastShow = news::Show::All;  // the stories' filter, kept for this boot
int lastMenuItem = 0;

// --- Words --------------------------------------------------------------------------------------

std::string dot() { return std::string(" ") + glyph::kDot + " "; }

bool sameDay(const tm &a, const tm &b) { return a.tm_year == b.tm_year && a.tm_yday == b.tm_yday; }

std::string hhmm(time_t t) {
  tm l;
  localtime_r(&t, &l);
  char buf[8];
  strftime(buf, sizeof buf, "%H:%M", &l);
  return buf;
}

// TODAY, YESTERDAY, "WED 18 NOV" within the week, else "18 NOV" (and the year if not this one).
std::string dayLabel(time_t t) {
  const time_t now = time(nullptr);
  tm a, b, c;
  localtime_r(&t, &a);
  localtime_r(&now, &b);
  if (sameDay(a, b)) return "TODAY";
  const time_t yesterday = now - 86400;
  localtime_r(&yesterday, &c);
  if (sameDay(a, c)) return "YESTERDAY";
  char buf[24];
  if (t < now && now - t < 6 * 86400) {
    strftime(buf, sizeof buf, "%a %d %b", &a);
  } else {
    strftime(buf, sizeof buf, a.tm_year == b.tm_year ? "%d %b" : "%d %b %Y", &a);
  }
  return ui::upper(buf);
}

// "TODAY 14:05", "YDA 21:44", "18 NOV 21:44".
std::string whenText(time_t t) {
  if (!t) return {};
  std::string day = dayLabel(t);
  if (day == "YESTERDAY") {
    day = "YDA";
  } else if (day.size() > 4 && day[3] == ' ' && isalpha(static_cast<unsigned char>(day[0]))) {
    day = day.substr(4);  // "WED 18 NOV" -> "18 NOV"
  }
  return day + " " + hhmm(t);
}

std::string minutesText(uint32_t words) {
  const uint32_t m = std::max<uint32_t>(1, (words + kWordsPerMinute / 2) / kWordsPerMinute);
  return std::to_string(m) + " MIN";
}

std::string wordsText(uint32_t words) {  // "1,840 WORDS"
  std::string n = std::to_string(words);
  for (int i = static_cast<int>(n.size()) - 3; i > 0; i -= 3) n.insert(i, ",");
  return n + (words == 1 ? " WORD" : " WORDS");
}

// "SYNCED 14:20", "SYNCED YDA 21:44", "SYNCING 3/8", "NOT SYNCED YET".
std::string syncText() {
  const news::Activity a = news::activity();
  if (!a.text.empty()) return a.text;
  const time_t t = news::lastSync();
  if (!t) return "NOT SYNCED YET";
  const std::string w = whenText(t);
  return "SYNCED " + (w.rfind("TODAY ", 0) == 0 ? w.substr(6) : w);
}

// "citydesk.example/2026/11/night-trains": no scheme, www, query or trailing slash.
std::string linkText(const std::string &url) {
  std::string s = url;
  size_t p = s.find("://");
  if (p != std::string::npos) s.erase(0, p + 3);
  if (s.rfind("www.", 0) == 0) s.erase(0, 4);
  p = s.find_first_of("?#");
  if (p != std::string::npos) s.resize(p);
  while (!s.empty() && s.back() == '/') s.pop_back();
  return s;
}

const char *showName(news::Show s) {
  switch (s) {
    case news::Show::New: return "NEW";
    case news::Show::Saved: return "SAVED";
    case news::Show::All: break;
  }
  return "ALL";
}

// "8", "8 · 1 FAILING".
std::string feedsText() {
  const int n = news::feedCount(), failing = news::failingCount();
  return std::to_string(n) + (failing ? dot() + std::to_string(failing) + " FAILING" : "");
}

// --- Pieces ---------------------------------------------------------------------------------

// The card under a list: a 3 px box with a heading line and tags, a title, a line under it,
// some text and a footer with the ● verb (the Podcasts card's layout).
struct Card {
  std::string line, title, subline, text, footer;
  std::string tag;  // right of the line
  bool tagInverted = false;
  std::string tag2;  // a second one, outlined, after it ("SAVED")
  const char *verb = nullptr;
};

void drawCard(Canvas &c, const Rect &box, const Card &k) {
  using namespace ui;
  if (box.h < 60) return;
  frame(c, box, kRule);
  const Rect in = box.inset(kRule + 14, kRule + 12);
  int y = in.y;
  int x = in.right();
  if (!k.tag2.empty()) {
    x -= tagWidth(k.tag2, kCardLine, 6);
    drawTag(c, x, y + 9, 19, k.tag2, kCardLine, TagKind::Outlined, 6);
    x -= 6;
  }
  if (!k.tag.empty()) {
    x -= tagWidth(k.tag, kCardLine, 6);
    drawTag(c, x, y + 9, 19, k.tag, kCardLine,
            k.tagInverted ? TagKind::Inverted : TagKind::Outlined, 6);
    x -= 6;
  }
  drawText(c, {in.x, y, x - in.x - 4, 18}, k.line, kCardLine);
  y += 18 + 8;
  // The footer first, so the text above knows where to stop.
  const int footerY = in.bottom() - 20;
  int verbW = 0;
  if (k.verb) {
    verbW = keycapWidth(glyph::kPress) + 6 + textWidth(kCardLine, k.verb);
    const int vx = in.right() - verbW;
    const int capW = drawKeycap(c, vx, footerY + 10, glyph::kPress);
    drawText(c, {vx + capW + 6, footerY, verbW - capW - 6, 20}, k.verb, kCardLine);
  }
  drawText(c, {in.x, footerY, in.w - verbW - 10, 20}, k.footer, kCardLine);
  for (const std::string &line : wrap(kCardTitle, k.title, in.w, 2)) {
    drawText(c, in.x, y + capHeight(kCardTitle) + 2, line, kCardTitle);
    y += 22;
  }
  if (!k.subline.empty()) {
    y += 2;
    drawText(c, {in.x, y, in.w, 18}, k.subline, kMeta);
    y += 18;
  }
  y += 6;
  const int lines = (footerY - 6 - y) / 20;
  if (lines > 0 && !k.text.empty()) {
    for (const std::string &line : wrap(kCardText, k.text, in.w, lines)) {
      drawText(c, in.x, y + capHeight(kCardText) + 3, line, kCardText);
      y += 20;
    }
  }
}

// A strip at the foot of the body: a 3 px rule, `left` and `right` in the card's small capitals.
void drawStrip(Canvas &c, const Rect &area, const std::string &left, const std::string &right) {
  using namespace ui;
  hrule(c, area.x, area.y, area.w);
  const Rect band{area.x + kPadX, area.y + kRule, area.w - 2 * kPadX, area.h - kRule};
  const int rightW = right.empty() ? 0 : textWidth(kCardLine, right);
  if (rightW) drawText(c, {band.right() - rightW, band.y, rightW, band.h}, right, kCardLine);
  drawText(c, {band.x, band.y, band.w - rightW - (rightW ? 12 : 0), band.h}, left, kCardLine);
}

void drawStoryRow(Canvas &c, const Rect &row, const StoryInfo &s, bool selected, bool withFeed) {
  using namespace ui;
  const uint16_t ink = selected ? kPaper : kInk;
  if (selected) {
    fill(c, row, kInk);
  } else {
    dashedH(c, row.x, row.bottom() - 1, row.w);
  }
  const int x = row.x + kPadX;
  if (selected) {
    drawText(c, {x, row.y, kMarkW, row.h}, glyph::kPlay, style::kCap, Align::Left, kPaper);
  } else if (s.fresh) {
    fill(c, {x + 1, row.centerY() - 4, 8, 8}, kInk);
  }
  const int right = row.right() - kPadX;
  int rightW = 0;
  if (s.saved) {
    rightW = tagWidth("SAVED", kRightBold, 5);
    drawTag(c, right - rightW, row.centerY(), 20, "SAVED", kRightBold, TagKind::Outlined, 5, ink);
  } else {
    const std::string m = minutesText(s.words);
    rightW = textWidth(kRight, m);
    drawText(c, {right - rightW, row.y, rightW, row.h}, m, kRight, Align::Left, ink);
  }
  const int tx = x + kMarkW + 8;
  const int tw = right - rightW - 10 - tx;
  drawText(c, {tx, row.y + 5, tw, 20}, s.title, s.fresh ? kTitleNew : kTitleRead, Align::Left, ink);
  std::string meta;
  if (withFeed) {
    meta = upper(s.feedTitle) + dot() + hhmm(s.published);
  } else {
    meta = hhmm(s.published) + (s.author.empty() ? "" : dot() + upper(s.author));
  }
  drawText(c, {tx, row.y + 24, tw, 18}, meta, kMeta, Align::Left, ink);
}

// --- Settings: the story's text size ----------------------------------------------------------

constexpr const char *kPrefs = "news";

uint8_t loadSize() {
  Preferences prefs;
  if (!prefs.begin(kPrefs, false)) return ::fonts::kDefaultReaderSize;
  const uint8_t v = prefs.isKey("size") ? prefs.getUChar("size") : ::fonts::kDefaultReaderSize;
  prefs.end();
  return v % ::fonts::kReaderSizeCount;
}

void saveSize(uint8_t size) {
  Preferences prefs;
  if (!prefs.begin(kPrefs, false)) return;
  prefs.putUChar("size", size);
  prefs.end();
}

std::unique_ptr<ui::Screen> makeStories(int feed);
std::unique_ptr<ui::Screen> makeFeeds();

// --- A story --------------------------------------------------------------------------------

// The Reader's layout (apps/note.cpp), with the chapter line naming the feed and the time.
constexpr int kPadTop = 22;
constexpr int kTextPadX = 30;
constexpr int kPadBottom = 8;
constexpr int kHeadLineH = 17;
constexpr int kHeadGap = 14;
constexpr TextStyle kHeadStyle{&jb::kMono13Bold, 2};
constexpr int kBodyH = screen::kHeight - ui::kStatusBarH - ui::kHintBarH;
constexpr int kTextW = screen::kWidth - 2 * kTextPadX;
constexpr int kTextH = kBodyH - kStripH - kPadTop - kHeadLineH - kHeadGap - kPadBottom;

class StoryScreen : public ui::Screen {
 public:
  StoryScreen(std::vector<Uid> order, int index)
      : order_(std::move(order)), index_(index), size_(loadSize()) {
    ++liveScreens;
    open();
  }
  ~StoryScreen() override { --liveScreens; }

  std::string tag() const override { return moduleTag("NEWS"); }
  std::string context() const override {
    char pos[16];
    snprintf(pos, sizeof pos, "%02d/%02d", index_ + 1, static_cast<int>(order_.size()));
    return pos + dot() + ui::upper(info_.feedTitle);  // the place first: a feed's name can be long
  }
  int hints(Hint *out) const override {
    out[0] = {glyph::kUp, "PREV"};
    out[1] = {glyph::kDown, lastPage() && hasNext() ? "NEXT STORY" : "NEXT"};
    out[2] = {glyph::kPress, "MENU"};
    out[3] = {"BOOT", "BACK"};
    return 4;
  }
  bool reading() const override { return true; }  // light sleep between pages

  bool onKey(Key key) override {
    switch (key) {
      case Key::Up: return turn(-1);
      case Key::Down:
        if (!lastPage()) return turn(+1);
        if (!hasNext()) return false;
        ++index_;
        open();
        return true;
      case Key::Select: openMenu(); return true;
      default: return false;
    }
  }

  void draw(Canvas &c, const Rect &body) override {
    using namespace ui;
    Rect r = body;
    drawProgress(c, r.takeBottom(kStripH));
    drawHeadLine(c, {r.x + kTextPadX, r.y + kPadTop, r.w - 2 * kTextPadX, kHeadLineH});
    const int textY = r.y + kPadTop + kHeadLineH + kHeadGap;
    if (doc_.empty()) {
      drawText(c, {r.x + kTextPadX, textY, kTextW, 40}, "NO TEXT", style::kLabel, Align::Center);
      return;
    }
    doc_.drawPage(c, page_, r.x + kTextPadX, textY);
  }

 private:
  bool hasNext() const { return index_ + 1 < static_cast<int>(order_.size()); }
  bool lastPage() const { return page_ + 1 >= doc_.pageCount(); }

  void open() {
    const Uid uid = order_[index_];
    info_ = StoryInfo{};
    news::story(uid, info_);
    std::string md;
    if (!news::body(uid, md)) md = info_.summary;
    if (!info_.full) {
      // The feed gives only this much: say so, and where the rest is.
      md = "## Summary only\n\n" + md;
      if (!info_.link.empty()) md += "\n\n---\n\nThe whole story: " + linkText(info_.link);
    }
    doc_.parse(info_.title, md);
    doc_.setByline(info_.author.empty() ? std::string() : "BY " + ui::upper(info_.author),
                   minutesText(info_.words) + dot() + wordsText(info_.words));
    layout(0);
    news::setRead(uid, true);
    lastOpened = uid;
  }

  void layout(uint32_t at) {
    doc_.layout(::fonts::kReader[size_], kTextW, kTextH);
    page_ = doc_.pageAt(at);
  }

  bool turn(int delta) {
    const int page = std::max(0, std::min(page_ + delta, doc_.pageCount() - 1));
    if (page == page_) return false;
    page_ = page;
    return true;
  }

  float progress() const { return static_cast<float>(page_ + 1) / doc_.pageCount(); }

  // "§ CITY DESK - - - - TODAY 14:05", like the Reader's chapter line.
  void drawHeadLine(Canvas &c, const Rect &line) const {
    using namespace ui;
    const std::string right = whenText(info_.published);
    const int rightW = right.empty() ? 0 : textWidth(kHeadStyle, right);
    const int base = baselineCentered(kHeadStyle, line.y, line.h);
    const std::string left = ellipsize(kHeadStyle,
                                       std::string(glyph::kSection) + " " + upper(info_.feedTitle),
                                       line.w - rightW - 40);
    const int dashFrom = drawText(c, line.x, base, left, kHeadStyle) + 12;
    const int dashTo = right.empty() ? line.right() : line.right() - rightW - 12;
    if (!right.empty()) drawText(c, line.right() - rightW, base, right, kHeadStyle);
    if (dashTo > dashFrom) dashedH(c, dashFrom, line.centerY(), dashTo - dashFrom);
  }

  // "P. 2/5 [=====:::::] 40%", as the Reader's.
  void drawProgress(Canvas &c, const Rect &strip) const {
    using namespace ui;
    hrule(c, strip.x, strip.y, strip.w);
    const Rect band{strip.x + kPadX, strip.y + kRule, strip.w - 2 * kPadX, strip.h - kRule};
    const std::string left =
        "P. " + std::to_string(page_ + 1) + "/" + std::to_string(doc_.pageCount());
    const std::string right = std::to_string(static_cast<int>(progress() * 100)) + "%";
    const int leftW = textWidth(kCardLine, left);
    const int rightW = textWidth(kCardLine, right);
    const int base = baselineCentered(kCardLine, band.y, band.h);
    drawText(c, band.x, base, left, kCardLine);
    drawText(c, band.right() - rightW, base, right, kCardLine);
    ui::drawProgress(c, {band.x + leftW + 10, band.centerY() - 7, band.w - leftW - rightW - 20, 14},
                     progress());
  }

  void openMenu() {
    StoryScreen *s = this;  // below the menu, so it outlives it
    const Uid uid = order_[index_];
    std::vector<ui::MenuItem> items;
    items.push_back({"SAVE",
                     [uid]() -> std::string {
                       StoryInfo i;
                       return news::story(uid, i) && i.saved ? "SAVED" : "NOT SAVED";
                     },
                     [uid] {
                       StoryInfo i;
                       if (news::story(uid, i)) news::setSaved(uid, !i.saved);
                     }});
    items.push_back({"TEXT SIZE", [s] { return ui::upper(::fonts::kReader[s->size_].name); },
                     [s] {
                       const uint32_t at = s->doc_.pageOffset(s->page_);
                       s->size_ = (s->size_ + 1) % ::fonts::kReaderSizeCount;
                       saveSize(s->size_);
                       s->layout(at);
                     }});
    items.push_back({"MARK UNREAD", nullptr, [uid] {
                       news::setRead(uid, false);
                       ui::nav::pop();  // the menu
                       ui::nav::pop();  // the story: back to the list
                     }});
    const int feed = info_.feed;
    items.push_back({"MORE FROM", [s] { return ui::upper(s->info_.feedTitle); }, [feed] {
                       ui::nav::replace(makeStories(feed));  // in place of the menu
                     }});
    items.push_back({"TO START", nullptr, [s] {
                       s->page_ = 0;
                       ui::nav::pop();
                     }});
    ui::nav::push(std::make_unique<ui::MenuScreen>("STORY", std::move(items)));
  }

  std::vector<Uid> order_;
  int index_;
  uint8_t size_;
  StoryInfo info_;
  NoteDoc doc_;
  int page_ = 0;
};

// --- The stories ----------------------------------------------------------------------------

class StoriesScreen : public ui::Screen {
 public:
  explicit StoriesScreen(int feed) : feed_(feed), show_(feed < 0 ? lastShow : news::Show::All) {
    ++liveScreens;
    if (feed_ < 0) news::load(true);  // feeds.txt may have changed (Wi-Fi Drive)
    rebuild(0);
    news::poll(true, kOpenSyncS);
  }
  ~StoriesScreen() override { --liveScreens; }

  std::string tag() const override { return moduleTag("NEWS"); }
  std::string context() const override {
    const news::Activity a = news::activity();
    if (a.busy && !a.text.empty()) return a.text;
    if (!sdcard::mounted()) return "NO CARD";
    if (feed_ < 0 && !news::hasFeeds()) return "NO FEEDS";
    const std::string who = feed_ < 0 ? "ALL FEEDS" : ui::upper(news::feed(feed_).title);
    const int fresh = news::freshCount(feed_);
    return who + (fresh ? dot() + std::to_string(fresh) + " NEW" : "");
  }
  int hints(Hint *out) const override {
    int n = 0;
    out[n++] = {glyph::kDial, "MOVE"};
    if (!stories_.empty()) out[n++] = {glyph::kPress, "READ"};
    out[n++] = {glyph::kHoldPress, "MENU"};
    out[n++] = {"BOOT", "BACK"};
    return n;
  }

  void onEnter() override {
    // Back from a story: it's read now, and the cursor goes to the last one read (▼ goes on to
    // the next story from the last page).
    rebuild(lastOpened ? lastOpened : selected());
    lastOpened = 0;
    seenGen_ = news::generation();
  }

  bool onKey(Key key) override {
    switch (key) {
      case Key::Up:
      case Key::Down: return move(key == Key::Up ? -1 : 1);
      case Key::Select: {
        if (stories_.empty()) return false;
        std::vector<Uid> order;
        order.reserve(stories_.size());
        for (const StoryInfo &s : stories_) order.push_back(s.uid);
        ui::nav::push(std::make_unique<StoryScreen>(std::move(order), cursor_));
        return true;
      }
      case Key::SelectLong: openMenu(); return true;
      default: return false;
    }
  }

  bool onTick() override {
    const uint32_t g = news::generation();
    if (g != seenGen_) {
      seenGen_ = g;
      rebuild(selected());
      pending_ = true;
    }
    if (news::activity().text != shownActivity_) pending_ = true;
    // A sync bumps the generation at every feed: redraw every few seconds while it runs.
    return pending_ && (!news::busy() || millis() - drawnAt_ >= kBusyRedrawMs);
  }

  void draw(Canvas &c, const Rect &body) override {
    using namespace ui;
    shownActivity_ = news::activity().text;
    drawnAt_ = millis();
    pending_ = false;
    Rect r = body;
    drawStrip(c, r.takeBottom(kStripH), syncText(), stripRight());
    const Rect cardArea = r.takeBottom(kStoryCardH + 2 * kCardMargin);
    listH_ = r.h;
    drawStories(c, r);
    drawCard(c, cardArea.inset(kCardMargin, kCardMargin), cardFor());
  }

  void cycleShow() {
    show_ = show_ == news::Show::All ? news::Show::New
            : show_ == news::Show::New ? news::Show::Saved
                                       : news::Show::All;
    if (feed_ < 0) lastShow = show_;
    rebuild(selected());
  }

 private:
  // A day's header, or a story.
  struct Entry {
    bool head;
    int story;  // a story: its index in stories_
    int group;  // a story: its header's index in entries_
    std::string label, right;  // a header
  };

  Uid selected() const {
    return cursor_ < static_cast<int>(stories_.size()) ? stories_[cursor_].uid : 0;
  }

  void rebuild(Uid keep) {
    stories_ = news::stories(feed_, show_);
    entries_.clear();
    std::string day;
    int head = -1;
    for (int i = 0; i < static_cast<int>(stories_.size()); ++i) {
      const std::string d = dayLabel(stories_[i].published);
      if (head < 0 || d != day) {
        day = d;
        head = static_cast<int>(entries_.size());
        entries_.push_back({true, -1, -1, d, ""});
      }
      entries_.push_back({false, i, head, "", ""});
    }
    // Each header's count: "4 NEW · 7".
    for (Entry &e : entries_) {
      if (!e.head) continue;
      int total = 0, fresh = 0;
      for (const Entry &s : entries_) {
        if (s.head || entries_[s.group].label != e.label) continue;
        ++total;
        fresh += stories_[s.story].fresh ? 1 : 0;
      }
      e.right = fresh ? std::to_string(fresh) + " NEW" + dot() + std::to_string(total)
                      : std::to_string(total);
    }
    cursor_ = 0;
    for (int i = 0; keep && i < static_cast<int>(stories_.size()); ++i) {
      if (stories_[i].uid == keep) cursor_ = i;
    }
  }

  int entryOf(int story) const {
    for (int i = 0; i < static_cast<int>(entries_.size()); ++i) {
      if (!entries_[i].head && entries_[i].story == story) return i;
    }
    return 0;
  }

  // Pages of entries for a list h px tall: a header never ends a page, and a page that starts
  // inside a day repeats its header. A header after another day's stories has a 3 px rule over it.
  std::vector<std::vector<int>> paginate(int h) const {
    std::vector<std::vector<int>> pages(1);
    int y = 0;
    for (int i = 0; i < static_cast<int>(entries_.size()); ++i) {
      const Entry &e = entries_[i];
      const int headH = pages.back().empty() ? ui::kSectionH : ui::kSectionH + ui::kRule;
      const int need = e.head ? headH + kRowH : kRowH;
      if (y + need > h && !pages.back().empty()) {
        pages.emplace_back();
        y = 0;
        if (!e.head) {
          pages.back().push_back(e.group);
          y += ui::kSectionH;
        }
      }
      y += e.head ? (pages.back().empty() ? ui::kSectionH : ui::kSectionH + ui::kRule) : kRowH;
      pages.back().push_back(i);
    }
    return pages;
  }

  static int pageOf(const std::vector<std::vector<int>> &pages, int entry) {
    for (int p = 0; p < static_cast<int>(pages.size()); ++p) {
      for (int e : pages[p]) {
        if (e == entry) return p;
      }
    }
    return 0;
  }

  bool move(int delta) {
    const int n = static_cast<int>(stories_.size());
    if (n == 0) return false;
    const int before = cursor_;
    if (input::lastKeyFast()) {
      // The dial held: a page at a time, landing on the page's first story.
      const auto pages = paginate(listH_);
      int p = pageOf(pages, entryOf(cursor_)) + (delta > 0 ? 1 : -1);
      p = std::max(0, std::min(p, static_cast<int>(pages.size()) - 1));
      for (int e : pages[p]) {
        if (!entries_[e].head) {
          cursor_ = entries_[e].story;
          break;
        }
      }
    } else if (n <= 10) {
      cursor_ = ((cursor_ + delta) % n + n) % n;
    } else {
      cursor_ = std::max(0, std::min(cursor_ + delta, n - 1));
    }
    return cursor_ != before;
  }

  void drawStories(Canvas &c, Rect r) const {
    using namespace ui;
    if (entries_.empty()) {
      drawSectionHeader(c, r.takeTop(kSectionH),
                        feed_ < 0 ? "STORIES" : upper(news::feed(feed_).title));
      drawText(c, {r.x + kPadX + kMarkW + 8, r.y, r.w - 2 * kPadX - kMarkW - 8, kRowH},
               emptyLine(), kFeedName);
      return;
    }
    const auto pages = paginate(r.h);
    const auto &page = pages[pageOf(pages, entryOf(cursor_))];
    int y = r.y;
    for (size_t k = 0; k < page.size(); ++k) {
      const Entry &e = entries_[page[k]];
      if (e.head) {
        if (k > 0) {
          hrule(c, r.x, y, r.w);
          y += kRule;
        }
        drawSectionHeader(c, {r.x, y, r.w, kSectionH}, e.label, e.right);
        y += kSectionH;
      } else {
        drawStoryRow(c, {r.x, y, r.w, kRowH}, stories_[e.story], e.story == cursor_, feed_ < 0);
        y += kRowH;
      }
    }
  }

  std::string emptyLine() const {
    if (!sdcard::mounted()) return "NO CARD";
    if (!news::hasFeeds()) return "NO FEEDS YET";
    if (show_ == news::Show::Saved) return "NOTHING SAVED";
    if (show_ == news::Show::New) return "NOTHING NEW";
    return news::busy() ? "SYNCING\xE2\x80\xA6" : "NOT SYNCED YET";
  }

  std::string stripRight() const {
    if (show_ == news::Show::New) return "NEW ONLY";
    if (show_ == news::Show::Saved) return "SAVED ONLY";
    if (feed_ >= 0) return news::feed(feed_).error.empty() ? "" : "LAST SYNC FAILED";
    if (!news::hasFeeds()) return "";
    const int failing = news::failingCount();
    if (failing) return std::to_string(failing) + " FAILING";
    const int n = news::feedCount();
    return std::to_string(n) + (n == 1 ? " FEED" : " FEEDS");
  }

  Card cardFor() const {
    Card k;
    if (!sdcard::mounted()) {
      k.title = "No card";
      k.text = "News keeps its feeds and stories on the microSD card.";
      return k;
    }
    if (!news::hasFeeds()) {
      k.title = "No feeds yet";
      k.text = "Add RSS or Atom feed URLs to SD:/News/feeds.txt, one per line. Wi-Fi Drive in "
               "System reaches the card from a computer.";
      return k;
    }
    if (stories_.empty()) {
      if (show_ == news::Show::Saved) {
        k.title = "Nothing saved";
        k.text = "Hold \xE2\x97\x8F on a story, or press \xE2\x97\x8F while reading one, and pick "
                 "SAVE.";
      } else if (show_ == news::Show::New) {
        k.title = "Nothing new";
        k.text = "Every story from the last three days has been read. Hold \xE2\x97\x8F and set "
                 "SHOW to ALL for the rest.";
      } else {
        k.title = "Not synced yet";
        k.text = wlan::configured()
                     ? "News syncs while it's open. Hold \xE2\x97\x8F and pick SYNC NOW to "
                       "start now."
                     : "Add a Wi-Fi network to SD:/inkdeck.ini, then hold \xE2\x97\x8F and pick "
                       "SYNC NOW.";
      }
      return k;
    }
    const StoryInfo &s = stories_[cursor_];
    k.line = ui::upper(s.feedTitle) + dot() + whenText(s.published);
    if (s.fresh) {
      k.tag = "NEW";
      k.tagInverted = true;
    }
    if (s.saved) k.tag2 = "SAVED";
    k.title = s.title;
    k.text = s.summary;
    k.footer = minutesText(s.words) + dot() + wordsText(s.words) +
               (s.full ? "" : dot() + "SUMMARY");
    k.verb = "READ";
    return k;
  }

  void openMenu() {
    StoriesScreen *n = this;  // below the menu, so it outlives it
    std::vector<ui::MenuItem> items;
    std::vector<int> ids;  // which item each is, for the cursor to come back to
    if (!stories_.empty()) {
      const Uid uid = stories_[cursor_].uid;
      items.push_back({"SAVE STORY",
                       [uid]() -> std::string {
                         StoryInfo i;
                         return news::story(uid, i) && i.saved ? "SAVED" : "NOT SAVED";
                       },
                       [uid] {
                         lastMenuItem = 0;
                         StoryInfo i;
                         if (news::story(uid, i)) news::setSaved(uid, !i.saved);
                       }});
      ids.push_back(0);
    }
    items.push_back({"SHOW", [n] { return std::string(showName(n->show_)); }, [n] {
                       lastMenuItem = 1;
                       n->cycleShow();
                     }});
    ids.push_back(1);
    items.push_back({"SYNC NOW",
                     []() -> std::string {
                       const news::Activity a = news::activity();
                       if (a.busy) return a.text;
                       const time_t t = news::lastSync();
                       if (!t) return "NEVER";
                       const std::string w = whenText(t);
                       return "LAST " + (w.rfind("TODAY ", 0) == 0 ? w.substr(6) : w);
                     },
                     [n] {
                       lastMenuItem = 2;
                       news::sync(n->feed_);
                       ui::nav::pop();
                     }});
    ids.push_back(2);
    if (feed_ < 0) {
      items.push_back({"FEEDS", [] { return feedsText(); }, [] {
                         lastMenuItem = 3;
                         ui::nav::replace(makeFeeds());  // in place of the menu
                       }});
      ids.push_back(3);
    }
    items.push_back({"MARK ALL READ",
                     [n]() -> std::string {
                       const int fresh = news::freshCount(n->feed_);
                       return fresh ? std::to_string(fresh) + " NEW" : std::string(glyph::kEmDash);
                     },
                     [n] {
                       lastMenuItem = 4;
                       news::markAllRead(n->feed_);
                       ui::nav::pop();
                     }});
    ids.push_back(4);
    int at = 0;
    for (int i = 0; i < static_cast<int>(ids.size()); ++i) {
      if (ids[i] == lastMenuItem) at = i;
    }
    ui::nav::push(std::make_unique<ui::MenuScreen>("NEWS", std::move(items), at));
  }

  int feed_;
  news::Show show_;
  std::vector<StoryInfo> stories_;
  std::vector<Entry> entries_;
  int cursor_ = 0;
  int listH_ = 476;
  uint32_t seenGen_ = 0;
  std::string shownActivity_;
  uint32_t drawnAt_ = 0;
  bool pending_ = false;
};

// --- The feeds ------------------------------------------------------------------------------

class FeedsScreen : public ui::Screen {
 public:
  FeedsScreen() {
    ++liveScreens;
    cursor_.reset(news::feedCount() + 1, 1);
  }
  ~FeedsScreen() override { --liveScreens; }

  std::string tag() const override { return moduleTag("NEWS"); }
  std::string context() const override {
    const news::Activity a = news::activity();
    if (a.busy && !a.text.empty()) return a.text;
    const int n = news::feedCount();
    const int failing = news::failingCount();
    return std::to_string(n) + (n == 1 ? " FEED" : " FEEDS") +
           (failing ? dot() + std::to_string(failing) + " FAILING" : "");
  }
  int hints(Hint *out) const override {
    out[0] = {glyph::kDial, "MOVE"};
    out[1] = {glyph::kPress, "OPEN"};
    out[2] = {glyph::kHoldPress, "SYNC"};
    out[3] = {"BOOT", "BACK"};
    return 4;
  }

  void onEnter() override { seenGen_ = news::generation(); }

  bool onKey(Key key) override {
    switch (key) {
      case Key::Up:
      case Key::Down: return cursor_.move(key == Key::Up ? -1 : 1, cursor_.count() <= 10);
      case Key::Select:
        if (cursor_.cursor() == 0) {
          ui::nav::pop();  // ALL FEEDS: the list below
        } else {
          ui::nav::push(makeStories(cursor_.cursor() - 1));
        }
        return true;
      case Key::SelectLong: news::sync(cursor_.cursor() - 1); return true;
      default: return false;
    }
  }

  bool onTick() override {
    const uint32_t g = news::generation();
    if (g != seenGen_) {
      seenGen_ = g;
      const int n = news::feedCount() + 1;
      if (n != cursor_.count()) cursor_.reset(n, std::min(cursor_.cursor(), n - 1));
      pending_ = true;
    }
    if (news::activity().text != shownActivity_) pending_ = true;
    return pending_ && (!news::busy() || millis() - drawnAt_ >= kBusyRedrawMs);
  }

  void draw(Canvas &c, const Rect &body) override {
    using namespace ui;
    shownActivity_ = news::activity().text;
    drawnAt_ = millis();
    pending_ = false;
    Rect r = body;
    const news::Activity a = news::activity();
    drawStrip(c, r.takeBottom(kStripH), "SD:/News/feeds.txt",
              a.text.empty() ? syncText() : a.text);
    const int rows = std::min(cursor_.count(), kFeedRowsMax);
    Rect list = r.takeTop(kSectionH + rows * kRowH);
    drawSectionHeader(c, list.takeTop(kSectionH), "FEEDS", "NEW");
    const int first = cursor_.pageStart(kFeedRowsMax);
    for (int i = first; i < cursor_.count() && i < first + kFeedRowsMax; ++i) {
      drawFeedRow(c, {list.x, list.y + (i - first) * kRowH, list.w, kRowH}, i,
                  i == cursor_.cursor());
    }
    // As tall as the space under the rows, up to what nine rows would leave.
    const Rect card = r.inset(kCardMargin, kCardMargin);
    drawCard(c, {card.x, card.y, card.w, std::min(card.h, kFeedCardMaxH)},
             cardFor(cursor_.cursor()));
  }

 private:
  void drawFeedRow(Canvas &c, const Rect &row, int i, bool selected) const {
    using namespace ui;
    const uint16_t ink = selected ? kPaper : kInk;
    std::string name, meta, count;
    bool failing = false;
    if (i == 0) {
      name = "ALL FEEDS";
      const int stories = news::storyCount();
      const int feeds = news::feedCount(), failing = news::failingCount();
      meta = std::to_string(feeds) + (feeds == 1 ? " FEED" : " FEEDS") +
             (failing ? dot() + std::to_string(failing) + " FAILING" : "") + dot() +
             std::to_string(stories) + " KEPT";
      const int fresh = news::freshCount();
      count = fresh ? std::to_string(fresh) : glyph::kDot;
    } else {
      const news::FeedInfo f = news::feed(i - 1);
      name = upper(f.title);
      failing = !f.error.empty();
      const std::string kept = dot() + std::to_string(f.stories) + " KEPT";
      if (failing) {
        meta = f.error + (f.failedSince ? " SINCE " + whenSince(f.failedSince) : "") + kept;
      } else if (f.fetched) {
        meta = f.kind + dot() + whenSince(f.fetched) + kept;
      } else {
        meta = "NOT SYNCED YET";
      }
      count = f.fresh ? std::to_string(f.fresh) : glyph::kDot;
    }
    if (selected) {
      fill(c, row, kInk);
      drawText(c, {row.x + kPadX, row.y, kMarkW, row.h}, glyph::kPlay, style::kCap, Align::Left,
               kPaper);
    } else if (i == 0) {
      hrule(c, row.x, row.bottom() - kRule, row.w);
    } else {
      dashedH(c, row.x, row.bottom() - 1, row.w);
    }
    const int right = row.right() - kPadX;
    int rightW;
    if (failing) {
      rightW = 20;
      const Rect box{right - rightW, row.centerY() - 10, 20, 20};
      if (selected) {
        frame(c, box, 2, kPaper);
        drawText(c, box, "!", kRightBold, Align::Center, kPaper);
      } else {
        fill(c, box, kInk);
        drawText(c, box, "!", kRightBold, Align::Center, kPaper);
      }
    } else {
      rightW = textWidth(kRightBold, count);
      drawText(c, {right - rightW, row.y, rightW, row.h}, count, kRightBold, Align::Left, ink);
    }
    const int tx = row.x + kPadX + kMarkW + 8;
    const int tw = right - rightW - 10 - tx;
    drawText(c, {tx, row.y + 5, tw, 20}, name, selected ? kFeedNameSelected : kFeedName,
             Align::Left, ink);
    drawText(c, {tx, row.y + 24, tw, 18}, meta, kMeta, Align::Left, ink);
  }

  // "14:20" today, "YDA 21:44", "18 NOV 21:44".
  static std::string whenSince(time_t t) {
    const std::string w = whenText(t);
    return w.rfind("TODAY ", 0) == 0 ? w.substr(6) : w;
  }

  Card cardFor(int i) const {
    Card k;
    if (i == 0) {
      k.title = "All feeds";
      const int n = news::feedCount();
      k.line = std::to_string(n) + (n == 1 ? " FEED" : " FEEDS") + dot() +
               std::to_string(news::storyCount()) + " STORIES";
      const time_t t = news::lastSync();
      if (t) {
        k.tag = "SYNCED " + whenSince(t);
        k.tagInverted = true;
      }
      k.subline = "SD:/News/feeds.txt";
      k.text = n ? "Every feed's stories in one list, newest first. Hold \xE2\x97\x8F to sync them "
                   "all."
                 : "Add RSS or Atom feed URLs to SD:/News/feeds.txt, one per line.";
      k.verb = "STORIES";
      return k;
    }
    const news::FeedInfo f = news::feed(i - 1);
    k.title = f.title;
    k.subline = linkText(f.url);
    k.line = (f.kind.empty() ? std::string("FEED") : f.kind) + dot() + std::to_string(f.stories) +
             (f.stories == 1 ? " STORY" : " STORIES");
    if (!f.error.empty()) {
      k.tag = "FAILING";
      k.tagInverted = true;
      k.text = "Syncing it fails: " + f.error +
               (f.failedSince ? " (since " + whenSince(f.failedSince) + ")." : ".") +
               (f.fetched ? " Its stories are from the last sync that worked, " +
                                whenSince(f.fetched) + "."
                          : "");
    } else if (f.fetched) {
      k.tag = "SYNCED " + whenSince(f.fetched);
      k.tagInverted = true;
      k.text = f.description;
    } else {
      k.tag = "NOT SYNCED";
      k.text = "Hold \xE2\x97\x8F to sync this feed.";
    }
    if (f.fetched) {
      char buf[48];
      snprintf(buf, sizeof buf, "%lu KB \xC2\xB7 %.1f S",
               static_cast<unsigned long>((f.bytes + 512) / 1024), f.ms / 1000.0f);
      k.footer = buf;
      if (f.fresh) k.footer += dot() + std::to_string(f.fresh) + " NEW";
    }
    k.verb = "STORIES";
    return k;
  }

  ui::ListCursor cursor_;
  uint32_t seenGen_ = 0;
  std::string shownActivity_;
  uint32_t drawnAt_ = 0;
  bool pending_ = false;
};

std::unique_ptr<ui::Screen> makeStories(int feed) { return std::make_unique<StoriesScreen>(feed); }
std::unique_ptr<ui::Screen> makeFeeds() { return std::make_unique<FeedsScreen>(); }

}  // namespace

std::unique_ptr<ui::Screen> makeNews() { return makeStories(-1); }

void pollNews() {
  if (isNapping()) return;  // reading on battery: the radio waits for a key
  const pmic::Status p = pmic::status();
  const bool usb = p.ok && p.vbus;
  const ui::Screen *top = ui::nav::top();
  // Not under the last frame before a power-off, which would cut it short; the edition on USB
  // stays up and wants its news.
  if (top && top->offFace() && !(usb && editionShowing())) return;
  static bool loaded = false;  // on USB the feeds sync without News being opened (read once)
  if (usb && !loaded) {
    loaded = true;
    news::load(false);
  }
  news::poll(liveScreens > 0 || usb);
}

bool newsOnScreen() {
  const ui::Screen *base = ui::nav::base();
  return liveScreens > 0 && base && base->tag() == moduleTag("NEWS");
}

}  // namespace apps
