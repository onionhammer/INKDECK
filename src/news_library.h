// News: RSS and Atom feeds, their stories, and which have been read or saved (apps/news.h shows
// them).
//
// The feeds are the URLs in SD:/News/feeds.txt, one per line (# for comments), as Podcasts
// reads SD:/Podcasts/feeds.txt. A sync fetches each feed (rss::parseNews(): RSS 2.0, RSS 1.0 and
// Atom), merges what it brings with the stories already kept, newest first, and keeps the newest
// kKeepPerFeed of them a feed, plus any saved ones. The stories and their text are kept on the
// card in SD:/.inkdeck/news/, so the screen opens offline: per feed, a .meta file (the feed and
// its stories, one line each), a .body file (the stories' text, Markdown for NoteDoc) and a
// .state file (read and saved, by story id).
//
// Syncing runs on one background task, a feed at a time, with Wi-Fi up only while it works.
// poll() syncs on its own when the caller says News is wanted (apps::pollNews(): a News screen
// open, the board on USB) and the last sync is over an hour old.
//
// A story is new until it's opened, if it came out in the last three days.
#pragma once
#include <stdint.h>
#include <time.h>

#include <string>
#include <vector>

namespace news {

constexpr const char *kRoot = "/sdcard/News";
constexpr const char *kFeedsFile = "/sdcard/News/feeds.txt";
constexpr int kKeepPerFeed = 50;

struct FeedInfo {
  std::string title;        // the feed's, else its host
  std::string url;
  std::string description;
  std::string kind;         // "RSS", "RDF", "ATOM"; "" before the first sync
  std::string error;        // the last sync's failure ("HTTP 404"); "" if it worked
  time_t fetched = 0;       // the last sync that worked
  time_t failedSince = 0;   // the first failure since then
  uint32_t bytes = 0;       // what the last sync downloaded
  uint32_t ms = 0;          // and how long it took
  int stories = 0;
  int fresh = 0;
};

// Stories are named by a number that stays the same while the board is on (a sync keeps it for
// a story it already had); 0 is none.
using Uid = uint32_t;

struct StoryInfo {
  Uid uid = 0;
  int feed = -1;            // its feed's index
  std::string feedTitle;
  std::string title;
  std::string author;
  std::string summary;      // plain text
  std::string link;
  time_t published = 0;     // when it came out, else when it was first fetched
  uint32_t words = 0;
  bool full = false;        // the feed carries the whole text, not only a summary
  bool fresh = false;       // new: not opened, from the last three days
  bool read = false;
  bool saved = false;
};

enum class Show : uint8_t { All, New, Saved };

// (Re)reads feeds.txt and what's kept on the card. False without a card. Creates SD:/News and
// a feeds.txt that explains itself if they're missing and create is set.
bool load(bool create = false);
uint32_t generation();  // changes whenever anything below changes

bool hasFeeds();
int feedCount();
FeedInfo feed(int f);
int failingCount();

// The stories of every feed (feed = -1) or one, newest first, as `show` filters them.
std::vector<StoryInfo> stories(int feed, Show show);
bool story(Uid uid, StoryInfo &out);
// The story's text, Markdown (notedoc.h), read from the card. False if it's gone.
bool body(Uid uid, std::string &markdown);

int freshCount(int feed = -1);  // new stories, every feed's or one's
int savedCount();
int storyCount();

void setRead(Uid uid, bool read);
void setSaved(Uid uid, bool saved);
void markAllRead(int feed = -1);

// Syncing, in the background: every feed (-1) or one.
void sync(int feed = -1);
void cancel();  // stops after the feed being fetched
bool busy();    // the task is running (Wi-Fi in use)
struct Activity {
  bool busy = false;
  std::string text;  // "JOINING WI-FI…", "SYNCING 3/8"; idle: the last result for two
                     // minutes ("SYNCED 8 FEEDS", "WI-FI: NETWORK NOT FOUND"), then ""
};
Activity activity();
time_t lastSync();  // the newest of the feeds' last good syncs; 0 if none

// A sync of every feed, if `wanted` and the last one (or the last try this boot) is everyS old.
// From loop() every pass (apps::pollNews()), and when a News screen opens.
void poll(bool wanted, uint32_t everyS = 3600);

}  // namespace news
