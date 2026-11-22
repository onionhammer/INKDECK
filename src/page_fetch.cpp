#include "page_fetch.h"

#include <Arduino.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <algorithm>
#include <memory>
#include <mutex>
#include <vector>

#include "features.h"
#include "netget.h"
#if INKDECK_AGENT
#include "agent_client.h"
#endif
#include "sdcard.h"
#include "wlan.h"

namespace pagefetch {
namespace {

constexpr const char *kDir = "/sdcard/.inkdeck/news/pages";
constexpr size_t kPageCap = 3 * 1024 * 1024;  // of a page downloaded
constexpr size_t kTextMax = 40 * 1024;        // of an article's text kept
constexpr uint32_t kConnectTimeoutMs = 20000;
constexpr uint32_t kAskTimeoutMs = 45000;  // inkharness: joining, connecting, its fetch
constexpr uint32_t kLingerMs = 20000;  // Wi-Fi up after a page, for the next link
constexpr int kKeepFiles = 200;
constexpr float kGuessBytes = 600 * 1024;  // a page of unknown length
// A browser's: some sites serve a stub, or nothing, to clients they don't know.
constexpr const char *kUserAgent =
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0 "
    "Safari/537.36";
constexpr const char *kMagic = "INKPAGE 1";
constexpr const char *kPicturesMagic = "INKPICS1";

std::mutex mu;
std::string wanted;   // the next page to fetch ("" for none)
uint16_t wantW = 0, wantH = 0;  // its pictures' largest size
std::string current;  // the page being or last fetched
Status state;
article::Article done;  // current's, when state is Done
bool working = false;

// --- The card -----------------------------------------------------------------------------------

uint32_t fnv1a(const std::string &s) {
  uint32_t h = 2166136261u;
  for (const char c : s) {
    h ^= static_cast<uint8_t>(c);
    h *= 16777619u;
  }
  return h;
}

std::string pathFor(const std::string &url, const char *ext = ".md") {
  char name[16];
  snprintf(name, sizeof name, "/%08lx", static_cast<unsigned long>(fnv1a(url)));
  return kDir + std::string(name) + ext;
}

// The pictures' file: kPicturesMagic, a count, then each picture's index (4 bytes), width and
// height (2 each, little-endian) and bits.
void putLe(std::string &s, uint32_t v, int bytes) {
  for (int i = 0; i < bytes; ++i) s += static_cast<char>((v >> (8 * i)) & 0xFF);
}

uint32_t get(const std::string &s, size_t &at, int bytes) {
  uint32_t v = 0;
  for (int i = 0; i < bytes && at < s.size(); ++i, ++at) {
    v |= static_cast<uint32_t>(static_cast<uint8_t>(s[at])) << (8 * i);
  }
  return v;
}

std::string packPictures(const std::vector<article::Picture> &pictures) {
  std::string all = kPicturesMagic;
  putLe(all, pictures.size(), 2);
  for (const article::Picture &p : pictures) {
    putLe(all, p.index, 4);
    putLe(all, p.width, 2);
    putLe(all, p.height, 2);
    all.append(reinterpret_cast<const char *>(p.bits.data()), p.bits.size());
  }
  return all;
}

std::vector<article::Picture> unpackPictures(const std::string &all) {
  std::vector<article::Picture> out;
  const size_t magic = strlen(kPicturesMagic);
  if (all.compare(0, magic, kPicturesMagic) != 0) return out;
  size_t at = magic;
  const uint32_t n = get(all, at, 2);
  for (uint32_t i = 0; i < n && at < all.size(); ++i) {
    article::Picture p;
    p.index = get(all, at, 4);
    p.width = static_cast<uint16_t>(get(all, at, 2));
    p.height = static_cast<uint16_t>(get(all, at, 2));
    const size_t len = static_cast<size_t>((p.width + 7) / 8) * p.height;
    if (at + len > all.size()) break;
    p.bits.assign(all.begin() + at, all.begin() + at + len);
    at += len;
    out.push_back(std::move(p));
  }
  return out;
}

std::string oneLine(std::string s) {
  std::replace(s.begin(), s.end(), '\n', ' ');
  return s;
}

bool readLine(FILE *f, std::string &out) {
  out.clear();
  int c;
  while ((c = fgetc(f)) != EOF && c != '\n') out += static_cast<char>(c);
  return c != EOF || !out.empty();
}

// Keeps the newest kKeepFiles pages (by their .md), with their pictures.
void prune() {
  DIR *d = opendir(kDir);
  if (!d) return;
  std::vector<std::pair<time_t, std::string>> files;
  while (dirent *e = readdir(d)) {
    const size_t len = strlen(e->d_name);
    if (e->d_name[0] == '.' || len < 3 || strcmp(e->d_name + len - 3, ".md") != 0) continue;
    const std::string path = std::string(kDir) + "/" + e->d_name;
    struct stat st;
    if (stat(path.c_str(), &st) == 0) files.push_back({st.st_mtime, path});
  }
  closedir(d);
  if (static_cast<int>(files.size()) <= kKeepFiles) return;
  std::sort(files.begin(), files.end());
  for (size_t i = 0; i + kKeepFiles < files.size(); ++i) {
    const std::string &md = files[i].second;
    remove(md.c_str());
    remove((md.substr(0, md.size() - 3) + ".pic").c_str());
  }
}

void save(const std::string &url, const article::Article &a) {
  if (!sdcard::mounted()) return;
  mkdir("/sdcard/.inkdeck", 0777);
  mkdir("/sdcard/.inkdeck/news", 0777);
  mkdir(kDir, 0777);
  const std::string path = pathFor(url);
  const std::string tmp = path + ".tmp";
  FILE *f = fopen(tmp.c_str(), "wb");
  if (!f) return;
  // Through a string: never fwrite() a sector or more straight from flash (AGENTS.md gotcha 7).
  std::string all = std::string(kMagic) + "\n" + oneLine(url) + "\n" + oneLine(a.title) + "\n" +
                    oneLine(a.byline) + "\n" + oneLine(a.site) + "\n" +
                    std::to_string(static_cast<long long>(a.published)) + "\n\n" + a.markdown;
  const bool ok = fwrite(all.data(), 1, all.size(), f) == all.size();
  fclose(f);
  remove(path.c_str());
  if (!ok || rename(tmp.c_str(), path.c_str()) != 0) {
    remove(tmp.c_str());
    return;
  }
  const std::string pics = pathFor(url, ".pic");
  remove(pics.c_str());
  if (!a.pictures.empty()) {
    // In PSRAM, then out (gotcha 7 is about flash, but a big write wants a RAM source anyway).
    const std::string packed = packPictures(a.pictures);
    if (FILE *pf = fopen(pics.c_str(), "wb")) {
      const bool wrote = fwrite(packed.data(), 1, packed.size(), pf) == packed.size();
      fclose(pf);
      if (!wrote) remove(pics.c_str());
    }
  }
  prune();
}

// --- The task -----------------------------------------------------------------------------------

// url's progress, unless another page has taken over since.
void setState(const std::string &url, const Status &s) {
  std::lock_guard<std::mutex> lock(mu);
  if (url != current) return;
  const uint32_t started = state.startedMs;
  state = s;
  state.startedMs = started;
}

// Abandoned: another page asked for, or cancel().
bool abandoned(const std::string &url) {
  std::lock_guard<std::mutex> lock(mu);
  return wanted != url;
}

bool abandonedFor(void *url) { return abandoned(*static_cast<const std::string *>(url)); }

// The article, done: kept on the card and handed over.
void finish(const std::string &url, article::Article a) {
  save(url, a);
  std::lock_guard<std::mutex> lock(mu);
  if (url != current) return;
  done = std::move(a);
  state.phase = Phase::Done;
}

void fetchOne(const std::string &url, bool &connected) {
  Status s;
#if INKDECK_AGENT
  if (agent::configured()) {
    s.phase = Phase::Asking;
    setState(url, s);
    agent::Page page;
    std::string error;
    bool reached = false;
    const uint32_t t0 = millis();
    uint16_t w, h;
    {
      std::lock_guard<std::mutex> lock(mu);
      w = wantW;
      h = wantH;
    }
    const bool ok = agent::readPage(url, w, h, kAskTimeoutMs, page, error, reached, abandonedFor,
                                    const_cast<std::string *>(&url));
    log_i("page: inkharness %s in %u ms: %s", ok ? "read it" : "didn't",
          static_cast<unsigned>(millis() - t0), ok ? url.c_str() : error.c_str());
    if (abandoned(url)) return;
    if (ok) {
      article::Article a;
      a.title = page.title;
      a.byline = page.byline;
      a.site = page.site;
      a.published = page.published;
      a.markdown = std::move(page.markdown);
      for (agent::PagePicture &p : page.pictures) {
        article::Picture pic;
        pic.index = p.index;
        pic.width = p.width;
        pic.height = p.height;
        pic.bits = std::move(p.bits);
        a.pictures.push_back(std::move(pic));
      }
      finish(url, std::move(a));
      return;
    }
    if (reached) {  // the daemon had a go: the site would say the same to the board
      s.phase = Phase::Failed;
      s.error = error;
      setState(url, s);
      return;
    }
    s = Status{};  // not there to ask: read it here
  }
#endif
  if (!connected) {
    s.phase = Phase::Joining;
    setState(url, s);
    std::string error;
    if (!wlan::connect(kConnectTimeoutMs, error)) {
      s.phase = Phase::Failed;
      s.error = "WI-FI: " + error;
      setState(url, s);
      return;
    }
    connected = true;
  }
  if (abandoned(url)) return;
  s.phase = Phase::Loading;
  setState(url, s);

  net::Get get;
  get.setUserAgent(kUserAgent);
  std::string error;
  if (!get.open(url, error)) {
    if (error.rfind("SERVER SAID ", 0) == 0) error = "HTTP " + error.substr(12);
    s.phase = Phase::Failed;
    s.error = error;
    setState(url, s);
    return;
  }
  const std::string where = get.url().empty() ? url : get.url();
  s.total = get.length();
  std::string page;
  page.reserve(s.total > 0 ? std::min<int64_t>(s.total, kPageCap) : 256 * 1024);
  std::unique_ptr<char[]> buf(new char[8192]);
  uint32_t shownMs = 0;
  while (page.size() < kPageCap) {
    const int n = get.read(buf.get(), 8192);
    if (n < 0) {
      if (page.empty()) {
        s.phase = Phase::Failed;
        s.error = "CONNECTION LOST";
        setState(url, s);
        return;
      }
      break;  // read what came
    }
    if (n == 0) break;
    page.append(buf.get(), n);
    if (abandoned(url)) return;
    if (millis() - shownMs > 250) {
      s.bytes = static_cast<uint32_t>(page.size());
      setState(url, s);
      shownMs = millis();
    }
  }
  get.close();
  s.bytes = static_cast<uint32_t>(page.size());
  s.phase = Phase::Reading;
  setState(url, s);

  article::Article a;
  const uint32_t t0 = millis();
  const bool ok = article::extract(page.data(), page.size(), where, kTextMax, a);
  log_i("page: %u bytes read in %u ms, %u bytes of text", static_cast<unsigned>(page.size()),
        static_cast<unsigned>(millis() - t0), static_cast<unsigned>(a.markdown.size()));
  page.clear();
  page.shrink_to_fit();
  if (!ok) {
    s.phase = Phase::Failed;
    s.error = "NO ARTICLE ON THE PAGE";
    setState(url, s);
    return;
  }
  finish(url, std::move(a));
}

void worker(void *) {
  bool connected = false;
  uint32_t idleSince = millis();
  for (;;) {
    std::string url;
    {
      std::lock_guard<std::mutex> lock(mu);
      if (!wanted.empty() && wanted == current && state.phase == Phase::Idle) url = wanted;
      if (url.empty() && (!connected || millis() - idleSince > kLingerMs)) {
        working = false;
        break;
      }
    }
    if (url.empty()) {
      delay(100);
      continue;
    }
    fetchOne(url, connected);
    {
      std::lock_guard<std::mutex> lock(mu);
      if (wanted == url) wanted.clear();  // done or failed: not again by itself
    }
    idleSince = millis();
  }
  if (connected) wlan::disconnect();
  vTaskDelete(nullptr);
}

}  // namespace

float Status::progress() const {
  switch (phase) {
    case Phase::Asking: return 0.3f;
    case Phase::Joining: return 0.05f;
    case Phase::Loading: {
      const float total = this->total > 0 ? static_cast<float>(this->total) : kGuessBytes;
      float f = bytes / total;
      if (this->total <= 0) f = f / (1 + f);  // creeps toward the end, never past it
      return 0.1f + 0.8f * std::min(f, 1.0f);
    }
    case Phase::Reading: return 0.92f;
    case Phase::Done: return 1;
    default: return 0;
  }
}

bool cached(const std::string &url, article::Article &out) {
  if (url.empty() || !sdcard::mounted()) return false;
  FILE *f = fopen(pathFor(url).c_str(), "rb");
  if (!f) return false;
  std::string line, stored, when;
  bool ok = readLine(f, line) && line == kMagic && readLine(f, stored) && stored == oneLine(url);
  article::Article a;
  ok = ok && readLine(f, a.title) && readLine(f, a.byline) && readLine(f, a.site) &&
       readLine(f, when) && readLine(f, line);
  if (ok) {
    a.published = static_cast<time_t>(atoll(when.c_str()));
    char buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) a.markdown.append(buf, n);
  }
  fclose(f);
  if (!ok) return false;
  if (FILE *pf = fopen(pathFor(url, ".pic").c_str(), "rb")) {
    std::string packed;
    char buf[2048];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, pf)) > 0) packed.append(buf, n);
    fclose(pf);
    a.pictures = unpackPictures(packed);
  }
  out = std::move(a);
  return true;
}

void fetch(const std::string &url, uint16_t pictureW, uint16_t pictureH) {
  if (url.empty()) return;
  std::lock_guard<std::mutex> lock(mu);
  wantW = pictureW;
  wantH = pictureH;
  if (url == current && (state.phase == Phase::Done || wanted == url)) return;
  // The worker starts it afresh (and drops the page under way, if any).
  wanted = current = url;
  done = article::Article{};
  state = Status{};
  state.startedMs = millis();
  if (working) return;
  working = true;
  // TLS and the reading need room; the page itself is in PSRAM.
  if (xTaskCreate(worker, "page", 12288, nullptr, 1, nullptr) != pdPASS) {
    working = false;
    wanted.clear();
    state.phase = Phase::Failed;
    state.error = "NO MEMORY FOR THE TASK";
  }
}

Status status(const std::string &url) {
  std::lock_guard<std::mutex> lock(mu);
  if (url != current) return {};
  Status s = state;
  // Waiting for the worker to pick it up reads as joining.
  if (s.phase == Phase::Idle && wanted == url) s.phase = Phase::Joining;
  return s;
}

bool result(const std::string &url, article::Article &out) {
  std::lock_guard<std::mutex> lock(mu);
  if (url != current || state.phase != Phase::Done) return false;
  out = done;
  return true;
}

void cancel() {
  std::lock_guard<std::mutex> lock(mu);
  wanted.clear();
  if (state.phase != Phase::Done) state = Status{};  // reads as never asked for
}

bool busy() {
  std::lock_guard<std::mutex> lock(mu);
  return working;
}

}  // namespace pagefetch
