#include "podcast_library.h"

#include <Arduino.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "article.h"
#include "audio.h"
#include "cache_file.h"
#include "dirlist.h"
#include "netget.h"
#include "rss.h"
#include "sdcard.h"
#include "text.h"
#include "wlan.h"

namespace podcasts {
namespace {

constexpr const char *kCacheDir = "/sdcard/.inkdeck/podcasts";
constexpr time_t kNewFor = 7 * 86400;
constexpr size_t kFeedCap = 3 * 1024 * 1024;     // a feed is read until kMaxEpisodes items
constexpr int64_t kSpareBytes = 50ll << 20;      // left free on the card after a download
constexpr uint32_t kConnectTimeoutMs = 20000;
constexpr time_t kResultShownFor = 120;
constexpr uint32_t kSaveEveryS = 30;
constexpr uint32_t kPlayedWithinS = 45;
constexpr int kChunk = 16 * 1024;

// --- The library ---------------------------------------------------------------------------

struct EpState {
  uint32_t position = 0;  // seconds
  bool played = false;
  time_t last = 0;        // when it was last listened to
  std::string file;       // feeds: the downloaded file's name in the show's folder
};

struct Show {
  bool feed = false;
  std::string key;     // names its files in kCacheDir
  std::string url;     // feeds
  std::string folder;  // under kRoot
  std::string title;
  std::string description;
  std::string error;
  time_t fetched = 0;
  bool listed = false;  // episodes known: a feed's from its cache or a refresh, a folder listed
  bool stateLoaded = false;
  std::vector<rss::Episode> episodes;  // a folder's: guid = file name, bytes -1 until looked up
  std::map<std::string, EpState> state;  // by episode id: the guid (a folder's: file name)
};

std::mutex mu;  // guards everything in this block and the worker's state below
std::vector<Show> shows;
std::atomic<uint32_t> gen{1};

Show *byKey(const std::string &key) {
  for (Show &s : shows) {
    if (s.key == key) return &s;
  }
  return nullptr;
}

// --- The worker's state (guarded by mu, except the atomics) ---------------------------------

enum class JobKind : uint8_t { Refresh, Download };
struct Job {
  JobKind kind;
  std::string key;  // Download: the show
  std::string id;   // Download: the episode
};
std::deque<Job> jobs;
bool working = false;
bool hasCurrent = false;
Job current{JobKind::Refresh, "", ""};
std::string currentTitle;
std::string phaseText;  // what a refresh is doing
std::string result;
time_t resultAt = 0;
std::atomic<int64_t> dlDone{0};
std::atomic<int64_t> dlTotal{0};
std::atomic<bool> cancelNow{false};

std::atomic<int64_t> used{-1};
std::atomic<int64_t> freeSpace{-1};
std::atomic<bool> scanning{false};

// --- Small helpers -------------------------------------------------------------------------

int64_t fileSize(const std::string &path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0 ? static_cast<int64_t>(st.st_size) : -1;
}

void ensureCacheDir() { cachefile::ensureDir(kCacheDir); }

// A FAT-safe file or folder name: ASCII, no \ / : * ? " < > |, no trailing dots or spaces.
std::string safeName(const std::string &utf8, size_t maxLen) {
  const std::string ascii = text::toAscii(utf8);
  std::string out;
  bool space = false;
  for (const char c : ascii) {
    const bool bad = static_cast<uint8_t>(c) < 0x20 || strchr("\\/:*?\"<>|", c) != nullptr;
    if (bad || c == ' ') {
      space = !out.empty();
      continue;
    }
    if (out.size() + (space ? 2 : 1) > maxLen) break;
    if (space) out += ' ';
    space = false;
    out += c;
  }
  while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
  return out.empty() ? "Untitled" : out;
}

bool isMp3Episode(const rss::Episode &e) {
  if (e.type.find("mpeg") != std::string::npos || e.type.find("mp3") != std::string::npos) {
    return true;
  }
  if (!e.type.empty()) return false;
  const std::string path = e.url.substr(0, e.url.find('?'));
  return path.size() > 4 && strcasecmp(path.c_str() + path.size() - 4, ".mp3") == 0;
}

// "01 - Funny Moms.mp3" -> 1, "Funny Moms"; anything else keeps its name.
void folderTitle(const std::string &file, std::string &title, int &number) {
  const std::string stem = file.substr(0, file.rfind('.'));
  size_t i = 0;
  while (i < stem.size() && isdigit(static_cast<unsigned char>(stem[i]))) ++i;
  size_t j = i;
  while (j < stem.size() && strchr(" -._", stem[j])) ++j;
  if (i > 0 && i <= 4 && j > i && j < stem.size()) {
    number = atoi(stem.substr(0, i).c_str());
    title = stem.substr(j);
  } else {
    number = -1;
    title = stem;
  }
}

std::string downloadName(const rss::Episode &e) {
  char date[16] = "";
  if (e.published) {
    tm t;
    gmtime_r(&e.published, &t);
    strftime(date, sizeof date, "%Y-%m-%d ", &t);
  }
  return date + safeName(e.title, 80) + ".mp3";
}

std::string folderPath(const std::string &folder) { return std::string(kRoot) + "/" + folder; }

// --- Files in kCacheDir --------------------------------------------------------------------

std::string cachePath(const std::string &key, const char *ext) {
  return std::string(kCacheDir) + "/" + key + ext;
}

void readFeedCache(Show &s) {
  std::string data;
  if (!cachefile::read(cachePath(s.key, ".feed"), data)) return;
  bool header = false;
  cachefile::forEachLine(data, [&](const std::string &line) {
    const std::vector<std::string> f = cachefile::split(line);
    if (f[0] == "F" && f.size() >= 6) {
      s.fetched = atoll(f[2].c_str());
      s.folder = f[3];
      s.title = f[4];
      s.description = f[5];
      header = true;
    } else if (f[0] == "E" && f.size() >= 10 && header) {
      rss::Episode e;
      e.guid = f[1];
      e.published = atoll(f[2].c_str());
      e.seconds = strtoul(f[3].c_str(), nullptr, 10);
      e.bytes = atoll(f[4].c_str());
      e.number = atoi(f[5].c_str());
      e.type = f[6];
      e.url = f[7];
      e.title = f[8];
      e.description = f[9];
      s.episodes.push_back(std::move(e));
    }
  });
  s.listed = header;
}

void writeFeedCache(const Show &s) {
  std::string out = "INKFEED1\n";
  char num[96];
  snprintf(num, sizeof num, "%lld", static_cast<long long>(s.fetched));
  out += "F\t" + cachefile::field(s.url) + "\t" + num + "\t" + cachefile::field(s.folder) + "\t" + cachefile::field(s.title) +
         "\t" + cachefile::field(s.description) + "\n";
  for (const rss::Episode &e : s.episodes) {
    snprintf(num, sizeof num, "%lld\t%lu\t%lld\t%d", static_cast<long long>(e.published),
             static_cast<unsigned long>(e.seconds), static_cast<long long>(e.bytes), e.number);
    out += "E\t" + cachefile::field(e.guid) + "\t" + num + "\t" + cachefile::field(e.type) + "\t" + cachefile::field(e.url) +
           "\t" + cachefile::field(e.title) + "\t" + cachefile::field(e.description) + "\n";
  }
  cachefile::writeWhole(cachePath(s.key, ".feed"), out);
}

std::map<std::string, EpState> readState(const std::string &key) {
  std::map<std::string, EpState> state;
  std::string data;
  if (!cachefile::read(cachePath(key, ".state"), data)) return state;
  cachefile::forEachLine(data, [&](const std::string &line) {
    const std::vector<std::string> f = cachefile::split(line);
    if (f[0] != "S" || f.size() < 6) return;
    EpState st;
    st.position = strtoul(f[2].c_str(), nullptr, 10);
    st.played = f[3] == "1";
    st.last = atoll(f[4].c_str());
    st.file = f[5];
    state[f[1]] = st;
  });
  return state;
}

void writeState(const std::string &key, const std::map<std::string, EpState> &state) {
  std::string out = "INKSTATE1\n";
  char num[64];
  for (const auto &kv : state) {
    const EpState &st = kv.second;
    snprintf(num, sizeof num, "%lu\t%d\t%lld", static_cast<unsigned long>(st.position),
             st.played ? 1 : 0, static_cast<long long>(st.last));
    out += "S\t" + cachefile::field(kv.first) + "\t" + num + "\t" + cachefile::field(st.file) + "\n";
  }
  cachefile::writeWhole(cachePath(key, ".state"), out);
}

// A feed's downloads that have gone missing (deleted on a computer) are forgotten.
void checkFiles(Show &s) {
  for (auto &kv : s.state) {
    if (!kv.second.file.empty() && fileSize(folderPath(s.folder) + "/" + kv.second.file) < 0) {
      kv.second.file.clear();
    }
  }
}

std::vector<rss::Episode> listFolder(const std::string &folder) {
  std::vector<rss::Episode> out;
  DirList dir(kRoot);
  if (!dir.open(folderPath(folder))) return out;
  for (const DirList::Entry &entry : dir.entries()) {
    if (entry.isDir || !DirList::isMp3(entry.name)) continue;
    rss::Episode e;
    e.guid = entry.name;
    e.bytes = -1;
    folderTitle(entry.name, e.title, e.number);
    out.push_back(std::move(e));
  }
  return out;
}

// --- Marks (mu held) -----------------------------------------------------------------------

bool queued(const std::string &key, const std::string &id) {
  for (const Job &j : jobs) {
    if (j.kind == JobKind::Download && j.key == key && j.id == id) return true;
  }
  return false;
}

bool downloadingNow(const std::string &key, const std::string &id) {
  return working && hasCurrent && current.kind == JobKind::Download && current.key == key &&
         current.id == id;
}

int percentOf(int64_t done, int64_t total) {
  return total > 0 ? static_cast<int>(std::min<int64_t>(100, done * 100 / total)) : 0;
}

EpisodeInfo describe(Show &s, int index, bool forList) {
  rss::Episode &e = s.episodes[index];
  EpisodeInfo info;
  info.title = e.title;
  info.description = e.description;
  info.published = e.published;
  info.seconds = e.seconds;
  info.number = e.number;
  const auto it = s.state.find(e.guid);
  const EpState st = it == s.state.end() ? EpState{} : it->second;
  info.positionS = st.position;
  if (!s.feed) {
    info.path = folderPath(s.folder) + "/" + e.guid;
    info.playable = DirList::isMp3(e.guid);
    if (e.bytes < 0) e.bytes = std::max<int64_t>(0, fileSize(info.path));
    info.bytes = e.bytes;
  } else {
    info.playable = isMp3Episode(e);
    if (!st.file.empty()) info.path = folderPath(s.folder) + "/" + st.file;
    info.bytes = e.bytes > 0 ? e.bytes : -1;
  }
  if (downloadingNow(s.key, e.guid)) {
    info.mark = Mark::Downloading;
    info.percent = percentOf(dlDone.load(), dlTotal.load());
  } else if (queued(s.key, e.guid)) {
    info.mark = Mark::Queued;
  } else if (st.played) {
    info.mark = Mark::Played;
  } else if (st.position > 0) {
    info.mark = Mark::Started;
  } else if (s.feed && !info.path.empty()) {
    info.mark = Mark::Cached;
  } else if (s.feed) {
    const int64_t part = s.folder.empty()
                             ? -1
                             : fileSize(folderPath(s.folder) + "/" + downloadName(e) + ".part");
    if (part > 0) {
      info.mark = Mark::Partial;
      info.percent = percentOf(part, e.bytes);
    } else if (e.published && time(nullptr) - e.published < kNewFor) {
      info.mark = Mark::New;
    }
  }
  (void)forList;
  return info;
}

int freshCount(Show &s) {
  if (!s.feed) return 0;
  int n = 0;
  const time_t now = time(nullptr);
  for (const rss::Episode &e : s.episodes) {
    if (!e.published || now - e.published >= kNewFor) continue;
    const auto it = s.state.find(e.guid);
    if (it == s.state.end() || (!it->second.played && !it->second.position && it->second.file.empty())) {
      ++n;
    }
  }
  return n;
}

void setResult(std::string text) {
  result = std::move(text);
  resultAt = time(nullptr);
}

// --- Space ---------------------------------------------------------------------------------

void scanTask(void *) {
  int64_t total = 0;
  if (DIR *root = opendir(kRoot)) {
    while (dirent *d = readdir(root)) {
      if (d->d_name[0] == '.' || d->d_type != DT_DIR) continue;
      const std::string dir = folderPath(d->d_name);
      if (DIR *sub = opendir(dir.c_str())) {
        while (dirent *f = readdir(sub)) {
          if (f->d_type == DT_REG) total += std::max<int64_t>(0, fileSize(dir + "/" + f->d_name));
        }
        closedir(sub);
      }
    }
    closedir(root);
  }
  used = total;
  freeSpace = static_cast<int64_t>(sdcard::freeBytes());
  log_i("podcasts: %lld MB in %s, %lld MB free", static_cast<long long>(total >> 20), kRoot,
        static_cast<long long>(freeSpace.load() >> 20));
  scanning = false;
  ++gen;
  vTaskDelete(nullptr);
}

void startScan() {
  if (scanning.exchange(true)) return;
  if (xTaskCreate(scanTask, "podscan", 4096, nullptr, 1, nullptr) != pdPASS) scanning = false;
}

// --- Network jobs (worker task) ------------------------------------------------------------

void runRefresh() {
  std::vector<std::pair<std::string, std::string>> feeds;  // key, url
  {
    std::lock_guard<std::mutex> lock(mu);
    for (const Show &s : shows) {
      if (s.feed) feeds.emplace_back(s.key, s.url);
    }
  }
  int ok = 0;
  std::string lastError;
  for (size_t i = 0; i < feeds.size(); ++i) {
    {
      std::lock_guard<std::mutex> lock(mu);
      phaseText = "REFRESHING " + std::to_string(i + 1) + "/" + std::to_string(feeds.size());
    }
    ++gen;
    const uint32_t t0 = millis();
    std::string body, error;
    rss::Channel channel;
    bool good = net::getFeed(feeds[i].second, body, error, kFeedCap, kMaxEpisodes);
    if (good && !rss::parse(body.data(), body.size(), kMaxEpisodes, channel)) {
      good = false;
      error = "NOT A PODCAST FEED";
    }
    Show copy;
    {
      std::lock_guard<std::mutex> lock(mu);
      Show *s = byKey(feeds[i].first);
      if (!s) continue;
      if (good) {
        if (!channel.title.empty()) s->title = channel.title;
        s->description = channel.description;
        s->episodes = std::move(channel.episodes);
        s->fetched = time(nullptr);
        s->error.clear();
        s->listed = true;
        if (s->folder.empty()) s->folder = safeName(s->title, 40);
        copy = *s;
      } else {
        s->error = error;
        lastError = error;
      }
    }
    if (good) {
      writeFeedCache(copy);
      ++ok;
      log_i("podcasts: %s: %u episodes from %u KB in %lu ms", copy.title.c_str(),
            static_cast<unsigned>(copy.episodes.size()), static_cast<unsigned>(body.size() >> 10),
            static_cast<unsigned long>(millis() - t0));
    } else {
      log_w("podcasts: refresh of feed %u failed: %s", static_cast<unsigned>(i + 1), error.c_str());
    }
    ++gen;
  }
  std::lock_guard<std::mutex> lock(mu);
  const int failed = static_cast<int>(feeds.size()) - ok;
  setResult(failed == 0 ? "UPDATED " + std::to_string(ok) + (ok == 1 ? " FEED" : " FEEDS")
            : ok == 0   ? "REFRESH FAILED: " + lastError
                        : std::to_string(failed) + " OF " + std::to_string(feeds.size()) +
                              " FEEDS FAILED");
}

void runDownload(const std::string &key, const std::string &id) {
  std::string url, folder, name, title;
  int64_t expected = 0;
  {
    std::lock_guard<std::mutex> lock(mu);
    Show *s = byKey(key);
    if (!s) return;
    const auto it = std::find_if(s->episodes.begin(), s->episodes.end(),
                                 [&](const rss::Episode &e) { return e.guid == id; });
    if (it == s->episodes.end()) return;
    if (s->folder.empty()) s->folder = safeName(s->title, 40);
    url = it->url;
    folder = s->folder;
    name = downloadName(*it);
    title = it->title;
    expected = it->bytes;
    currentTitle = title;
  }
  mkdir(kRoot, 0777);
  mkdir(folderPath(folder).c_str(), 0777);
  const std::string path = folderPath(folder) + "/" + name;
  const std::string part = path + ".part";
  const std::string partId = part + ".id";  // the version of the file the .part holds
  int64_t have = std::max<int64_t>(0, fileSize(part));
  std::string version;
  if (have > 0 && (!cachefile::read(partId, version) || version.empty())) have = 0;  // can't resume safely
  dlDone = have;
  dlTotal = expected;

  const int64_t free = static_cast<int64_t>(sdcard::freeBytes());
  if (free > 0 && expected > 0 && free < expected - have + kSpareBytes) {
    std::lock_guard<std::mutex> lock(mu);
    setResult("CARD FULL: " + title);
    return;
  }

  const uint32_t t0 = millis();
  std::string error;
  net::Get get;
  bool complete = false;
  if (!get.open(url, error, have, version)) {
    std::lock_guard<std::mutex> lock(mu);
    setResult("DOWNLOAD FAILED: " + error);
    return;
  }
  if (get.status() == 416) {
    complete = have > 0;  // nothing past what we have: it was all there
  } else {
    if (get.status() != 206) have = 0;  // the whole file (it changed, or no range support)
    log_i("podcasts: HTTP %d, %s at %lld KB, %lld KB to come", get.status(),
          have ? "resuming" : "starting", static_cast<long long>(have >> 10),
          static_cast<long long>(get.length() >> 10));
    if (!have) {
      const std::string id = get.validator();
      if (id.empty() || !cachefile::writeWhole(partId, id)) remove(partId.c_str());
    }
    FILE *f = fopen(part.c_str(), have ? "ab" : "wb");
    if (!f) {
      std::lock_guard<std::mutex> lock(mu);
      setResult("CAN'T WRITE TO THE CARD");
      return;
    }
    setvbuf(f, nullptr, _IOFBF, 32 * 1024);
    if (get.length() >= 0) dlTotal = have + get.length();
    dlDone = have;
    std::unique_ptr<char[]> buf(new char[kChunk]);
    int64_t got = 0;
    while (!cancelNow.load()) {
      const int n = get.read(buf.get(), kChunk);
      if (n < 0) {
        error = "CONNECTION LOST";
        break;
      }
      if (n == 0) {
        complete = true;
        break;
      }
      if (fwrite(buf.get(), 1, n, f) != static_cast<size_t>(n)) {
        error = "SD WRITE FAILED";
        break;
      }
      got += n;
      dlDone = have + got;
      // Commit the size on the card every megabyte: FAT records it only on a sync or close, and
      // a power cut should leave a .part that GET can resume.
      if ((got & ~((1 << 20) - 1)) != ((got - n) & ~((1 << 20) - 1))) {
        fflush(f);
        fsync(fileno(f));
      }
    }
    fclose(f);
    const uint32_t ms = millis() - t0;
    log_i("podcasts: %lld KB in %lu ms (%lu KB/s)%s", static_cast<long long>(got >> 10),
          static_cast<unsigned long>(ms), static_cast<unsigned long>(ms ? (got >> 10) * 1000 / ms : 0),
          complete ? "" : ", stopped");
  }
  get.close();

  std::lock_guard<std::mutex> lock(mu);
  if (!complete) {
    setResult(cancelNow.load() ? "STOPPED: " + title : "DOWNLOAD FAILED: " + error);
    return;
  }
  remove(path.c_str());
  remove(partId.c_str());
  if (rename(part.c_str(), path.c_str()) != 0) {
    setResult("CAN'T RENAME THE DOWNLOAD");
    return;
  }
  Show *s = byKey(key);
  if (!s) return;
  s->state[id].file = name;
  const std::map<std::string, EpState> state = s->state;
  if (used.load() >= 0) used += fileSize(path);
  setResult("SAVED: " + title);
  // Written with mu held: a short file, and it keeps the state in step.
  writeState(key, state);
}

void worker(void *) {
  bool connected = false;
  for (;;) {
    Job job;
    {
      std::lock_guard<std::mutex> lock(mu);
      if (jobs.empty()) {
        working = false;
        hasCurrent = false;
        break;
      }
      job = jobs.front();
      jobs.pop_front();
      current = job;
      hasCurrent = true;
      currentTitle.clear();
      phaseText = connected ? "" : "JOINING WI-FI\xE2\x80\xA6";
    }
    dlDone = 0;  // not the last download's numbers while this one gets going
    dlTotal = 0;
    ++gen;
    if (!connected) {
      std::string error;
      if (!wlan::connect(kConnectTimeoutMs, error)) {
        std::lock_guard<std::mutex> lock(mu);
        jobs.clear();
        working = false;
        hasCurrent = false;
        setResult("WI-FI: " + error);
        break;
      }
      connected = true;
    }
    cancelNow = false;
    if (job.kind == JobKind::Refresh) {
      runRefresh();
    } else {
      runDownload(job.key, job.id);
    }
    ++gen;
  }
  if (connected) wlan::disconnect();
  ++gen;
  vTaskDelete(nullptr);
}

void enqueue(Job job) {
  std::lock_guard<std::mutex> lock(mu);
  for (const Job &j : jobs) {
    if (j.kind == job.kind && j.key == job.key && j.id == job.id) return;
  }
  if (hasCurrent && current.kind == job.kind && current.key == job.key && current.id == job.id) {
    return;
  }
  jobs.push_back(std::move(job));
  ++gen;
  if (working) return;
  working = true;
  // TLS, the feed parser and the file writes need room.
  if (xTaskCreate(worker, "podcasts", 12288, nullptr, 1, nullptr) != pdPASS) {
    working = false;
    jobs.clear();
    setResult("NO MEMORY FOR THE DOWNLOAD TASK");
  }
}

// --- Following the player (UI task) --------------------------------------------------------

std::string trackedPath;
std::string trackedKey;
std::string trackedId;
uint32_t lastSavedS = 0;
bool savedOnce = false;

void follow(const std::string &path) {
  trackedPath = path;
  trackedKey.clear();
  trackedId.clear();
  savedOnce = false;
  const std::string prefix = std::string(kRoot) + "/";
  if (path.compare(0, prefix.size(), prefix) != 0) return;
  const std::string rest = path.substr(prefix.size());
  const size_t slash = rest.find('/');
  if (slash == std::string::npos) return;
  const std::string folder = rest.substr(0, slash), file = rest.substr(slash + 1);
  std::string key;
  bool needState = false;
  {
    std::lock_guard<std::mutex> lock(mu);
    for (Show &s : shows) {
      if (strcasecmp(s.folder.c_str(), folder.c_str()) != 0) continue;
      if (!s.feed) {
        trackedKey = s.key;
        trackedId = file;
      } else {
        for (const auto &kv : s.state) {
          if (kv.second.file == file) {
            trackedKey = s.key;
            trackedId = kv.first;
          }
        }
      }
      needState = !trackedKey.empty() && !s.stateLoaded;
      key = s.key;
      break;
    }
  }
  if (needState) {  // a folder show played from Files before the Podcasts screen opened it
    std::map<std::string, EpState> state = readState(key);
    std::lock_guard<std::mutex> lock(mu);
    if (Show *s = byKey(key)) {
      if (!s->stateLoaded) {
        s->state = std::move(state);
        s->stateLoaded = true;
      }
    }
  }
}

}  // namespace

// --- Public --------------------------------------------------------------------------------

bool load(bool create) {
  if (!sdcard::mounted()) {
    std::lock_guard<std::mutex> lock(mu);
    shows.clear();
    ++gen;
    return false;
  }
  if (create) {
    mkdir(kRoot, 0777);
    if (fileSize(kFeedsFile) < 0) {
      // Written from RAM, not straight from flash (AGENTS.md, Gotchas).
      cachefile::writeWhole(kFeedsFile,
                      "# INKDECK podcasts: one RSS feed URL per line. Lines starting with # are "
                      "skipped.\n"
                      "# Downloads go to a folder per show next to this file. Folders of MP3s "
                      "copied\n"
                      "# here by hand show up as shows too.\n");
    }
  }
  std::vector<Show> fresh;
  std::string list;
  if (cachefile::read(kFeedsFile, list)) {
    cachefile::forEachLine(list, [&](const std::string &raw) {
      size_t b = raw.find_first_not_of(" \t");
      if (b == std::string::npos || raw[b] == '#') return;
      std::string url = raw.substr(b, raw.find_last_not_of(" \t") - b + 1);
      if (url.compare(0, 7, "http://") != 0 && url.compare(0, 8, "https://") != 0) return;
      for (const Show &s : fresh) {
        if (s.url == url) return;
      }
      Show s;
      s.feed = true;
      s.url = url;
      s.key = cachefile::hexKey(url);
      readFeedCache(s);
      if (s.title.empty()) s.title = article::host(url);
      s.state = readState(s.key);
      s.stateLoaded = true;
      checkFiles(s);
      fresh.push_back(std::move(s));
    });
  }
  DirList root(kRoot);
  if (root.open(kRoot)) {
    for (const DirList::Entry &entry : root.entries()) {
      if (!entry.isDir || entry.name[0] == '.') continue;
      const bool isFeedFolder = std::any_of(fresh.begin(), fresh.end(), [&](const Show &s) {
        return s.feed && strcasecmp(s.folder.c_str(), entry.name.c_str()) == 0;
      });
      if (isFeedFolder) continue;
      Show s;
      s.folder = entry.name;
      s.title = entry.name;
      s.key = cachefile::hexKey("dir:" + entry.name);
      fresh.push_back(std::move(s));
    }
  }
  {
    std::lock_guard<std::mutex> lock(mu);
    shows = std::move(fresh);
    ++gen;
  }
  startScan();
  return true;
}

uint32_t generation() { return gen.load(); }

int showCount() {
  std::lock_guard<std::mutex> lock(mu);
  return static_cast<int>(shows.size());
}

ShowInfo show(int i) {
  std::lock_guard<std::mutex> lock(mu);
  ShowInfo info;
  if (i < 0 || i >= static_cast<int>(shows.size())) return info;
  Show &s = shows[i];
  info.title = s.title;
  info.description = s.description;
  info.folder = s.folder;
  info.error = s.error;
  info.feed = s.feed;
  info.fetched = s.fetched;
  info.fresh = freshCount(s);
  info.episodes = s.listed ? static_cast<int>(s.episodes.size()) : (s.feed ? 0 : -1);
  return info;
}

int episodeCount(int i) {
  std::string folder, key;
  {
    std::lock_guard<std::mutex> lock(mu);
    if (i < 0 || i >= static_cast<int>(shows.size())) return 0;
    Show &s = shows[i];
    if (s.feed || s.listed) return static_cast<int>(s.episodes.size());
    folder = s.folder;
    key = s.key;
  }
  // List the folder without holding the lock: hundreds of files take a moment.
  std::vector<rss::Episode> episodes = listFolder(folder);
  std::map<std::string, EpState> state = readState(key);
  std::lock_guard<std::mutex> lock(mu);
  Show *s = byKey(key);
  if (!s) return 0;
  if (!s->listed) {
    s->episodes = std::move(episodes);
    s->listed = true;
    if (!s->stateLoaded) {
      s->state = std::move(state);
      s->stateLoaded = true;
    }
    ++gen;
  }
  return static_cast<int>(s->episodes.size());
}

EpisodeInfo episode(int i, int e) {
  std::lock_guard<std::mutex> lock(mu);
  if (i < 0 || i >= static_cast<int>(shows.size())) return {};
  Show &s = shows[i];
  if (e < 0 || e >= static_cast<int>(s.episodes.size())) return {};
  return describe(s, e, false);
}

int resumeIndex(int i) {
  const int n = episodeCount(i);
  std::lock_guard<std::mutex> lock(mu);
  if (i < 0 || i >= static_cast<int>(shows.size()) || n == 0) return 0;
  const Show &s = shows[i];
  int best = -1;
  time_t bestAt = 0;
  for (int e = 0; e < static_cast<int>(s.episodes.size()); ++e) {
    const auto it = s.state.find(s.episodes[e].guid);
    if (it != s.state.end() && it->second.last > bestAt) {
      best = e;
      bestAt = it->second.last;
    }
  }
  if (best < 0) return 0;
  // A folder is listened to in order: after one played to the end comes the next.
  const auto it = s.state.find(s.episodes[best].guid);
  if (!s.feed && it->second.played && best + 1 < n) return best + 1;
  return best;
}

int freshTotal() {
  std::lock_guard<std::mutex> lock(mu);
  int n = 0;
  for (Show &s : shows) n += freshCount(s);
  return n;
}

bool hasFeeds() {
  std::lock_guard<std::mutex> lock(mu);
  return std::any_of(shows.begin(), shows.end(), [](const Show &s) { return s.feed; });
}

void refresh() { enqueue({JobKind::Refresh, "", ""}); }

void get(int i, int e) {
  std::string key, id;
  {
    std::lock_guard<std::mutex> lock(mu);
    if (i < 0 || i >= static_cast<int>(shows.size())) return;
    Show &s = shows[i];
    if (!s.feed || e < 0 || e >= static_cast<int>(s.episodes.size())) return;
    if (!isMp3Episode(s.episodes[e])) return;
    const auto it = s.state.find(s.episodes[e].guid);
    if (it != s.state.end() && !it->second.file.empty()) return;  // already on the card
    key = s.key;
    id = s.episodes[e].guid;
  }
  enqueue({JobKind::Download, key, id});
}

void cancel(int i, int e) {
  std::lock_guard<std::mutex> lock(mu);
  if (i < 0 || i >= static_cast<int>(shows.size())) return;
  Show &s = shows[i];
  if (e < 0 || e >= static_cast<int>(s.episodes.size())) return;
  const std::string &id = s.episodes[e].guid;
  jobs.erase(std::remove_if(jobs.begin(), jobs.end(),
                            [&](const Job &j) {
                              return j.kind == JobKind::Download && j.key == s.key && j.id == id;
                            }),
             jobs.end());
  if (downloadingNow(s.key, id)) cancelNow = true;
  ++gen;
}

void togglePlayed(int i, int e) {
  std::string key;
  std::map<std::string, EpState> state;
  {
    std::lock_guard<std::mutex> lock(mu);
    if (i < 0 || i >= static_cast<int>(shows.size())) return;
    Show &s = shows[i];
    if (e < 0 || e >= static_cast<int>(s.episodes.size())) return;
    EpState &st = s.state[s.episodes[e].guid];
    st.played = !st.played;
    st.position = 0;
    st.last = time(nullptr);
    key = s.key;
    state = s.state;
    ++gen;
  }
  writeState(key, state);
}

Activity activity() {
  std::lock_guard<std::mutex> lock(mu);
  Activity a;
  a.busy = working;
  if (working) {
    if (hasCurrent && current.kind == JobKind::Download && !currentTitle.empty()) {
      a.text = "DL " + std::to_string(percentOf(dlDone.load(), dlTotal.load())) + "% \xC2\xB7 " +
               currentTitle;
    } else {
      a.text = phaseText;
    }
    if (!jobs.empty()) a.text = "+" + std::to_string(jobs.size()) + " \xC2\xB7 " + a.text;
  } else if (!result.empty() && time(nullptr) - resultAt < kResultShownFor) {
    a.text = result;
  }
  return a;
}

bool busy() {
  std::lock_guard<std::mutex> lock(mu);
  return working;
}

int64_t usedBytes() { return used.load(); }
int64_t freeBytes() { return freeSpace.load(); }

bool play(int i, int e) {
  std::vector<std::string> playlist;
  uint32_t startMs = 0;
  std::string key;
  std::map<std::string, EpState> state;
  {
    std::lock_guard<std::mutex> lock(mu);
    if (i < 0 || i >= static_cast<int>(shows.size())) return false;
    Show &s = shows[i];
    if (e < 0 || e >= static_cast<int>(s.episodes.size())) return false;
    const EpisodeInfo info = describe(s, e, false);
    if (info.path.empty() || !info.playable) return false;
    if (s.feed) {
      playlist.push_back(info.path);
    } else {
      // A folder plays on into the next episodes.
      for (int j = e; j < static_cast<int>(s.episodes.size()); ++j) {
        if (DirList::isMp3(s.episodes[j].guid)) {
          playlist.push_back(folderPath(s.folder) + "/" + s.episodes[j].guid);
        }
      }
    }
    EpState &st = s.state[s.episodes[e].guid];
    if (st.played) {  // listening again: from the start
      st.played = false;
      st.position = 0;
    }
    startMs = st.position * 1000;
    st.last = time(nullptr);
    key = s.key;
    state = s.state;
    ++gen;
  }
  writeState(key, state);
  audio::play(std::move(playlist), 0, startMs);
  trackedPath.clear();  // poll() picks the new track up
  return true;
}

void poll() {
  static uint32_t lastMs = 0;
  if (millis() - lastMs < 1000) return;
  lastMs = millis();
  const audio::Status a = audio::status();
  if (a.state == audio::State::Stopped || a.path.empty()) {
    trackedPath.clear();
    return;
  }
  if (a.path != trackedPath) follow(a.path);
  if (trackedKey.empty()) return;
  const uint32_t pos = a.elapsedMs / 1000, duration = a.durationMs / 1000;
  std::map<std::string, EpState> state;
  {
    std::lock_guard<std::mutex> lock(mu);
    Show *s = byKey(trackedKey);
    if (!s || !s->stateLoaded) return;
    EpState &st = s->state[trackedId];
    const bool nearEnd = duration > 120 && pos + kPlayedWithinS >= duration;
    if (nearEnd) {
      if (st.played) return;
      st.played = true;
      st.position = 0;
    } else {
      if (st.played) return;  // played before and playing again from Files: leave it
      const bool due = !savedOnce || pos >= lastSavedS + kSaveEveryS || pos < lastSavedS ||
                       (a.state == audio::State::Paused && pos != lastSavedS);
      if (!due) return;
      st.position = pos;
    }
    st.last = time(nullptr);
    lastSavedS = pos;
    savedOnce = true;
    state = s->state;
    ++gen;
  }
  writeState(trackedKey, state);
}

}  // namespace podcasts
