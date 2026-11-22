#include "news_library.h"

#include <Arduino.h>
#include <esp_random.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <deque>
#include <set>

#include "article.h"
#include "cache_file.h"
#include "netget.h"
#include "rss.h"
#include "rtc.h"
#include "sdcard.h"
#include "text.h"
#include "wlan.h"

namespace news {
namespace {

constexpr const char *kCacheDir = "/sdcard/.inkdeck/news";
constexpr time_t kNewFor = 3 * 86400;
constexpr size_t kFeedCap = 1536 * 1024;  // of a feed downloaded; read until kKeepPerFeed items
constexpr size_t kMaxBody = 20 * 1024;    // of a story's text kept
constexpr uint32_t kConnectTimeoutMs = 20000;
constexpr time_t kResultShownFor = 120;
constexpr uint8_t kRead = 1, kSaved = 2;

// --- The library -----------------------------------------------------------------------------

struct Item {
  Uid uid = 0;
  std::string id;
  std::string title;
  std::string author;
  std::string summary;
  std::string link;
  time_t published = 0;  // else when it was first seen
  uint32_t words = 0;
  bool full = false;
  uint32_t offset = 0;  // its text in the .body file
  uint32_t length = 0;
};

struct Feed {
  std::string key;  // names its files in kCacheDir
  std::string url;
  std::string title;
  std::string description;
  std::string kind;
  std::string error;
  std::string stamp;  // the .body file that goes with these offsets
  time_t fetched = 0;
  time_t failedSince = 0;
  uint32_t bytes = 0;
  uint32_t ms = 0;
  std::vector<Item> items;  // newest first
  std::map<std::string, uint8_t> state;  // by story id: kRead | kSaved
};

std::mutex mu;  // guards everything in this block and the worker's state below
std::vector<Feed> feeds;
std::atomic<uint32_t> gen{1};
std::atomic<uint32_t> nextUid{1};
std::mutex stateFileMu;  // one writer at a time for the .state files

Feed *byKey(const std::string &key) {
  for (Feed &f : feeds) {
    if (f.key == key) return &f;
  }
  return nullptr;
}

bool isFresh(const Feed &f, const Item &it, time_t now) {
  const auto st = f.state.find(it.id);
  if (st != f.state.end() && (st->second & kRead)) return false;
  return it.published && now - it.published < kNewFor;
}

// --- The worker's state (guarded by mu) --------------------------------------------------------

std::deque<std::string> queue;  // feed keys to sync, in the file's order
bool working = false;
std::string currentKey;  // the feed being fetched
std::string phaseText;
std::string result;
time_t resultAt = 0;
uint32_t lastAttemptMs = 0;  // a sync of every feed last started; 0: not this boot
std::atomic<bool> cancelNow{false};

void setResult(std::string text) {
  result = std::move(text);
  resultAt = time(nullptr);
}

// --- Small helpers -----------------------------------------------------------------------------

bool exists(const std::string &path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0;
}

void ensureCacheDir() { cachefile::ensureDir(kCacheDir); }

std::string cachePath(const std::string &key, const char *ext) {
  return std::string(kCacheDir) + "/" + key + ext;
}

std::string newStamp() {
  char buf[12];
  snprintf(buf, sizeof buf, "%08lx", static_cast<unsigned long>(esp_random()));
  return buf;
}

// --- Files in kCacheDir ------------------------------------------------------------------------

// The meta file: "F" the feed, then "S" a story a line.
void readMeta(Feed &f) {
  std::string data;
  if (!cachefile::read(cachePath(f.key, ".meta"), data)) return;
  cachefile::forEachLine(data, [&](const std::string &line) {
    const std::vector<std::string> v = cachefile::split(line);
    if (v[0] == "F" && v.size() >= 12) {
      f.fetched = atoll(v[2].c_str());
      f.failedSince = atoll(v[3].c_str());
      f.bytes = strtoul(v[4].c_str(), nullptr, 10);
      f.ms = strtoul(v[5].c_str(), nullptr, 10);
      f.kind = v[6];
      f.stamp = v[7];
      f.title = v[8];
      f.description = v[9];
      f.error = v[10];
    } else if (v[0] == "S" && v.size() >= 12) {
      Item it;
      it.id = v[1];
      it.published = atoll(v[2].c_str());
      it.words = strtoul(v[3].c_str(), nullptr, 10);
      it.full = v[4] == "1";
      it.offset = strtoul(v[5].c_str(), nullptr, 10);
      it.length = strtoul(v[6].c_str(), nullptr, 10);
      it.link = v[7];
      it.author = v[8];
      it.title = v[9];
      it.summary = v[10];
      f.items.push_back(std::move(it));
    }
  });
}

std::string metaText(const Feed &f) {
  std::string out = "INKNEWS1\n";
  char num[96];
  snprintf(num, sizeof num, "%lld\t%lld\t%lu\t%lu", static_cast<long long>(f.fetched),
           static_cast<long long>(f.failedSince), static_cast<unsigned long>(f.bytes),
           static_cast<unsigned long>(f.ms));
  out += "F\t" + cachefile::field(f.url) + "\t" + num + "\t" + cachefile::field(f.kind) + "\t" + cachefile::field(f.stamp) + "\t" +
         cachefile::field(f.title) + "\t" + cachefile::field(f.description) + "\t" + cachefile::field(f.error) + "\t\n";
  for (const Item &it : f.items) {
    snprintf(num, sizeof num, "%lld\t%lu\t%d\t%lu\t%lu", static_cast<long long>(it.published),
             static_cast<unsigned long>(it.words), it.full ? 1 : 0,
             static_cast<unsigned long>(it.offset), static_cast<unsigned long>(it.length));
    out += "S\t" + cachefile::field(it.id) + "\t" + num + "\t" + cachefile::field(it.link) + "\t" + cachefile::field(it.author) +
           "\t" + cachefile::field(it.title) + "\t" + cachefile::field(it.summary) + "\t\n";
  }
  return out;
}

std::map<std::string, uint8_t> readState(const std::string &key) {
  std::map<std::string, uint8_t> state;
  std::string data;
  if (!cachefile::read(cachePath(key, ".state"), data)) return state;
  cachefile::forEachLine(data, [&](const std::string &line) {
    const std::vector<std::string> v = cachefile::split(line);
    if (v[0] == "R" && v.size() >= 3) state[v[2]] = static_cast<uint8_t>(atoi(v[1].c_str()));
  });
  return state;
}

void writeState(const std::string &key, const std::map<std::string, uint8_t> &state) {
  std::string out = "INKSTATE1\n";
  for (const auto &kv : state) {
    if (kv.second) out += "R\t" + std::to_string(kv.second) + "\t" + cachefile::field(kv.first) + "\n";
  }
  std::lock_guard<std::mutex> lock(stateFileMu);
  cachefile::writeWhole(cachePath(key, ".state"), out);
}

// Saves a feed's state (copied under mu, written outside it).
void saveState(const std::string &key) {
  std::map<std::string, uint8_t> copy;
  {
    std::lock_guard<std::mutex> lock(mu);
    const Feed *f = byKey(key);
    if (!f) return;
    copy = f->state;
  }
  writeState(key, copy);
}

// A story's text from the feed's .body file, if the file is the one `stamp` names.
bool readBody(const std::string &key, const std::string &stamp, uint32_t offset, uint32_t length,
              std::string &out, FILE *open = nullptr) {
  FILE *f = open ? open : fopen(cachePath(key, ".body").c_str(), "rb");
  if (!f) return false;
  bool ok = false;
  char head[32] = {};
  fseek(f, 0, SEEK_SET);
  if (fgets(head, sizeof head, f) && std::string(head) == "INKBODY " + stamp + "\n") {
    out.resize(length);
    ok = fseek(f, offset, SEEK_SET) == 0 &&
         (length == 0 || fread(&out[0], 1, length, f) == length);
  }
  if (!open) fclose(f);
  return ok;
}

// --- Syncing (worker task) ---------------------------------------------------------------------

// Reads a feed until it has kKeepPerFeed complete stories (or ends): some carry every article
// whole and run to megabytes.
bool fetchFeed(const std::string &url, std::string &body, std::string &error) {
  if (net::getFeed(url, body, error, kFeedCap, kKeepPerFeed)) return true;
  if (error.rfind("SERVER SAID ", 0) == 0) error = "HTTP " + error.substr(12);
  return false;
}

// Merges a fetched feed into what's kept and writes its files. Returns the feed as kept now.
Feed merge(const Feed &old, rss::NewsFeed &fetched, time_t now, uint32_t bytes, uint32_t ms) {
  Feed f = old;
  f.bytes = bytes;
  f.ms = ms;
  if (!fetched.title.empty()) f.title = fetched.title;
  f.description = fetched.description;
  f.kind = fetched.kind;
  f.error.clear();
  f.failedSince = 0;
  f.fetched = now;
  f.stamp = newStamp();

  std::map<std::string, const Item *> before;
  for (const Item &it : old.items) before[it.id] = &it;
  struct Kept {
    Item item;
    const std::string *text;  // the new text, else read the old
  };
  std::vector<Kept> kept;
  std::set<std::string> ids;
  for (rss::Story &s : fetched.stories) {
    if (!ids.insert(s.id).second) continue;  // the same story twice in one feed
    Item it;
    const auto was = before.find(s.id);
    it.uid = was != before.end() ? was->second->uid : nextUid++;
    it.id = s.id;
    it.title = s.title;
    it.author = s.author;
    it.summary = s.summary;
    it.link = s.link;
    it.full = s.full;
    it.words = text::countWords(s.body);
    // Undated: when it was first seen, so it keeps its place.
    it.published = s.published ? s.published : was != before.end() ? was->second->published : now;
    kept.push_back({std::move(it), &s.body});
  }
  for (const Item &it : old.items) {
    if (ids.insert(it.id).second) kept.push_back({it, nullptr});
  }
  std::stable_sort(kept.begin(), kept.end(), [](const Kept &a, const Kept &b) {
    return a.item.published > b.item.published;
  });
  // The newest kKeepPerFeed, and saved ones past them.
  std::vector<Kept> keep;
  for (size_t i = 0; i < kept.size(); ++i) {
    const auto st = old.state.find(kept[i].item.id);
    const bool saved = st != old.state.end() && (st->second & kSaved);
    if (static_cast<int>(keep.size()) < kKeepPerFeed || saved) keep.push_back(std::move(kept[i]));
  }

  // The texts, into a new .body file: the fetched ones, and the old ones copied over.
  std::string bodies = "INKBODY " + f.stamp + "\n";
  FILE *oldFile = fopen(cachePath(old.key, ".body").c_str(), "rb");
  f.items.clear();
  for (Kept &k : keep) {
    std::string text;
    if (k.text) {
      text = *k.text;
    } else if (!oldFile ||
               !readBody(old.key, old.stamp, k.item.offset, k.item.length, text, oldFile)) {
      text = k.item.summary;
    }
    k.item.offset = static_cast<uint32_t>(bodies.size());
    k.item.length = static_cast<uint32_t>(text.size());
    bodies += text;
    f.items.push_back(std::move(k.item));
  }
  if (oldFile) fclose(oldFile);

  // What's read or saved, for the stories still kept.
  std::map<std::string, uint8_t> state;
  for (const Item &it : f.items) {
    const auto st = old.state.find(it.id);
    if (st != old.state.end() && st->second) state[it.id] = st->second;
  }
  f.state = std::move(state);

  // The body first: a .meta always names the .body it goes with (by its stamp), so a power cut
  // between the two leaves the old texts unreadable rather than wrong.
  if (!cachefile::writeWhole(cachePath(f.key, ".body"), bodies) ||
      !cachefile::writeWhole(cachePath(f.key, ".meta"), metaText(f))) {
    log_w("news: couldn't keep %s on the card", f.title.c_str());
  }
  return f;
}

void syncOne(const std::string &key, int index, int total, int &ok, std::string &lastError) {
  std::string url;
  {
    std::lock_guard<std::mutex> lock(mu);
    const Feed *f = byKey(key);
    if (!f) return;
    url = f->url;
    phaseText = total > 1 ? "SYNCING " + std::to_string(index) + "/" + std::to_string(total)
                          : "SYNCING " + f->title;
  }
  ++gen;
  const uint32_t t0 = millis();
  std::string body, error;
  rss::NewsFeed fetched;
  bool good = fetchFeed(url, body, error);
  const uint32_t bytes = static_cast<uint32_t>(body.size());
  if (good && !rss::parseNews(body.data(), body.size(), kKeepPerFeed, kMaxBody, fetched)) {
    good = false;
    error = "NOT A FEED";
  }
  body.clear();
  body.shrink_to_fit();
  const time_t now = time(nullptr);
  Feed old;
  {
    std::lock_guard<std::mutex> lock(mu);
    Feed *f = byKey(key);
    if (!f) return;
    if (!good) {
      f->error = error;
      if (!f->failedSince) f->failedSince = now;
      lastError = error;
    }
    old = *f;
  }
  if (!good) {
    // Kept, so the failure shows offline too.
    cachefile::writeWhole(cachePath(old.key, ".meta"), metaText(old));
    log_w("news: sync of %s failed: %s", old.title.c_str(), error.c_str());
    ++gen;
    return;
  }
  Feed fresh = merge(old, fetched, now, bytes, millis() - t0);
  {
    std::lock_guard<std::mutex> lock(mu);
    Feed *f = byKey(key);
    if (f) {
      // Read or saved while the files were written: keep that, for the stories still kept.
      std::set<std::string> kept;
      for (const Item &it : fresh.items) kept.insert(it.id);
      for (const auto &kv : f->state) {
        if (kv.second && kept.count(kv.first)) fresh.state[kv.first] |= kv.second;
      }
      *f = fresh;
    }
  }
  saveState(fresh.key);
  ++ok;
  log_i("news: %s: %u stories (%u kept) from %u KB in %lu ms", fresh.title.c_str(),
        static_cast<unsigned>(fetched.stories.size()), static_cast<unsigned>(fresh.items.size()),
        static_cast<unsigned>(bytes >> 10), static_cast<unsigned long>(fresh.ms));
  ++gen;
}

void worker(void *) {
  bool connected = false;
  int ok = 0, done = 0, total = 0;
  std::string lastError;
  std::string wifiError;  // the join failed: nothing synced
  for (;;) {
    std::string key;
    {
      std::lock_guard<std::mutex> lock(mu);
      if (queue.empty() || cancelNow) {
        queue.clear();
        break;
      }
      key = queue.front();
      queue.pop_front();
      currentKey = key;
      if (done == 0) total = 1 + static_cast<int>(queue.size());
      if (!connected) phaseText = "JOINING WI-FI\xE2\x80\xA6";
    }
    ++gen;
    if (!connected) {
      std::string error;
      if (!wlan::connect(kConnectTimeoutMs, error)) {
        wifiError = error;
        std::lock_guard<std::mutex> lock(mu);
        queue.clear();
        currentKey.clear();
        break;
      }
      connected = true;
    }
    ++done;
    syncOne(key, done, std::max(total, done), ok, lastError);
    std::lock_guard<std::mutex> lock(mu);
    currentKey.clear();
  }
  if (connected) wlan::disconnect();
  {
    std::lock_guard<std::mutex> lock(mu);
    working = false;
    const int failed = done - ok;
    if (!wifiError.empty()) {
      setResult("WI-FI: " + wifiError);
    } else if (cancelNow) {
      setResult("SYNC STOPPED");
    } else if (failed == 0) {
      setResult("SYNCED " + std::to_string(ok) + (ok == 1 ? " FEED" : " FEEDS"));
    } else if (ok == 0) {
      setResult("SYNC FAILED: " + lastError);
    } else {
      setResult(std::to_string(failed) + " OF " + std::to_string(done) + " FEEDS FAILED");
    }
    phaseText.clear();
  }
  ++gen;
  vTaskDelete(nullptr);
}

}  // namespace

// --- Public ----------------------------------------------------------------------------------

bool load(bool create) {
  if (!sdcard::mounted()) {
    std::lock_guard<std::mutex> lock(mu);
    if (!working && !feeds.empty()) {
      feeds.clear();
      ++gen;
    }
    return false;
  }
  if (create) {
    mkdir(kRoot, 0777);
    if (!exists(kFeedsFile)) {
      // Written from RAM, not straight from flash (AGENTS.md, Gotchas).
      cachefile::writeWhole(kFeedsFile,
                 std::string("# INKDECK news: one RSS or Atom feed URL per line. Lines starting "
                             "with # are\n# skipped. The board syncs them while News is open, "
                             "and hourly on USB power.\n"));
    }
  }
  // Uids carry over for stories already in memory, so screens keep their places.
  std::map<std::string, std::map<std::string, Uid>> uids;
  {
    std::lock_guard<std::mutex> lock(mu);
    for (const Feed &f : feeds) {
      for (const Item &it : f.items) uids[f.key][it.id] = it.uid;
    }
  }
  std::vector<Feed> fresh;
  std::string list;
  if (cachefile::read(kFeedsFile, list)) {
    cachefile::forEachLine(list, [&](const std::string &raw) {
      const size_t b = raw.find_first_not_of(" \t");
      if (b == std::string::npos || raw[b] == '#') return;
      const std::string url = raw.substr(b, raw.find_last_not_of(" \t") - b + 1);
      if (url.compare(0, 7, "http://") != 0 && url.compare(0, 8, "https://") != 0) return;
      for (const Feed &f : fresh) {
        if (f.url == url) return;
      }
      Feed f;
      f.url = url;
      f.key = cachefile::hexKey(url);
      readMeta(f);
      if (f.title.empty()) f.title = article::host(url);
      f.state = readState(f.key);
      const auto known = uids.find(f.key);
      for (Item &it : f.items) {
        Uid uid = 0;
        if (known != uids.end()) {
          const auto u = known->second.find(it.id);
          if (u != known->second.end()) uid = u->second;
        }
        it.uid = uid ? uid : nextUid++;
      }
      fresh.push_back(std::move(f));
    });
  }
  std::lock_guard<std::mutex> lock(mu);
  feeds = std::move(fresh);
  ++gen;
  return true;
}

uint32_t generation() { return gen.load(); }

bool hasFeeds() {
  std::lock_guard<std::mutex> lock(mu);
  return !feeds.empty();
}

int feedCount() {
  std::lock_guard<std::mutex> lock(mu);
  return static_cast<int>(feeds.size());
}

FeedInfo feed(int i) {
  std::lock_guard<std::mutex> lock(mu);
  FeedInfo info;
  if (i < 0 || i >= static_cast<int>(feeds.size())) return info;
  const Feed &f = feeds[i];
  info.title = f.title;
  info.url = f.url;
  info.description = f.description;
  info.kind = f.kind;
  info.error = f.error;
  info.fetched = f.fetched;
  info.failedSince = f.failedSince;
  info.bytes = f.bytes;
  info.ms = f.ms;
  info.stories = static_cast<int>(f.items.size());
  const time_t now = time(nullptr);
  for (const Item &it : f.items) info.fresh += isFresh(f, it, now) ? 1 : 0;
  return info;
}

int failingCount() {
  std::lock_guard<std::mutex> lock(mu);
  int n = 0;
  for (const Feed &f : feeds) n += f.error.empty() ? 0 : 1;
  return n;
}

namespace {

StoryInfo describe(const Feed &f, int index, const Item &it, time_t now) {
  const auto st = f.state.find(it.id);
  const uint8_t flags = st == f.state.end() ? 0 : st->second;
  StoryInfo out;
  out.uid = it.uid;
  out.feed = index;
  out.feedTitle = f.title;
  out.title = it.title;
  out.author = it.author;
  out.summary = it.summary;
  out.link = it.link;
  out.published = it.published;
  out.words = it.words;
  out.full = it.full;
  out.read = flags & kRead;
  out.saved = flags & kSaved;
  out.fresh = isFresh(f, it, now);
  return out;
}

}  // namespace

std::vector<StoryInfo> stories(int feed, Show show) {
  std::vector<StoryInfo> out;
  {
    std::lock_guard<std::mutex> lock(mu);
    const time_t now = time(nullptr);
    for (int i = 0; i < static_cast<int>(feeds.size()); ++i) {
      if (feed >= 0 && i != feed) continue;
      const Feed &f = feeds[i];
      for (const Item &it : f.items) {
        StoryInfo s = describe(f, i, it, now);
        if (show == Show::Saved && !s.saved) continue;
        if (show == Show::New && !s.fresh) continue;
        out.push_back(std::move(s));
      }
    }
  }
  std::stable_sort(out.begin(), out.end(), [](const StoryInfo &a, const StoryInfo &b) {
    return a.published > b.published;
  });
  return out;
}

bool story(Uid uid, StoryInfo &out) {
  std::lock_guard<std::mutex> lock(mu);
  const time_t now = time(nullptr);
  for (int i = 0; i < static_cast<int>(feeds.size()); ++i) {
    for (const Item &it : feeds[i].items) {
      if (it.uid != uid) continue;
      out = describe(feeds[i], i, it, now);
      return true;
    }
  }
  return false;
}

bool body(Uid uid, std::string &markdown) {
  std::string key, stamp, summary;
  uint32_t offset = 0, length = 0;
  {
    std::lock_guard<std::mutex> lock(mu);
    bool found = false;
    for (const Feed &f : feeds) {
      for (const Item &it : f.items) {
        if (it.uid != uid) continue;
        key = f.key;
        stamp = f.stamp;
        offset = it.offset;
        length = it.length;
        summary = it.summary;
        found = true;
        break;
      }
      if (found) break;
    }
    if (!found) return false;
  }
  if (readBody(key, stamp, offset, length, markdown)) return true;
  markdown = summary;  // the file is gone or from another sync
  return !summary.empty();
}

int freshCount(int feed) {
  std::lock_guard<std::mutex> lock(mu);
  const time_t now = time(nullptr);
  int n = 0;
  for (int i = 0; i < static_cast<int>(feeds.size()); ++i) {
    if (feed >= 0 && i != feed) continue;
    for (const Item &it : feeds[i].items) n += isFresh(feeds[i], it, now) ? 1 : 0;
  }
  return n;
}

int savedCount() {
  std::lock_guard<std::mutex> lock(mu);
  int n = 0;
  for (const Feed &f : feeds) {
    for (const auto &kv : f.state) n += (kv.second & kSaved) ? 1 : 0;
  }
  return n;
}

int storyCount() {
  std::lock_guard<std::mutex> lock(mu);
  int n = 0;
  for (const Feed &f : feeds) n += static_cast<int>(f.items.size());
  return n;
}

namespace {

// Sets or clears a flag on a story; returns its feed's key if that changed anything.
std::string setFlag(Uid uid, uint8_t flag, bool on) {
  std::lock_guard<std::mutex> lock(mu);
  for (Feed &f : feeds) {
    for (const Item &it : f.items) {
      if (it.uid != uid) continue;
      uint8_t &flags = f.state[it.id];
      const uint8_t before = flags;
      flags = on ? (flags | flag) : (flags & ~flag);
      if (!flags) f.state.erase(it.id);
      if (flags == before) return {};
      ++gen;
      return f.key;
    }
  }
  return {};
}

}  // namespace

void setRead(Uid uid, bool read) {
  const std::string key = setFlag(uid, kRead, read);
  if (!key.empty()) saveState(key);
}

void setSaved(Uid uid, bool saved) {
  const std::string key = setFlag(uid, kSaved, saved);
  if (!key.empty()) saveState(key);
}

void markAllRead(int feed) {
  std::vector<std::string> changed;
  {
    std::lock_guard<std::mutex> lock(mu);
    for (int i = 0; i < static_cast<int>(feeds.size()); ++i) {
      if (feed >= 0 && i != feed) continue;
      Feed &f = feeds[i];
      bool any = false;
      for (const Item &it : f.items) {
        uint8_t &flags = f.state[it.id];
        if (!(flags & kRead)) {
          flags |= kRead;
          any = true;
        }
      }
      if (any) changed.push_back(f.key);
    }
    if (!changed.empty()) ++gen;
  }
  for (const std::string &key : changed) saveState(key);
}

void sync(int feed) {
  std::lock_guard<std::mutex> lock(mu);
  bool added = false;
  for (int i = 0; i < static_cast<int>(feeds.size()); ++i) {
    if (feed >= 0 && i != feed) continue;
    if (feeds[i].key == currentKey ||
        std::find(queue.begin(), queue.end(), feeds[i].key) != queue.end()) {
      continue;
    }
    queue.push_back(feeds[i].key);
    added = true;
  }
  if (feed < 0) lastAttemptMs = millis() | 1;
  if (!added) return;
  ++gen;
  if (working) return;
  working = true;
  cancelNow = false;
  // TLS, the parser and the file writes need room.
  if (xTaskCreate(worker, "news", 12288, nullptr, 1, nullptr) != pdPASS) {
    working = false;
    queue.clear();
    setResult("NO MEMORY FOR THE SYNC TASK");
  }
}

void cancel() { cancelNow = true; }

bool busy() {
  std::lock_guard<std::mutex> lock(mu);
  return working;
}

Activity activity() {
  std::lock_guard<std::mutex> lock(mu);
  Activity a;
  a.busy = working;
  if (working) {
    a.text = phaseText;
  } else if (!result.empty() && time(nullptr) - resultAt < kResultShownFor) {
    a.text = result;
  }
  return a;
}

time_t lastSync() {
  std::lock_guard<std::mutex> lock(mu);
  time_t t = 0;
  for (const Feed &f : feeds) t = std::max(t, f.fetched);
  return t;
}

void poll(bool wanted, uint32_t everyS) {
  if (!wanted || !wlan::configured()) return;
  {
    std::lock_guard<std::mutex> lock(mu);
    if (working || feeds.empty()) return;
    if (lastAttemptMs && millis() - lastAttemptMs < everyS * 1000) return;
    if (rtc::timeValid()) {
      time_t newest = 0;
      for (const Feed &f : feeds) newest = std::max(newest, f.fetched);
      if (newest && time(nullptr) - newest < static_cast<time_t>(everyS)) return;
    }
  }
  sync();
}

}  // namespace news
