// Podcasts: shows, their episodes, and what has been downloaded and listened to.
//
// A show is either
//   a feed:   an RSS URL in SD:/Podcasts/feeds.txt (one per line, # for comments). Refreshing
//             reads its newest episodes; GET downloads one to SD:/Podcasts/<show title>/.
//   a folder: any other folder in SD:/Podcasts, its MP3s copied there by hand. Episodes are
//             the files in name order.
// What each episode has reached (position, played, the downloaded file) is kept per show on the
// card in SD:/.inkdeck/podcasts/, next to the parsed feeds, so the screen opens offline.
//
// Network work (refreshing feeds, downloads) runs on one background task, one job at a time,
// with Wi-Fi up until the queue is empty. Episodes play through audio.h from where they were
// left; poll() saves the position as they play.
#pragma once
#include <stdint.h>
#include <time.h>

#include <string>

namespace podcasts {

constexpr const char *kRoot = "/sdcard/Podcasts";
constexpr const char *kFeedsFile = "/sdcard/Podcasts/feeds.txt";
constexpr int kMaxEpisodes = 30;  // newest episodes kept per feed

// The state column of an episode row.
enum class Mark : uint8_t {
  None,
  New,          // a feed episode from the last 7 days, not touched yet
  Queued,       // waiting to download
  Downloading,  // percent
  Partial,      // a download that stopped part-way (GET resumes it); percent
  Cached,       // on the card, not started
  Started,      // positionS into it
  Played,
};

struct ShowInfo {
  std::string title;
  std::string description;  // feeds
  std::string folder;       // under kRoot; "" for a feed not fetched yet
  std::string error;        // the last refresh's failure
  bool feed = false;
  time_t fetched = 0;
  int fresh = 0;            // episodes marked New
  int episodes = -1;        // -1: not listed yet (a folder is listed when first opened)
};

struct EpisodeInfo {
  std::string title;
  std::string description;
  std::string path;     // the local file ("" if not on the card)
  time_t published = 0;
  uint32_t seconds = 0;  // from the feed; 0 if unknown
  int64_t bytes = -1;    // the file's size, else the feed's; -1 unknown
  int number = -1;
  Mark mark = Mark::None;
  int percent = 0;       // Downloading, Partial
  uint32_t positionS = 0;
  bool playable = false;  // MP3 (the player decodes nothing else)
};

// (Re)reads feeds.txt, the show folders and the cached feeds. False without a card. Creates
// SD:/Podcasts and a feeds.txt that explains itself if they're missing and create is set.
bool load(bool create = false);
uint32_t generation();  // changes whenever anything below changes

int showCount();
ShowInfo show(int s);
int episodeCount(int s);  // lists a folder show the first time
EpisodeInfo episode(int s, int e);
int resumeIndex(int s);   // where to put the cursor: the episode last listened to
int freshTotal();         // New episodes, all feeds

// Network jobs.
bool hasFeeds();
void refresh();                // every feed
void get(int s, int e);        // queue a download (or resume a partial one)
void cancel(int s, int e);     // a queued or running download (the partial file stays)
void togglePlayed(int s, int e);

struct Activity {
  bool busy = false;
  std::string text;  // "REFRESHING 2/3", "DL 62% · Title"; when idle, the last result for a
                     // couple of minutes ("UPDATED 3 FEEDS"), then ""
};
Activity activity();
bool busy();  // the worker is running (Wi-Fi in use)

// Space: the Podcasts folder and the card's free space, counted in the background after
// load(); -1 until known.
int64_t usedBytes();
int64_t freeBytes();

// Plays the episode from where it was left (a folder show then carries on with the next files)
// and returns true; false if it isn't on the card or isn't an MP3.
bool play(int s, int e);

// From loop(): follows what the player is playing and saves its position about every 30 s;
// marks an episode played in its last 45 s.
void poll();

}  // namespace podcasts
