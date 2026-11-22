#include "apps/podcasts.h"

#include <Arduino.h>
#include <stdio.h>

#include <algorithm>

#include "apps/home.h"
#include "apps/music.h"
#include "podcast_library.h"
#include "timeutil.h"
#include "trackinfo.h"
#include "wlan.h"

namespace apps {
namespace {

using namespace ui;
using podcasts::EpisodeInfo;
using podcasts::Mark;
using podcasts::ShowInfo;

// Sizes and weights from the Podcasts artboard.
constexpr TextStyle kShowName{&fonts::kMono15Bold, 0};
constexpr TextStyle kShowNameSelected{&fonts::kMono15ExtraBold, 0};
constexpr TextStyle kShowCount{&fonts::kMono13Bold, 0};
constexpr TextStyle kShowCountSelected{&fonts::kMono13ExtraBold, 0};
constexpr TextStyle kState{&fonts::kMono13ExtraBold, 0};
constexpr TextStyle kEpTitle{&fonts::kMono15Bold, 0};
constexpr TextStyle kEpTitleSelected{&fonts::kMono15ExtraBold, 0};
constexpr TextStyle kEpMeta{&fonts::kMono13Medium, 0};
constexpr TextStyle kEpNumber{&fonts::kMono13Bold, 0};
constexpr TextStyle kCardLine{&fonts::kMono13Bold, 1};
constexpr TextStyle kCardTitle{&fonts::kMono18ExtraBold, 0};
constexpr TextStyle kCardText{&fonts::kMono14Medium, 0};

constexpr int kShowRowsMax = 6;
constexpr int kEpRowH = 46;
constexpr int kEpRows = 5;
constexpr int kStateW = 72, kNumberW = 44;
constexpr int kCacheBarH = 32;
constexpr int kCardMargin = 14;
constexpr uint32_t kProgressEveryMs = 5000;  // download progress redraws at most this often
constexpr time_t kRefreshAfter = 3 * 3600;   // feeds older than this refresh when the screen opens

using timeutil::clockText;

std::string sizeText(int64_t bytes) {
  if (bytes < 0) return glyph::kEllipsis;
  char buf[16];
  const double mb = bytes / 1e6;
  if (mb < 1000) {
    snprintf(buf, sizeof buf, "%.0f MB", mb);
  } else {
    snprintf(buf, sizeof buf, "%.1f GB", mb / 1000);
  }
  return buf;
}

std::string dateText(time_t t, bool year) {
  if (!t) return {};
  tm local;
  localtime_r(&t, &local);
  char buf[16];
  strftime(buf, sizeof buf, year ? "%d %b %Y" : "%d %b", &local);
  return upper(buf);
}

// "14:02" today, "12 NOV" before.
std::string updatedText(time_t t) {
  const time_t now = time(nullptr);
  tm a, b;
  localtime_r(&t, &a);
  localtime_r(&now, &b);
  if (a.tm_yday != b.tm_yday || a.tm_year != b.tm_year) return dateText(t, false);
  char buf[8];
  strftime(buf, sizeof buf, "%H:%M", &a);
  return buf;
}

std::string stateText(const EpisodeInfo &e) {
  switch (e.mark) {
    case Mark::New: return "[NEW]";
    case Mark::Queued: return "[QUEUED]";
    case Mark::Downloading: return "[DL " + std::to_string(e.percent) + "%]";
    case Mark::Partial: return "[" + std::to_string(e.percent) + "%]";
    case Mark::Cached: return "[SD]";
    case Mark::Started:
      return std::string("[") + glyph::kPlay + " " + std::to_string(e.positionS / 60) + "m]";
    case Mark::Played: return "[\xE2\x9C\x93]";  // ✓
    case Mark::None: break;
  }
  return {};
}

class PodcastsScreen : public Screen {
 public:
  PodcastsScreen() {
    podcasts::load(true);
    shows_.reset(podcasts::showCount());
    openShow(false);
    autoRefresh();
  }

  std::string tag() const override { return moduleTag("PODCASTS"); }
  std::string context() const override {
    const podcasts::Activity a = podcasts::activity();
    if (!a.text.empty()) return a.text;
    const int fresh = podcasts::freshTotal();
    if (fresh > 0) return std::to_string(fresh) + " NEW";
    const int n = podcasts::showCount();
    return n ? std::to_string(n) + (n == 1 ? " SHOW" : " SHOWS") : "NO SHOWS";
  }

  int hints(Hint *out) const override {
    out[0] = {glyph::kDial, "MOVE"};
    int n = 1;
    if (focus_ == Focus::Shows) {
      if (shows_.count() > 0) out[n++] = {glyph::kPress, "OPEN"};
      if (podcasts::hasFeeds()) out[n++] = {glyph::kHoldPress, "REFRESH"};
    } else if (episodes_.count() > 0) {
      const EpisodeInfo e = selectedEpisode();
      if (const char *verb = pressVerb(e)) out[n++] = {glyph::kPress, verb};
      const bool downloading = e.mark == Mark::Downloading || e.mark == Mark::Queued;
      out[n++] = {glyph::kHoldPress, downloading           ? "CANCEL"
                                     : e.mark == Mark::Played ? "UNPLAYED"
                                                              : "PLAYED"};
    }
    out[n++] = {"BOOT", "BACK"};
    return n;
  }

  void onEnter() override {
    seenGen_ = podcasts::generation();
    seenTrackInfo_ = trackinfo::generation();
  }

  bool onKey(Key key) override {
    if (focus_ == Focus::Shows) {
      switch (key) {
        case Key::Up:
        case Key::Down:
          if (!shows_.move(key == Key::Up ? -1 : 1, true)) return false;
          openShow(false);
          return true;
        case Key::Select:
          if (shows_.count() == 0) return false;
          openShow(true);
          focus_ = Focus::Episodes;
          return true;
        case Key::SelectLong:
          if (!podcasts::hasFeeds()) return false;
          podcasts::refresh();
          return true;
        default: return false;
      }
    }
    switch (key) {
      case Key::Up:
      case Key::Down:
        return episodes_.move(key == Key::Up ? -1 : 1, episodes_.count() <= 10);
      case Key::Select: {
        const EpisodeInfo e = selectedEpisode();
        if (!e.path.empty() && e.playable) {
          if (podcasts::play(shows_.cursor(), episodes_.cursor())) nav::push(makeMusic());
        } else if (e.playable && (e.mark == Mark::None || e.mark == Mark::New ||
                                  e.mark == Mark::Partial)) {
          podcasts::get(shows_.cursor(), episodes_.cursor());
        }
        return true;
      }
      case Key::SelectLong: {
        const EpisodeInfo e = selectedEpisode();
        if (e.mark == Mark::Downloading || e.mark == Mark::Queued) {
          podcasts::cancel(shows_.cursor(), episodes_.cursor());
        } else {
          podcasts::togglePlayed(shows_.cursor(), episodes_.cursor());
        }
        return true;
      }
      case Key::Back:
        focus_ = Focus::Shows;
        return true;
      default: return false;
    }
  }

  bool onTick() override {
    bool redraw = false;
    const uint32_t g = podcasts::generation();
    if (g != seenGen_) {
      seenGen_ = g;
      const int n = podcasts::showCount();
      if (n != shows_.count()) shows_.reset(n, std::min(shows_.cursor(), std::max(0, n - 1)));
      const int eps = n ? podcasts::episodeCount(shows_.cursor()) : 0;
      if (eps != episodes_.count()) episodes_.reset(eps, std::min(episodes_.cursor(), std::max(0, eps - 1)));
      redraw = true;
    }
    const uint32_t t = trackinfo::generation();
    if (t != seenTrackInfo_) {
      seenTrackInfo_ = t;
      redraw = true;  // the selected file's tags arrived
    }
    // Download progress: the status bar and the row, every few seconds.
    const podcasts::Activity a = podcasts::activity();
    if (a.text != shownActivity_ && (!a.busy || millis() - progressAt_ >= kProgressEveryMs)) {
      redraw = true;
    }
    return redraw;
  }

  void draw(Canvas &c, const Rect &body) override {
    shownActivity_ = podcasts::activity().text;
    progressAt_ = millis();
    Rect r = body;
    const Rect cache = r.takeBottom(kCacheBarH);
    const int rows = std::clamp(shows_.count(), 1, kShowRowsMax);
    drawShows(c, r.takeTop(kSectionH + rows * kRowH));
    hrule(c, r.x, r.y, r.w);
    r.takeTop(kRule);
    drawEpisodes(c, r.takeTop(kSectionH + kEpRows * kEpRowH));
    drawCard(c, r.inset(kCardMargin, kCardMargin));
    drawCache(c, cache);
  }

 private:
  enum class Focus : uint8_t { Shows, Episodes };

  // The episodes of the show under the cursor; `atResume` puts the cursor on the one last
  // listened to (when opening it), otherwise at the top (a preview while moving).
  void openShow(bool atResume) {
    const int n = shows_.count() ? podcasts::episodeCount(shows_.cursor()) : 0;
    episodes_.reset(n, atResume ? podcasts::resumeIndex(shows_.cursor()) : 0);
  }

  void autoRefresh() {
    if (!podcasts::hasFeeds() || !wlan::configured() || podcasts::busy()) return;
    const time_t now = time(nullptr);
    for (int i = 0; i < podcasts::showCount(); ++i) {
      const ShowInfo s = podcasts::show(i);
      if (s.feed && now - s.fetched > kRefreshAfter) {
        podcasts::refresh();
        return;
      }
    }
  }

  EpisodeInfo selectedEpisode() const {
    if (episodes_.count() == 0) return {};
    return podcasts::episode(shows_.cursor(), episodes_.cursor());
  }

  static const char *pressVerb(const EpisodeInfo &e) {
    if (!e.playable) return nullptr;
    if (!e.path.empty()) return e.mark == Mark::Started ? "RESUME" : "PLAY";
    if (e.mark == Mark::Partial) return "RESUME DL";
    if (e.mark == Mark::None || e.mark == Mark::New) return "GET";
    return nullptr;
  }

  // An episode's title: the MP3's own title tag for folder shows, once it has been read.
  std::string titleOf(const EpisodeInfo &e, bool request) const {
    if (!e.path.empty() && e.description.empty()) {
      if (request) trackinfo::request(e.path, false);
      trackinfo::Info info;
      if (trackinfo::get(e.path, info) && !info.meta.title.empty()) return info.meta.title;
    }
    return e.title;
  }

  uint32_t secondsOf(const EpisodeInfo &e) const {
    if (e.seconds) return e.seconds;
    trackinfo::Info info;
    if (!e.path.empty() && trackinfo::get(e.path, info)) return info.meta.durationMs / 1000;
    return 0;
  }

  void drawShows(Canvas &c, const Rect &area) {
    Rect r = area;
    drawSectionHeader(c, r.takeTop(kSectionH), "FEEDS", "NEW");
    if (shows_.count() == 0) {
      drawText(c, {kPadX + 24, r.y, r.w - 2 * kPadX - 24, kRowH}, "NO SHOWS YET", kShowName);
      return;
    }
    const int first = shows_.pageStart(kShowRowsMax);
    for (int i = first; i < shows_.count() && i < first + kShowRowsMax; ++i) {
      const Rect row{r.x, r.y + (i - first) * kRowH, r.w, kRowH};
      const ShowInfo s = podcasts::show(i);
      const bool selected = i == shows_.cursor();
      const uint16_t ink = selected ? kPaper : kInk;
      const std::string count = s.fresh > 0 ? std::to_string(s.fresh) : glyph::kDot;
      if (selected) {
        fill(c, row, kInk);
        drawText(c, {row.x + kPadX, row.y, 14, row.h}, glyph::kPlay, style::kCap, Align::Left,
                 kPaper);
      } else {
        dashedH(c, row.x, row.bottom() - 1, row.w);
      }
      const TextStyle &countStyle = selected ? kShowCountSelected : kShowCount;
      const int countW = textWidth(countStyle, count);
      drawText(c, {row.right() - kPadX - countW, row.y, countW, row.h}, count, countStyle,
               Align::Left, ink);
      const int nameX = row.x + kPadX + 24;
      drawText(c, {nameX, row.y, row.right() - kPadX - countW - 10 - nameX, row.h}, upper(s.title),
               selected ? kShowNameSelected : kShowName, Align::Left, ink);
    }
  }

  void drawEpisodes(Canvas &c, const Rect &area) {
    Rect r = area;
    const ShowInfo show = podcasts::show(shows_.cursor());
    const std::string count =
        shows_.count() == 0 ? "" : std::to_string(episodes_.count()) + " EP";
    drawSectionHeader(c, r.takeTop(kSectionH), shows_.count() ? upper(show.title) : "EPISODES",
                      count);
    const int first = episodes_.pageStart(kEpRows);
    for (int i = first; i < episodes_.count() && i < first + kEpRows; ++i) {
      const Rect row{r.x, r.y + (i - first) * kEpRowH, r.w, kEpRowH};
      const EpisodeInfo e = podcasts::episode(shows_.cursor(), i);
      const bool selected = focus_ == Focus::Episodes && i == episodes_.cursor();
      const uint16_t ink = selected ? kPaper : kInk;
      if (selected) {
        fill(c, row, kInk);
      } else {
        dashedH(c, row.x, row.bottom() - 1, row.w);
      }
      const int x = row.x + kPadX;
      drawText(c, {x, row.y, kStateW, row.h}, stateText(e), kState, Align::Left, ink);
      const std::string number = e.number >= 0 ? "#" + std::to_string(e.number) : "";
      drawText(c, {row.right() - kPadX - kNumberW, row.y, kNumberW, row.h}, number, kEpNumber,
               Align::Right, ink);
      const int tx = x + kStateW + 10;
      const int tw = row.right() - kPadX - kNumberW - 10 - tx;
      std::string meta;
      if (show.feed) {
        meta = e.seconds ? clockText(e.seconds) : "";
        const std::string date = dateText(e.published, false);
        if (!date.empty()) meta += (meta.empty() ? "" : std::string(" ") + glyph::kDot + " ") + date;
      } else {
        meta = sizeText(e.bytes);
      }
      drawText(c, {tx, row.y + 5, tw, 20}, e.title, selected ? kEpTitleSelected : kEpTitle,
               Align::Left, ink);
      drawText(c, {tx, row.y + 24, tw, 18}, meta, kEpMeta, Align::Left, ink);
    }
  }

  // The card: a 3 px box with a heading line and tag, a title, some text and a footer.
  struct Card {
    std::string line, tag, title, text, footer;
    bool tagInverted = false;
    const char *verb = nullptr;  // the ● hint at the bottom right
  };

  Card cardFor() const {
    Card k;
    if (shows_.count() == 0) {
      k.title = "No shows yet";
      k.text = "Add RSS feed URLs to SD:/Podcasts/feeds.txt, one per line, or copy folders of "
               "MP3s into SD:/Podcasts.";
      return k;
    }
    const ShowInfo show = podcasts::show(shows_.cursor());
    if (focus_ == Focus::Shows) {
      k.title = show.title;
      if (show.feed) {
        k.tag = "FEED";
        k.line = show.fetched ? std::to_string(episodes_.count()) + " EPISODES " + glyph::kDot +
                                    " UPDATED " + updatedText(show.fetched)
                              : "NOT FETCHED YET";
        k.text = show.error.empty() ? show.description : "REFRESH FAILED: " + show.error;
        k.footer = "HOLD \xE2\x97\x8F REFRESHES THE FEEDS";
      } else {
        k.tag = "SD FOLDER";
        k.line = std::to_string(episodes_.count()) + " EPISODES";
        k.text = "SD:/Podcasts/" + show.folder + "/";
      }
      k.verb = "OPEN";
      return k;
    }
    if (episodes_.count() == 0) {
      k.title = show.title;
      k.text = show.feed ? "No episodes yet. Hold \xE2\x97\x8F on the feed list to refresh."
                         : "No MP3 files in this folder.";
      return k;
    }
    const EpisodeInfo e = selectedEpisode();
    k.title = titleOf(e, true);
    const std::string number = e.number >= 0 ? "#" + std::to_string(e.number) : "";
    const std::string date = dateText(e.published, true);
    k.line = number + (!number.empty() && !date.empty() ? std::string(" ") + glyph::kDot + " " : "") + date;
    const uint32_t seconds = secondsOf(e);
    if (show.feed) {
      k.text = e.description;
    } else if (e.mark == Mark::Started) {
      k.text = "At " + clockText(e.positionS) + (seconds ? " of " + clockText(seconds) : "") + ".";
    } else if (e.mark == Mark::Played) {
      k.text = "Played.";
    }
    if (!e.playable) {
      k.tag = "NOT MP3";
    } else if (e.mark == Mark::Downloading) {
      k.tag = "DL " + std::to_string(e.percent) + "%";
      k.tagInverted = true;
    } else if (e.mark == Mark::Queued) {
      k.tag = "QUEUED";
    } else if (e.mark == Mark::Partial) {
      k.tag = "PARTIAL";
    } else if (!e.path.empty()) {
      k.tag = show.feed ? "CACHED" : "ON SD";
      k.tagInverted = true;
    } else {
      k.tag = "NOT CACHED";
    }
    k.footer = seconds ? clockText(seconds) : "";
    if (e.bytes >= 0) k.footer += (k.footer.empty() ? "" : std::string(" ") + glyph::kDot + " ") + sizeText(e.bytes);
    k.footer += (k.footer.empty() ? "" : std::string(" ") + glyph::kDot + " ") +
                std::string(e.playable ? "MP3" : "OTHER");
    k.verb = pressVerb(e);
    return k;
  }

  void drawCard(Canvas &c, const Rect &box) {
    if (box.h < 60) return;
    frame(c, box, kRule);
    const Card k = cardFor();
    const Rect in = box.inset(kRule + 14, kRule + 12);
    int y = in.y;
    int tagW = 0;
    if (!k.tag.empty()) {
      tagW = tagWidth(k.tag, kCardLine, 6);
      drawTag(c, in.right() - tagW, y + 9, 19, k.tag, kCardLine,
              k.tagInverted ? TagKind::Inverted : TagKind::Outlined, 6);
    }
    drawText(c, {in.x, y, in.w - tagW - 10, 18}, k.line, kCardLine);
    y += 18 + 8;
    // Footer first, so the text above knows where to stop.
    const int footerY = in.bottom() - 20;
    int verbW = 0;
    if (k.verb) {
      verbW = keycapWidth(glyph::kPress) + 6 + textWidth(kCardLine, k.verb);
      const int vx = in.right() - verbW;
      const int capW = drawKeycap(c, vx, footerY + 10, glyph::kPress);
      drawText(c, {vx + capW + 6, footerY, verbW - capW - 6, 20}, k.verb, kCardLine);
    }
    drawText(c, {in.x, footerY, in.w - verbW - 10, 20}, k.footer, kCardLine);

    const std::vector<std::string> title = wrap(kCardTitle, k.title, in.w, 2);
    for (const std::string &line : title) {
      drawText(c, in.x, y + capHeight(kCardTitle) + 2, line, kCardTitle);
      y += 22;
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

  void drawCache(Canvas &c, const Rect &area) {
    hrule(c, area.x, area.y, area.w);
    const Rect band{area.x + kPadX, area.y + kRule, area.w - 2 * kPadX, area.h - kRule};
    const int64_t used = podcasts::usedBytes(), free = podcasts::freeBytes();
    const std::string label = sizeText(used);
    const int labelW = textWidth(kCardLine, label);
    const int keyW = textWidth(kCardLine, "SD CACHE");
    drawText(c, {band.x, band.y, keyW, band.h}, "SD CACHE", kCardLine);
    drawText(c, {band.right() - labelW, band.y, labelW, band.h}, label, kCardLine);
    // How much of the space podcasts could have they use: theirs plus what's free.
    const Rect bar{band.x + keyW + 10, band.centerY() - 7, band.w - keyW - labelW - 20, 14};
    frame(c, bar, 2);
    const Rect inner = bar.inset(4, 4);
    const float f = used >= 0 && free >= 0 && used + free > 0
                        ? static_cast<float>(used) / static_cast<float>(used + free)
                        : 0;
    const int solid = static_cast<int>(inner.w * std::clamp(f, 0.0f, 1.0f));
    fill(c, {inner.x, inner.y, solid, inner.h}, kInk);
    dither50(c, {inner.x + solid, inner.y, inner.w - solid, inner.h});
  }

  Focus focus_ = Focus::Shows;
  ListCursor shows_;
  ListCursor episodes_;
  uint32_t seenGen_ = 0;
  uint32_t seenTrackInfo_ = 0;
  std::string shownActivity_;
  uint32_t progressAt_ = 0;
};

}  // namespace

std::unique_ptr<ui::Screen> makePodcasts() { return std::make_unique<PodcastsScreen>(); }

}  // namespace apps
