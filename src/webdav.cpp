#include "webdav.h"

#include <Arduino.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <SD_MMC.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <esp_rom_md5.h>
#include <lwip/sockets.h>
#include <strings.h>
#include <time.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

#include "ff.h"
#include "input.h"
#include "ota.h"  // kHostname: the board is inkdeck.local for both
#include "sdcard.h"
#include "wlan.h"

namespace webdav {
namespace {

constexpr uint32_t kConnectTimeoutMs = 20000;
constexpr uint32_t kIdleMs = kIdleMinutes * 60 * 1000;
constexpr uint32_t kKeepAliveMs = 30000;  // a connection with no new request this long closes
constexpr uint32_t kBodyTimeoutMs = 30000;  // silence in the middle of a request body
constexpr size_t kMaxHead = 8192;
constexpr size_t kMaxSmallBody = 64 * 1024;  // PROPFIND, PROPPATCH and LOCK bodies
constexpr size_t kMaxDrain = 1 << 20;        // more unread body than this: close instead
constexpr size_t kIoBuf = 32 * 1024;         // file data, per connection (PSRAM)
constexpr size_t kChunkOut = 8192;           // a PROPFIND reply goes out in chunks this big
constexpr uint32_t kStack = 7168;
constexpr const char *kRealm = "INKDECK";
constexpr const char *kAllow =
    "OPTIONS, GET, HEAD, PUT, DELETE, PROPFIND, PROPPATCH, MKCOL, COPY, MOVE, LOCK, UNLOCK";

std::atomic<Phase> phase{Phase::Idle};
std::atomic<bool> running{false};        // the listener task exists
std::atomic<bool> stopRequested{false};  // stop() asked
std::atomic<bool> closing{false};        // the listener is done: connections end
std::atomic<int> clients{0};
std::atomic<uint64_t> readTotal{0};
std::atomic<uint64_t> writtenTotal{0};
std::atomic<uint32_t> lastReadMs{0};
std::atomic<uint32_t> lastWriteMs{0};
std::atomic<bool> anyRead{false};
std::atomic<bool> anyWrite{false};
std::atomic<uint32_t> lastUseMs{0};  // the last file read or change (the idle stop)
std::atomic<uint32_t> badSignIns{0};
std::atomic<bool> settingsTouched{false};
std::atomic<bool> podcastsTouched{false};

std::mutex textMutex;
std::string resultText;
std::string ipText;
std::string lastChangeText;

// Set by start() before the listener runs; read-only while it does.
std::string ha1;     // MD5(user:realm:password)
std::string nonce;   // this session's Digest nonce
std::string opaque;
char drive[3] = "0:";  // FatFs's drive for the card

std::string cachedPassword;  // UI task

void setText(std::string &field, std::string text) {
  std::lock_guard<std::mutex> lock(textMutex);
  field = std::move(text);
}

std::string hex(const uint8_t *d, size_t n) {
  static const char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(n * 2);
  for (size_t i = 0; i < n; ++i) {
    out += kHex[d[i] >> 4];
    out += kHex[d[i] & 15];
  }
  return out;
}

std::string md5hex(const std::string &s) {
  md5_context_t ctx;
  esp_rom_md5_init(&ctx);
  esp_rom_md5_update(&ctx, s.data(), s.size());
  uint8_t digest[16];
  esp_rom_md5_final(digest, &ctx);
  return hex(digest, sizeof digest);
}

std::string randomHex(size_t bytes) {
  std::vector<uint8_t> b(bytes);
  esp_fill_random(b.data(), b.size());
  return hex(b.data(), b.size());
}

// ---- Paths -------------------------------------------------------------------------------------

bool unreserved(unsigned char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
         c == '_' || c == '.' || c == '~';
}

int hexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// A request URI (or a Destination, which is a whole URL) as a clean path on the card: "/" or
// "/Books/Dune.epub", decoded, no trailing slash. False for "..", a bad escape or a control
// character.
bool decodePath(const std::string &uri, std::string &out) {
  std::string p = uri;
  const size_t scheme = p.find("://");
  if (scheme != std::string::npos && scheme < p.find('/')) {
    const size_t slash = p.find('/', scheme + 3);
    p = slash == std::string::npos ? "/" : p.substr(slash);
  }
  const size_t query = p.find('?');
  if (query != std::string::npos) p.resize(query);
  std::string decoded;
  for (size_t i = 0; i < p.size(); ++i) {
    if (p[i] != '%') {
      decoded += p[i];
      continue;
    }
    if (i + 2 >= p.size()) return false;
    const int hi = hexDigit(p[i + 1]), lo = hexDigit(p[i + 2]);
    if (hi < 0 || lo < 0) return false;
    decoded += static_cast<char>(hi << 4 | lo);
    i += 2;
  }
  out.clear();
  size_t i = 0;
  while (i <= decoded.size()) {
    size_t j = decoded.find('/', i);
    if (j == std::string::npos) j = decoded.size();
    const std::string seg = decoded.substr(i, j - i);
    if (seg == "..") return false;
    if (!seg.empty() && seg != ".") out += "/" + seg;
    i = j + 1;
  }
  if (out.empty()) out = "/";
  for (const char c : out) {
    if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) return false;
  }
  return true;
}

std::string encodePath(const std::string &path) {
  static const char kHex[] = "0123456789ABCDEF";
  std::string out;
  for (const char ch : path) {
    const unsigned char c = static_cast<unsigned char>(ch);
    if (unreserved(c) || c == '/') {
      out += ch;
    } else {
      out += '%';
      out += kHex[c >> 4];
      out += kHex[c & 15];
    }
  }
  return out;
}

std::string fatPath(const std::string &path) { return std::string(drive) + path; }
std::string parentOf(const std::string &path) {
  const size_t slash = path.rfind('/');
  return slash == 0 ? "/" : path.substr(0, slash);
}
std::string nameOf(const std::string &path) { return path.substr(path.rfind('/') + 1); }
std::string childOf(const std::string &dir, const char *name) {
  return (dir == "/" ? "" : dir) + "/" + name;
}

// FAT names are case-insensitive (for ASCII, which is all FatFs folds here).
bool samePath(const std::string &a, const std::string &b) { return strcasecmp(a.c_str(), b.c_str()) == 0; }
bool inside(const std::string &path, const std::string &dir) {
  return path.size() > dir.size() && path[dir.size()] == '/' &&
         strncasecmp(path.c_str(), dir.c_str(), dir.size()) == 0;
}

// Finder's bookkeeping: extended attributes in ._ files, window state in .DS_Store.
bool finderFile(const std::string &path) {
  const std::string name = nameOf(path);
  return name.compare(0, 2, "._") == 0 || name == ".DS_Store";
}

std::string xmlEscape(const std::string &s) {
  std::string out;
  for (const char c : s) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      default: out += c;
    }
  }
  return out;
}

// ---- The card ----------------------------------------------------------------------------------

struct Entry {
  bool exists = false;
  bool dir = false;
  uint64_t size = 0;
  time_t mtime = 0;
};

// FAT keeps local time.
time_t fatTime(WORD date, WORD time) {
  tm t{};
  t.tm_year = ((date >> 9) & 0x7F) + 80;
  t.tm_mon = std::max(1, (date >> 5) & 0xF) - 1;
  t.tm_mday = std::max(1, date & 0x1F);
  t.tm_hour = time >> 11;
  t.tm_min = (time >> 5) & 0x3F;
  t.tm_sec = (time & 0x1F) * 2;
  t.tm_isdst = -1;
  return mktime(&t);
}

Entry entryOf(const FILINFO &fi) {
  Entry e;
  e.exists = true;
  e.dir = (fi.fattrib & AM_DIR) != 0;
  e.size = fi.fsize;
  e.mtime = fatTime(fi.fdate, fi.ftime);
  return e;
}

Entry lookup(const std::string &path) {
  Entry e;
  if (path == "/") {
    e.exists = e.dir = true;
    e.mtime = time(nullptr);
    return e;
  }
  FILINFO fi;
  if (f_stat(fatPath(path).c_str(), &fi) == FR_OK) e = entryOf(fi);
  return e;
}

bool space(uint64_t &total, uint64_t &free) {
  FATFS *fs = nullptr;
  DWORD freeClusters = 0;
  if (f_getfree(drive, &freeClusters, &fs) != FR_OK || !fs) return false;
#if FF_MAX_SS != FF_MIN_SS
  const uint64_t cluster = static_cast<uint64_t>(fs->csize) * fs->ssize;
#else
  const uint64_t cluster = static_cast<uint64_t>(fs->csize) * FF_MAX_SS;
#endif
  total = static_cast<uint64_t>(fs->n_fatent - 2) * cluster;
  free = static_cast<uint64_t>(freeClusters) * cluster;
  return true;
}

// The names in a folder, read before anything in it changes.
std::vector<std::pair<std::string, bool>> listNames(const std::string &dir) {
  std::vector<std::pair<std::string, bool>> names;
  FF_DIR *d = new FF_DIR;
  FILINFO *fi = new FILINFO;
  if (f_opendir(d, fatPath(dir).c_str()) == FR_OK) {
    while (f_readdir(d, fi) == FR_OK && fi->fname[0]) {
      names.emplace_back(fi->fname, (fi->fattrib & AM_DIR) != 0);
    }
    f_closedir(d);
  }
  delete fi;
  delete d;
  return names;
}

// A file, or a folder and everything in it.
FRESULT removeTree(const std::string &path, bool dir) {
  if (dir) {
    for (const auto &n : listNames(path)) {
      const FRESULT fr = removeTree(childOf(path, n.first.c_str()), n.second);
      if (fr != FR_OK) return fr;
    }
  }
  return f_unlink(fatPath(path).c_str());  // a file, or a folder once it's empty
}

FRESULT copyFile(const std::string &from, const std::string &to, uint8_t *buf) {
  FIL *in = new FIL;
  FIL *out = new FIL;
  FRESULT fr = f_open(in, fatPath(from).c_str(), FA_READ);
  if (fr == FR_OK) {
    fr = f_open(out, fatPath(to).c_str(), FA_CREATE_ALWAYS | FA_WRITE);
    if (fr == FR_OK) {
      for (;;) {
        UINT got = 0, put = 0;
        fr = f_read(in, buf, kIoBuf, &got);
        if (fr != FR_OK || got == 0) break;
        fr = f_write(out, buf, got, &put);
        if (fr == FR_OK && put != got) fr = FR_DENIED;  // the card is full
        if (fr != FR_OK) break;
        lastUseMs = millis();
      }
      const FRESULT closed = f_close(out);
      if (fr == FR_OK) fr = closed;
      if (fr != FR_OK) f_unlink(fatPath(to).c_str());
    }
    f_close(in);
  }
  delete out;
  delete in;
  return fr;
}

FRESULT copyTree(const std::string &from, const std::string &to, bool dir, bool deep,
                 uint8_t *buf) {
  if (!dir) return copyFile(from, to, buf);
  FRESULT fr = f_mkdir(fatPath(to).c_str());
  if (fr != FR_OK || !deep) return fr;
  for (const auto &n : listNames(from)) {
    fr = copyTree(childOf(from, n.first.c_str()), childOf(to, n.first.c_str()), n.second, true, buf);
    if (fr != FR_OK) return fr;
  }
  return FR_OK;
}

// What the firmware has to reread, and what the screen says changed.
void touched(const std::string &path) {
  if (samePath(path, "/inkdeck.ini")) settingsTouched = true;
  if (samePath(path, "/Podcasts") || inside(path, "/Podcasts")) podcastsTouched = true;
  lastUseMs = millis();
}

void changed(const char *verb, const std::string &path) {
  touched(path);
  if (finderFile(path)) return;
  setText(lastChangeText, std::string(verb) + " " + path.substr(1));
}

std::string contentType(const std::string &path) {
  static const std::pair<const char *, const char *> kTypes[] = {
      {".txt", "text/plain; charset=utf-8"}, {".ini", "text/plain; charset=utf-8"},
      {".md", "text/markdown; charset=utf-8"}, {".html", "text/html; charset=utf-8"},
      {".htm", "text/html; charset=utf-8"},  {".xml", "application/xml"},
      {".json", "application/json"},         {".epub", "application/epub+zip"},
      {".pdf", "application/pdf"},           {".mp3", "audio/mpeg"},
      {".wav", "audio/wav"},                 {".m4a", "audio/mp4"},
      {".png", "image/png"},                 {".jpg", "image/jpeg"},
      {".jpeg", "image/jpeg"},               {".gif", "image/gif"},
      {".bmp", "image/bmp"},                 {".csv", "text/csv"},
  };
  const size_t dot = path.rfind('.');
  if (dot != std::string::npos && dot > path.rfind('/')) {
    const char *ext = path.c_str() + dot;
    for (const auto &t : kTypes) {
      if (strcasecmp(ext, t.first) == 0) return t.second;
    }
  }
  return "application/octet-stream";
}

std::string httpDate(time_t t) {
  tm g;
  gmtime_r(&t, &g);
  char buf[40];
  strftime(buf, sizeof buf, "%a, %d %b %Y %H:%M:%S GMT", &g);
  return buf;
}

std::string isoDate(time_t t) {
  tm g;
  gmtime_r(&t, &g);
  char buf[24];
  strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &g);
  return buf;
}

std::string etag(const Entry &e) {
  char buf[40];
  snprintf(buf, sizeof buf, "\"%llx-%llx\"", static_cast<unsigned long long>(e.size),
           static_cast<unsigned long long>(e.mtime));
  return buf;
}

// ---- XML ---------------------------------------------------------------------------------------

// The properties a PROPPATCH names, as (namespace, local name). Prefixes come from the body's
// xmlns declarations; clients don't rebind them.
std::vector<std::pair<std::string, std::string>> patchedProps(const std::string &xml) {
  std::map<std::string, std::string> ns;
  for (size_t at = xml.find("xmlns"); at != std::string::npos; at = xml.find("xmlns", at + 5)) {
    size_t p = at + 5;
    std::string prefix;
    if (p < xml.size() && xml[p] == ':') {
      const size_t eq = xml.find('=', p);
      if (eq == std::string::npos) break;
      prefix = xml.substr(p + 1, eq - p - 1);
      p = eq;
    }
    if (p >= xml.size() || xml[p] != '=' || p + 1 >= xml.size()) continue;
    const char quote = xml[p + 1];
    const size_t end = xml.find(quote, p + 2);
    if (end == std::string::npos) break;
    ns[prefix] = xml.substr(p + 2, end - p - 2);
  }
  std::vector<std::pair<std::string, std::string>> props;
  int depth = -1;  // inside a <prop>: 0 at its children's level
  for (size_t lt = xml.find('<'); lt != std::string::npos; lt = xml.find('<', lt + 1)) {
    const size_t gt = xml.find('>', lt);
    if (gt == std::string::npos) break;
    std::string tag = xml.substr(lt + 1, gt - lt - 1);
    if (tag.empty() || tag[0] == '?' || tag[0] == '!') continue;
    const bool closing = tag[0] == '/';
    const bool selfClosing = !tag.empty() && tag.back() == '/';
    if (closing) tag.erase(0, 1);
    const std::string qname = tag.substr(0, tag.find_first_of(" \t\r\n/"));
    const size_t colon = qname.find(':');
    const std::string prefix = colon == std::string::npos ? "" : qname.substr(0, colon);
    const std::string local = colon == std::string::npos ? qname : qname.substr(colon + 1);
    if (depth < 0) {
      if (!closing && !selfClosing && local == "prop") depth = 0;
      continue;
    }
    if (closing) {
      if (depth == 0) {
        depth = -1;  // </prop>
      } else {
        --depth;
      }
      continue;
    }
    if (depth == 0) props.emplace_back(ns.count(prefix) ? ns[prefix] : "", local);
    if (!selfClosing) ++depth;
  }
  return props;
}

// ---- A connection ------------------------------------------------------------------------------

struct Request {
  std::string method;
  std::string uri;
  std::string path;  // decoded; "" if the URI was bad
  std::vector<std::pair<std::string, std::string>> headers;  // names in lower case
  int64_t contentLength = -1;
  bool chunked = false;
  bool keepAlive = true;
  bool expectContinue = false;

  const std::string *header(const char *name) const {
    for (const auto &h : headers) {
      if (h.first == name) return &h.second;
    }
    return nullptr;
  }
};

const char *reason(int code) {
  switch (code) {
    case 100: return "Continue";
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 206: return "Partial Content";
    case 207: return "Multi-Status";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 412: return "Precondition Failed";
    case 413: return "Payload Too Large";
    case 415: return "Unsupported Media Type";
    case 416: return "Range Not Satisfiable";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 503: return "Service Unavailable";
    case 507: return "Insufficient Storage";
  }
  return "Error";
}

// A FatFs result as an HTTP status. creating: running out of room is 507, not 403.
int statusOf(FRESULT fr, bool creating = false) {
  switch (fr) {
    case FR_OK: return 200;
    case FR_NO_FILE: return 404;
    case FR_NO_PATH: return 409;
    case FR_EXIST: return 405;
    case FR_INVALID_NAME: return 400;
    case FR_DENIED: return creating ? 507 : 403;
    case FR_WRITE_PROTECTED: return 403;
    case FR_NOT_READY:
    case FR_TIMEOUT: return 503;
    default: return 500;
  }
}

class Connection {
 public:
  explicit Connection(int fd) : fd_(fd) {}
  ~Connection() {
    if (buf_) heap_caps_free(buf_);
  }

  void serve();

 private:
  // Receiving.
  int recvSome(void *dst, size_t want, uint32_t timeoutMs);
  bool readHead(Request &r);
  bool readLine(std::string &line);
  int take(uint8_t *dst, size_t want);
  int readBody(uint8_t *dst, size_t max);
  int fillBody(uint8_t *dst, size_t max);
  bool readSmallBody(std::string &out);
  bool hasBody() const { return chunked_ ? !chunkDone_ : bodyLeft_ > 0; }
  void sendContinue();

  // Sending.
  bool sendAll(const void *data, size_t len);
  void sendHead(int code, const std::string &headers, int64_t length, bool chunked = false);
  void respond(int code, const std::string &headers = "", const std::string &body = "",
               bool headOnly = false);
  bool sendChunk(const std::string &data);

  // Methods.
  void handle(Request &r);
  void challenge(bool stale);
  void options();
  void propfind(const Request &r);
  std::string responseXml(const std::string &path, const Entry &e, bool quota);
  void get(const Request &r, bool headOnly);
  void listing(const Request &r, bool headOnly);
  void put(const Request &r);
  void remove(const Request &r);
  void mkcol(const Request &r);
  void copyMove(const Request &r, bool move);
  void lock(const Request &r);
  void proppatch(const Request &r);

  int fd_;
  uint8_t *buf_ = nullptr;
  std::string in_;  // received, not yet used
  bool keepAlive_ = true;
  bool failed_ = false;  // the socket broke (or the client broke the protocol): close
  // The current request's body.
  bool chunked_ = false;
  int64_t bodyLeft_ = 0;   // with a Content-Length
  int64_t chunkLeft_ = 0;  // chunked: left in the current chunk
  bool chunkDone_ = true;
  bool continuePending_ = false;  // Expect: 100-continue, and no 100 sent yet
};

int Connection::recvSome(void *dst, size_t want, uint32_t timeoutMs) {
  const uint32_t t0 = millis();
  for (;;) {
    const int n = recv(fd_, dst, want, 0);
    if (n >= 0) return n;
    if (errno != EAGAIN && errno != EWOULDBLOCK) return -1;
    if (closing || millis() - t0 >= timeoutMs) return -1;
  }
}

bool Connection::readHead(Request &r) {
  const uint32_t t0 = millis();
  size_t end;
  for (;;) {
    while (in_.compare(0, 2, "\r\n") == 0) in_.erase(0, 2);  // stray line ends between requests
    end = in_.find("\r\n\r\n");
    if (end != std::string::npos) break;
    if (in_.size() > kMaxHead) {
      keepAlive_ = false;
      respond(431);
      return false;
    }
    char tmp[512];
    const uint32_t waited = millis() - t0;
    if (waited >= kKeepAliveMs) return false;
    const int n = recvSome(tmp, sizeof tmp, kKeepAliveMs - waited);
    if (n <= 0) return false;
    in_.append(tmp, n);
  }
  const std::string head = in_.substr(0, end);
  in_.erase(0, end + 4);

  size_t lineEnd = head.find("\r\n");
  const std::string line = head.substr(0, lineEnd);
  const size_t sp1 = line.find(' ');
  const size_t sp2 = line.find(' ', sp1 + 1);
  if (sp1 == std::string::npos || sp2 == std::string::npos) {
    keepAlive_ = false;
    respond(400);
    return false;
  }
  r.method = line.substr(0, sp1);
  r.uri = line.substr(sp1 + 1, sp2 - sp1 - 1);
  const bool http10 = line.compare(sp2 + 1, std::string::npos, "HTTP/1.0") == 0;
  if (!decodePath(r.uri, r.path)) r.path.clear();

  while (lineEnd != std::string::npos) {
    const size_t start = lineEnd + 2;
    lineEnd = head.find("\r\n", start);
    const std::string h = head.substr(start, lineEnd == std::string::npos ? std::string::npos
                                                                         : lineEnd - start);
    const size_t colon = h.find(':');
    if (colon == std::string::npos) continue;
    std::string name = h.substr(0, colon);
    for (char &c : name) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    const size_t v0 = h.find_first_not_of(" \t", colon + 1);
    const size_t v1 = h.find_last_not_of(" \t");
    r.headers.emplace_back(name, v0 == std::string::npos ? "" : h.substr(v0, v1 - v0 + 1));
  }
  if (const std::string *cl = r.header("content-length")) r.contentLength = atoll(cl->c_str());
  if (const std::string *te = r.header("transfer-encoding")) {
    r.chunked = strcasestr(te->c_str(), "chunked") != nullptr;
  }
  const std::string *conn = r.header("connection");
  r.keepAlive = http10 ? conn && strcasestr(conn->c_str(), "keep-alive")
                       : !(conn && strcasestr(conn->c_str(), "close"));
  if (const std::string *ex = r.header("expect")) {
    r.expectContinue = strcasecmp(ex->c_str(), "100-continue") == 0;
  }
  return true;
}

bool Connection::readLine(std::string &line) {
  for (;;) {
    const size_t e = in_.find("\r\n");
    if (e != std::string::npos) {
      line = in_.substr(0, e);
      in_.erase(0, e + 2);
      return true;
    }
    if (in_.size() > kMaxHead) return false;
    char tmp[256];
    const int n = recvSome(tmp, sizeof tmp, kBodyTimeoutMs);
    if (n <= 0) return false;
    in_.append(tmp, n);
  }
}

int Connection::take(uint8_t *dst, size_t want) {
  if (!in_.empty()) {
    const size_t n = std::min(want, in_.size());
    memcpy(dst, in_.data(), n);
    in_.erase(0, n);
    return static_cast<int>(n);
  }
  const int n = recvSome(dst, want, kBodyTimeoutMs);
  return n > 0 ? n : -1;
}

void Connection::sendContinue() {
  if (!continuePending_) return;
  continuePending_ = false;
  static const char kContinue[] = "HTTP/1.1 100 Continue\r\n\r\n";
  sendAll(kContinue, sizeof kContinue - 1);
}

// Up to max bytes of the body (Content-Length or chunked): 0 at its end, -1 if it broke.
int Connection::readBody(uint8_t *dst, size_t max) {
  if (failed_) return -1;
  sendContinue();
  if (!chunked_) {
    if (bodyLeft_ <= 0) return 0;
    const int n = take(dst, static_cast<size_t>(std::min<int64_t>(max, bodyLeft_)));
    if (n < 0) {
      failed_ = true;
      return -1;
    }
    bodyLeft_ -= n;
    return n;
  }
  if (chunkDone_) return 0;
  std::string line;
  if (chunkLeft_ == 0) {
    char *end = nullptr;
    if (!readLine(line)) {
      failed_ = true;
      return -1;
    }
    chunkLeft_ = strtoll(line.c_str(), &end, 16);
    if (end == line.c_str() || chunkLeft_ < 0) {
      failed_ = true;
      return -1;
    }
    if (chunkLeft_ == 0) {
      do {  // trailers, up to the empty line
        if (!readLine(line)) {
          failed_ = true;
          return -1;
        }
      } while (!line.empty());
      chunkDone_ = true;
      return 0;
    }
  }
  const int n = take(dst, static_cast<size_t>(std::min<int64_t>(max, chunkLeft_)));
  if (n < 0) {
    failed_ = true;
    return -1;
  }
  chunkLeft_ -= n;
  if (chunkLeft_ == 0 && (!readLine(line) || !line.empty())) {
    failed_ = true;
    return -1;
  }
  return n;
}

// Fills dst as far as the body goes (whole buffers write to the card fastest).
int Connection::fillBody(uint8_t *dst, size_t max) {
  size_t got = 0;
  while (got < max) {
    const int n = readBody(dst + got, max - got);
    if (n < 0) return -1;
    if (n == 0) break;
    got += n;
  }
  return static_cast<int>(got);
}

bool Connection::readSmallBody(std::string &out) {
  out.clear();
  uint8_t tmp[512];
  for (;;) {
    const int n = readBody(tmp, sizeof tmp);
    if (n < 0) return false;
    if (n == 0) return true;
    if (out.size() + n > kMaxSmallBody) {
      failed_ = true;
      return false;
    }
    out.append(reinterpret_cast<const char *>(tmp), n);
  }
}

bool Connection::sendAll(const void *data, size_t len) {
  if (failed_) return false;
  const char *p = static_cast<const char *>(data);
  uint32_t progressMs = millis();
  while (len) {
    const int n = send(fd_, p, len, 0);
    if (n > 0) {
      p += n;
      len -= n;
      progressMs = millis();
      continue;
    }
    // SO_SNDTIMEO wakes it each second: give up on a client that stopped reading.
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && !closing &&
        millis() - progressMs < kBodyTimeoutMs) {
      continue;
    }
    failed_ = true;
    return false;
  }
  return true;
}

void Connection::sendHead(int code, const std::string &headers, int64_t length, bool chunked) {
  std::string out = "HTTP/1.1 " + std::to_string(code) + " " + reason(code) + "\r\n";
  out += "Server: INKDECK\r\n";
  out += headers;
  if (chunked) {
    out += "Transfer-Encoding: chunked\r\n";
  } else {
    out += "Content-Length: " + std::to_string(length) + "\r\n";
  }
  if (!keepAlive_) out += "Connection: close\r\n";
  out += "\r\n";
  sendAll(out.data(), out.size());
}

void Connection::respond(int code, const std::string &headers, const std::string &body,
                         bool headOnly) {
  // A request body we won't read: with no 100 sent, the client may or may not send it, so the
  // connection can't carry on.
  if (continuePending_ && hasBody()) keepAlive_ = false;
  continuePending_ = false;
  std::string out = "HTTP/1.1 " + std::to_string(code) + " " + reason(code) + "\r\n";
  out += "Server: INKDECK\r\n";
  out += headers;
  out += "Content-Length: " + std::to_string(body.size()) + "\r\n";
  if (!keepAlive_) out += "Connection: close\r\n";
  out += "\r\n";
  if (!headOnly) out += body;
  sendAll(out.data(), out.size());
}

bool Connection::sendChunk(const std::string &data) {
  if (data.empty()) return true;
  char size[16];
  const int n = snprintf(size, sizeof size, "%x\r\n", static_cast<unsigned>(data.size()));
  return sendAll(size, n) && sendAll(data.data(), data.size()) && sendAll("\r\n", 2);
}

void Connection::serve() {
  timeval rcv{1, 0};  // recv wakes each second to see whether the server is closing
  setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &rcv, sizeof rcv);
  timeval snd{1, 0};
  setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &snd, sizeof snd);
  int one = 1;
  setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
  // The SDMMC driver DMAs whole sectors straight into a cache-line-aligned buffer.
  buf_ = static_cast<uint8_t *>(heap_caps_aligned_alloc(64, kIoBuf, MALLOC_CAP_SPIRAM));
  if (!buf_) return;

  while (!closing && !failed_) {
    Request r;
    keepAlive_ = true;
    chunked_ = false;
    bodyLeft_ = 0;
    chunkDone_ = true;
    continuePending_ = false;
    if (!readHead(r)) break;
    keepAlive_ = r.keepAlive;
    chunked_ = r.chunked;
    bodyLeft_ = chunked_ ? 0 : std::max<int64_t>(0, r.contentLength);
    chunkLeft_ = 0;
    chunkDone_ = !chunked_;
    continuePending_ = r.expectContinue && hasBody();
    handle(r);
    if (failed_ || !keepAlive_) break;
    // Whatever body the handler didn't need.
    if (hasBody()) {
      if (!chunked_ && bodyLeft_ > static_cast<int64_t>(kMaxDrain)) break;
      size_t drained = 0;
      int n;
      while ((n = readBody(buf_, kIoBuf)) > 0 && (drained += n) <= kMaxDrain) {
      }
      if (n != 0) break;
    }
  }
}

// ---- Methods -----------------------------------------------------------------------------------

enum class Auth : uint8_t { Ok, Missing, Stale, Bad };

// The fields of a Digest Authorization header.
std::map<std::string, std::string> digestFields(const std::string &h) {
  std::map<std::string, std::string> f;
  size_t i = 7;  // after "Digest "
  while (i < h.size()) {
    while (i < h.size() && (h[i] == ' ' || h[i] == ',')) ++i;
    const size_t eq = h.find('=', i);
    if (eq == std::string::npos) break;
    std::string key = h.substr(i, eq - i);
    while (!key.empty() && key.back() == ' ') key.pop_back();
    for (char &c : key) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    i = eq + 1;
    while (i < h.size() && h[i] == ' ') ++i;
    std::string value;
    if (i < h.size() && h[i] == '"') {
      ++i;
      while (i < h.size() && h[i] != '"') {
        if (h[i] == '\\' && i + 1 < h.size()) ++i;
        value += h[i++];
      }
      ++i;
    } else {
      const size_t end = h.find(',', i);
      value = h.substr(i, end == std::string::npos ? std::string::npos : end - i);
      while (!value.empty() && value.back() == ' ') value.pop_back();
      i = end == std::string::npos ? h.size() : end;
    }
    f[key] = value;
  }
  return f;
}

Auth checkAuth(const Request &r) {
  const std::string *h = r.header("authorization");
  if (!h) return Auth::Missing;
  if (strncasecmp(h->c_str(), "Digest ", 7) != 0) return Auth::Bad;
  auto f = digestFields(*h);
  if (f["username"] != kUser) return Auth::Bad;
  const std::string &alg = f["algorithm"];
  if (!alg.empty() && strcasecmp(alg.c_str(), "MD5") != 0) return Auth::Bad;
  if (f["nonce"] != nonce) return Auth::Stale;  // signed for an earlier session
  const std::string ha2 = md5hex(r.method + ":" + f["uri"]);
  const std::string &qop = f["qop"];
  const std::string expected =
      qop.empty() ? md5hex(ha1 + ":" + nonce + ":" + ha2)
                  : md5hex(ha1 + ":" + nonce + ":" + f["nc"] + ":" + f["cnonce"] + ":" + qop + ":" +
                           ha2);
  std::string response = f["response"];
  for (char &c : response) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
  return response == expected ? Auth::Ok : Auth::Bad;
}

void Connection::handle(Request &r) {
  if (r.method == "OPTIONS") {
    options();
    return;
  }
  const Auth auth = checkAuth(r);
  if (auth != Auth::Ok) {
    if (auth == Auth::Bad) {
      ++badSignIns;
      log_w("webdav: bad sign-in for %s %s", r.method.c_str(), r.uri.c_str());
    }
    challenge(auth == Auth::Stale);
    return;
  }
  if (r.path.empty()) {
    respond(400);
    return;
  }
  log_d("webdav: %s %s", r.method.c_str(), r.path.c_str());
  if (r.method == "PROPFIND") {
    propfind(r);
  } else if (r.method == "GET" || r.method == "HEAD") {
    get(r, r.method == "HEAD");
  } else if (r.method == "PUT") {
    put(r);
  } else if (r.method == "DELETE") {
    remove(r);
  } else if (r.method == "MKCOL") {
    mkcol(r);
  } else if (r.method == "COPY" || r.method == "MOVE") {
    copyMove(r, r.method == "MOVE");
  } else if (r.method == "LOCK") {
    lock(r);
  } else if (r.method == "UNLOCK") {
    respond(204);
  } else if (r.method == "PROPPATCH") {
    proppatch(r);
  } else {
    respond(r.method == "POST" || r.method == "PATCH" ? 405 : 501,
            std::string("Allow: ") + kAllow + "\r\n");
  }
}

void Connection::challenge(bool stale) {
  std::string h = std::string("WWW-Authenticate: Digest realm=\"") + kRealm +
                  "\", qop=\"auth\", algorithm=MD5, nonce=\"" + nonce + "\", opaque=\"" + opaque +
                  "\"";
  if (stale) h += ", stale=TRUE";
  respond(401, h + "\r\nContent-Type: text/plain\r\n", "sign in as inkdeck\n");
}

void Connection::options() {
  respond(200, std::string("DAV: 1, 2\r\nMS-Author-Via: DAV\r\nAllow: ") + kAllow + "\r\n");
}

std::string Connection::responseXml(const std::string &path, const Entry &e, bool quota) {
  std::string href = encodePath(path);
  if (e.dir && href.back() != '/') href += '/';
  std::string x = "<D:response><D:href>" + href + "</D:href><D:propstat><D:prop>";
  if (e.dir) {
    x += "<D:resourcetype><D:collection/></D:resourcetype>";
  } else {
    x += "<D:resourcetype/><D:getcontentlength>" + std::to_string(e.size) +
         "</D:getcontentlength><D:getcontenttype>" + contentType(path) +
         "</D:getcontenttype><D:getetag>" + etag(e) + "</D:getetag>";
  }
  if (e.mtime > 0) {
    x += "<D:getlastmodified>" + httpDate(e.mtime) + "</D:getlastmodified><D:creationdate>" +
         isoDate(e.mtime) + "</D:creationdate>";
  }
  x += "<D:displayname>" + xmlEscape(path == "/" ? "INKDECK" : nameOf(path)) + "</D:displayname>";
  x += "<D:supportedlock><D:lockentry><D:lockscope><D:exclusive/></D:lockscope><D:locktype>"
       "<D:write/></D:locktype></D:lockentry></D:supportedlock><D:lockdiscovery/>";
  uint64_t total = 0, free = 0;
  if (quota && e.dir && space(total, free)) {
    x += "<D:quota-available-bytes>" + std::to_string(free) +
         "</D:quota-available-bytes><D:quota-used-bytes>" + std::to_string(total - free) +
         "</D:quota-used-bytes>";
  }
  x += "</D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response>\n";
  return x;
}

// macOS's WebDAV file system (Finder's mounts and mount_webdav). Told the free space while it
// mounts, its kernel side asks again from inside mount(2), of the agent that is waiting in that
// very call, and gives up only after 90 s (webdavfs: webdav_vfs_getattr, 9 tries of 10 s; seen
// on macOS 27, 2026-11-17). With no quota properties it mounts at once and shows the free space
// as unknown.
bool fromMacMount(const Request &r) {
  const std::string *ua = r.header("user-agent");
  return ua && ua->compare(0, 9, "WebDAVFS/") == 0;
}

void Connection::propfind(const Request &r) {
  std::string body;  // which properties: all of ours go back whatever it asks
  if (!readSmallBody(body)) {
    keepAlive_ = false;
    respond(400);
    return;
  }
  const Entry e = lookup(r.path);
  if (!e.exists) {
    respond(404);
    return;
  }
  const std::string *depth = r.header("depth");
  const bool children = e.dir && !(depth && *depth == "0");
  sendHead(207, "Content-Type: application/xml; charset=\"utf-8\"\r\n", 0, true);
  std::string out =
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<D:multistatus xmlns:D=\"DAV:\">\n";
  out += responseXml(r.path, e, !fromMacMount(r));
  if (children) {
    FF_DIR *d = new FF_DIR;
    FILINFO *fi = new FILINFO;
    if (f_opendir(d, fatPath(r.path).c_str()) == FR_OK) {
      while (f_readdir(d, fi) == FR_OK && fi->fname[0]) {
        out += responseXml(childOf(r.path, fi->fname), entryOf(*fi), false);
        if (out.size() >= kChunkOut) {
          if (!sendChunk(out)) break;
          out.clear();
        }
      }
      f_closedir(d);
    }
    delete fi;
    delete d;
  }
  out += "</D:multistatus>\n";
  if (sendChunk(out)) sendAll("0\r\n\r\n", 5);
}

// Range: bytes=a-b, a- or -n (one range; anything else gets the whole file).
bool parseRange(const std::string &h, uint64_t size, uint64_t &from, uint64_t &to, bool &bad) {
  bad = false;
  if (h.compare(0, 6, "bytes=") != 0 || h.find(',') != std::string::npos) return false;
  const std::string spec = h.substr(6);
  const size_t dash = spec.find('-');
  if (dash == std::string::npos) return false;
  const std::string a = spec.substr(0, dash), b = spec.substr(dash + 1);
  if (a.empty()) {
    const uint64_t n = strtoull(b.c_str(), nullptr, 10);
    if (!n) {
      bad = true;
      return false;
    }
    from = n >= size ? 0 : size - n;
    to = size - 1;
  } else {
    from = strtoull(a.c_str(), nullptr, 10);
    to = b.empty() ? size - 1 : std::min<uint64_t>(strtoull(b.c_str(), nullptr, 10), size - 1);
  }
  if (from >= size || from > to) {
    bad = true;
    return false;
  }
  return true;
}

void Connection::get(const Request &r, bool headOnly) {
  const Entry e = lookup(r.path);
  if (!e.exists) {
    respond(404);
    return;
  }
  if (e.dir) {
    listing(r, headOnly);
    return;
  }
  FIL *f = new FIL;
  const FRESULT fr = f_open(f, fatPath(r.path).c_str(), FA_READ);
  if (fr != FR_OK) {
    delete f;
    respond(statusOf(fr));
    return;
  }
  uint64_t from = 0, to = e.size ? e.size - 1 : 0;
  bool partial = false, bad = false;
  if (const std::string *range = r.header("range")) {
    if (e.size) partial = parseRange(*range, e.size, from, to, bad);
  }
  if (bad) {
    f_close(f);
    delete f;
    respond(416, "Content-Range: bytes */" + std::to_string(e.size) + "\r\n");
    return;
  }
  const uint64_t length = e.size ? to - from + 1 : 0;
  std::string h = "Content-Type: " + contentType(r.path) + "\r\nLast-Modified: " +
                  httpDate(e.mtime) + "\r\nETag: " + etag(e) + "\r\nAccept-Ranges: bytes\r\n";
  if (partial) {
    h += "Content-Range: bytes " + std::to_string(from) + "-" + std::to_string(to) + "/" +
         std::to_string(e.size) + "\r\n";
  }
  sendHead(partial ? 206 : 200, h, static_cast<int64_t>(length));
  if (!headOnly && length) {
    if (from) f_lseek(f, from);
    uint64_t left = length;
    while (left && !failed_) {
      UINT got = 0;
      if (f_read(f, buf_, static_cast<UINT>(std::min<uint64_t>(left, kIoBuf)), &got) != FR_OK ||
          !got) {
        failed_ = true;  // the length is promised: all the client can learn is a cut connection
        break;
      }
      if (!sendAll(buf_, got)) break;
      left -= got;
      readTotal += got;
      lastReadMs = lastUseMs = millis();
      anyRead = true;
    }
  }
  f_close(f);
  delete f;
}

// A folder in a browser: a plain list of links.
void Connection::listing(const Request &r, bool headOnly) {
  std::string base = encodePath(r.path);
  if (base.back() != '/') base += '/';
  std::string html = "<!doctype html><meta charset=utf-8><title>INKDECK " +
                     xmlEscape(r.path) + "</title><h1>" + xmlEscape(r.path) + "</h1><ul>";
  if (r.path != "/") html += "<li><a href=\"../\">../</a></li>";
  for (const auto &n : listNames(r.path)) {
    const std::string name = n.first + (n.second ? "/" : "");
    html += "<li><a href=\"" + base + encodePath(n.first) + (n.second ? "/" : "") + "\">" +
            xmlEscape(name) + "</a></li>";
  }
  html += "</ul>";
  respond(200, "Content-Type: text/html; charset=utf-8\r\n", html, headOnly);
}

void Connection::put(const Request &r) {
  if (r.path == "/") {
    respond(405);
    return;
  }
  const Entry e = lookup(r.path);
  if (e.exists && e.dir) {
    respond(405);
    return;
  }
  const Entry parent = lookup(parentOf(r.path));
  if (!parent.exists || !parent.dir) {
    respond(409);
    return;
  }
  // Finder says how much is coming in X-Expected-Entity-Length when it sends chunks.
  int64_t expected = r.chunked ? -1 : r.contentLength;
  if (const std::string *x = r.header("x-expected-entity-length")) expected = atoll(x->c_str());
  uint64_t total = 0, free = 0;
  if (expected > 0 && space(total, free) && static_cast<uint64_t>(expected) > free) {
    respond(507);
    return;
  }
  FIL *f = new FIL;
  const std::string fat = fatPath(r.path);
  FRESULT fr = f_open(f, fat.c_str(), FA_CREATE_ALWAYS | FA_WRITE);
  if (fr != FR_OK) {
    delete f;
    respond(statusOf(fr, true));
    return;
  }
  touched(r.path);
  bool full = false;
  for (;;) {
    const int n = fillBody(buf_, kIoBuf);
    if (n <= 0) break;
    UINT put = 0;
    fr = f_write(f, buf_, n, &put);
    if (fr != FR_OK || put != static_cast<UINT>(n)) {
      full = fr == FR_OK;
      if (full) fr = FR_DENIED;
      break;
    }
    writtenTotal += n;
    lastWriteMs = lastUseMs = millis();
    anyWrite = true;
  }
  const FRESULT closed = f_close(f);
  delete f;
  if (fr == FR_OK) fr = closed;
  if (failed_ || fr != FR_OK) {
    f_unlink(fat.c_str());  // half a file is worse than none
    log_w("webdav: PUT %s failed (%s, FatFs %d)", r.path.c_str(),
          failed_ ? "the body broke off" : "the card", static_cast<int>(fr));
    if (failed_) return;  // the connection is gone, or past saving
    keepAlive_ = false;   // the rest of the body is still coming
    respond(full ? 507 : statusOf(fr, true));
    return;
  }
  changed("wrote", r.path);
  respond(e.exists ? 204 : 201);
}

void Connection::remove(const Request &r) {
  if (r.path == "/") {
    respond(403);
    return;
  }
  const Entry e = lookup(r.path);
  if (!e.exists) {
    respond(404);
    return;
  }
  const FRESULT fr = removeTree(r.path, e.dir);
  if (fr == FR_OK) changed("deleted", r.path);
  respond(fr == FR_OK ? 204 : statusOf(fr));
}

void Connection::mkcol(const Request &r) {
  if (hasBody()) {
    respond(415);
    return;
  }
  if (r.path == "/" || lookup(r.path).exists) {
    respond(405);
    return;
  }
  const Entry parent = lookup(parentOf(r.path));
  if (!parent.exists || !parent.dir) {
    respond(409);
    return;
  }
  const FRESULT fr = f_mkdir(fatPath(r.path).c_str());
  if (fr == FR_OK) changed("new folder", r.path);
  respond(fr == FR_OK ? 201 : statusOf(fr, true));
}

void Connection::copyMove(const Request &r, bool move) {
  const std::string *dest = r.header("destination");
  std::string to;
  if (!dest || !decodePath(*dest, to)) {
    respond(400);
    return;
  }
  const std::string &from = r.path;
  const std::string *ow = r.header("overwrite");
  const bool overwrite = !ow || (*ow != "F" && *ow != "f");
  if (from == "/" || to == "/" || from == to || inside(to, from)) {
    respond(403);
    return;
  }
  const Entry src = lookup(from);
  if (!src.exists) {
    respond(404);
    return;
  }
  const Entry parent = lookup(parentOf(to));
  if (!parent.exists || !parent.dir) {
    respond(409);
    return;
  }
  // A new case for the same name is a rename of the same entry, not an overwrite.
  const bool sameEntry = samePath(from, to);
  const Entry target = sameEntry ? Entry{} : lookup(to);
  if (target.exists) {
    if (!overwrite) {
      respond(412);
      return;
    }
    const FRESULT fr = removeTree(to, target.dir);
    if (fr != FR_OK) {
      respond(statusOf(fr));
      return;
    }
  }
  FRESULT fr;
  if (move) {
    fr = f_rename(fatPath(from).c_str(), fatPath(to).c_str());
    if (fr == FR_OK) touched(from);
  } else {
    const std::string *depth = r.header("depth");
    fr = copyTree(from, to, src.dir, !(depth && *depth == "0"), buf_);
  }
  if (fr == FR_OK) changed(move ? "moved to" : "copied to", to);
  respond(fr == FR_OK ? (target.exists ? 204 : 201) : statusOf(fr, !move));
}

void Connection::lock(const Request &r) {
  std::string body;
  if (!readSmallBody(body)) {
    keepAlive_ = false;
    respond(400);
    return;
  }
  if (r.path == "/") {
    respond(403);
    return;
  }
  int code = 200;
  if (!lookup(r.path).exists) {
    // Locking a name that doesn't exist creates an empty file there (RFC 4918, 9.10.4).
    const Entry parent = lookup(parentOf(r.path));
    if (!parent.exists || !parent.dir) {
      respond(409);
      return;
    }
    FIL *f = new FIL;
    const FRESULT fr = f_open(f, fatPath(r.path).c_str(), FA_CREATE_NEW | FA_WRITE);
    if (fr == FR_OK) f_close(f);
    delete f;
    if (fr != FR_OK) {
      respond(statusOf(fr, true));
      return;
    }
    touched(r.path);
    code = 201;
  }
  // Locks are advisory: every LOCK gets one, and nothing is refused for want of a token. A
  // refresh (no body) keeps the token it names.
  std::string token;
  const std::string *ifHeader = r.header("if");
  if (body.empty() && ifHeader) {
    const size_t a = ifHeader->find("opaquelocktoken:");
    const size_t b = a == std::string::npos ? a : ifHeader->find('>', a);
    if (b != std::string::npos) token = ifHeader->substr(a, b - a);
  }
  if (token.empty()) {
    const std::string h = randomHex(16);
    token = "opaquelocktoken:" + h.substr(0, 8) + "-" + h.substr(8, 4) + "-" + h.substr(12, 4) +
            "-" + h.substr(16, 4) + "-" + h.substr(20);
  }
  const std::string *depth = r.header("depth");
  const std::string xml =
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<D:prop xmlns:D=\"DAV:\"><D:lockdiscovery>"
      "<D:activelock><D:locktype><D:write/></D:locktype><D:lockscope><D:exclusive/>"
      "</D:lockscope><D:depth>" +
      std::string(depth && *depth == "0" ? "0" : "infinity") +
      "</D:depth><D:timeout>Second-3600</D:timeout><D:locktoken><D:href>" + token +
      "</D:href></D:locktoken><D:lockroot><D:href>" + encodePath(r.path) +
      "</D:href></D:lockroot></D:activelock></D:lockdiscovery></D:prop>\n";
  respond(code, "Lock-Token: <" + token + ">\r\nContent-Type: application/xml; charset=\"utf-8\"\r\n",
          xml);
}

// Accepted and answered property by property, but nothing is kept: FAT has nowhere to put dead
// properties (Windows sends its file times this way).
void Connection::proppatch(const Request &r) {
  std::string body;
  if (!readSmallBody(body)) {
    keepAlive_ = false;
    respond(400);
    return;
  }
  const Entry e = lookup(r.path);
  if (!e.exists) {
    respond(404);
    return;
  }
  std::string href = encodePath(r.path);
  if (e.dir && href.back() != '/') href += '/';
  std::string xml = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<D:multistatus xmlns:D=\"DAV:\">"
                    "<D:response><D:href>" +
                    href + "</D:href><D:propstat><D:prop>";
  for (const auto &p : patchedProps(body)) {
    xml += p.first.empty() ? "<" + p.second + " xmlns=\"\"/>"
                           : "<P:" + p.second + " xmlns:P=\"" + xmlEscape(p.first) + "\"/>";
  }
  xml += "</D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response>"
         "</D:multistatus>\n";
  respond(207, "Content-Type: application/xml; charset=\"utf-8\"\r\n", xml);
}

// ---- The server --------------------------------------------------------------------------------

std::atomic<uint32_t> stackLeast{UINT32_MAX};  // the least stack a connection had left

// A connection's task. Its stack is in PSRAM (internal RAM is Wi-Fi's: about 55 KB is left
// with it up), which is safe because it touches only the card and sockets, never the flash.
// Such a task is best deleted by another, so it parks itself when done and the listener's
// reap() deletes it. Slots outlive a session: one still closing when the server stops is
// collected by the next.
struct Slot {
  TaskHandle_t task = nullptr;
  int fd = -1;
  std::atomic<bool> done{false};
};
Slot slots[kMaxClients];

void connectionTask(void *arg) {
  Slot &slot = *static_cast<Slot *>(arg);
  {
    Connection c(slot.fd);
    c.serve();
  }
  shutdown(slot.fd, SHUT_RDWR);
  close(slot.fd);
  const uint32_t left = uxTaskGetStackHighWaterMark(nullptr);
  if (left < stackLeast) stackLeast = left;
  slot.done = true;
  vTaskSuspend(nullptr);
}

// Deletes the connection tasks that have finished; how many still run.
int reap() {
  int running = 0;
  for (Slot &s : slots) {
    if (!s.task) continue;
    if (s.done && eTaskGetState(s.task) == eSuspended) {
      vTaskDeleteWithCaps(s.task);
      s.task = nullptr;
      s.done = false;
    } else {
      ++running;
    }
  }
  clients = running;
  return running;
}

Slot *freeSlot() {
  for (Slot &s : slots) {
    if (!s.task) return &s;
  }
  return nullptr;
}

uint32_t idleForMs() { return std::min(millis() - lastUseMs.load(), input::idleMs()); }

// Joins, serves until stop() or the idle limit, and leaves the radio as it found it. "" when
// stopped, else why it ended.
std::string serveCard() {
  std::string error;
  if (!wlan::connect(kConnectTimeoutMs, error)) return "WI-FI: " + error;
  WiFi.setSleep(false);  // modem sleep slows every transfer several times over
  setText(ipText, WiFi.localIP().toString().c_str());

  const int listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  int one = 1;
  setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(kPort);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if (listener < 0 || bind(listener, reinterpret_cast<sockaddr *>(&addr), sizeof addr) < 0 ||
      listen(listener, kMaxClients) < 0) {
    log_e("webdav: can't listen on port %u (errno %d)", kPort, errno);
    if (listener >= 0) close(listener);
    WiFi.setSleep(true);
    wlan::disconnect();
    return "COULDN'T OPEN PORT " + std::to_string(kPort);
  }
  if (MDNS.begin(ota::kHostname)) {
    MDNS.addService("webdav", "tcp", kPort);
    MDNS.addServiceTxt("webdav", "tcp", "path", "/");
  }
  lastUseMs = millis();
  phase = Phase::Serving;
  log_i("webdav: serving the card at http://%s.local/ (%s)", ota::kHostname,
        WiFi.localIP().toString().c_str());

  bool timedOut = false;
  while (!stopRequested) {
    reap();
    if (idleForMs() >= kIdleMs) {
      timedOut = true;
      break;
    }
    Slot *slot = freeSlot();
    if (!slot) {  // the next connection waits in the backlog
      delay(50);
      continue;
    }
    fd_set ready;
    FD_ZERO(&ready);
    FD_SET(listener, &ready);
    timeval tv{0, 250000};
    if (select(listener + 1, &ready, nullptr, nullptr, &tv) <= 0) continue;
    slot->fd = accept(listener, nullptr, nullptr);
    if (slot->fd < 0) continue;
    slot->done = false;
    if (xTaskCreatePinnedToCoreWithCaps(connectionTask, "webdav-conn", kStack, slot, 1,
                                        &slot->task, tskNO_AFFINITY,
                                        MALLOC_CAP_SPIRAM) != pdPASS) {
      log_w("webdav: no memory for another connection");
      slot->task = nullptr;
      close(slot->fd);
    }
  }

  closing = true;  // connections finish the buffer they're on and close
  close(listener);
  const uint32_t t0 = millis();
  while (reap() > 0 && millis() - t0 < 5000) delay(50);
  MDNS.end();
  WiFi.setSleep(true);
  wlan::disconnect();
  log_i("webdav: stopped%s: %llu bytes read, %llu written, least connection stack left %lu",
        timedOut ? " (idle)" : "", static_cast<unsigned long long>(readTotal.load()),
        static_cast<unsigned long long>(writtenTotal.load()),
        static_cast<unsigned long>(stackLeast.load()));
  return timedOut ? "STOPPED AFTER " + std::to_string(kIdleMinutes) + " MIN IDLE" : "";
}

void run(void *) {
  {
    const std::string why = serveCard();
    setText(resultText, why);
    phase = why.empty() ? Phase::Idle : Phase::Failed;
  }  // before the task goes: vTaskDelete() skips destructors
  running = false;
  vTaskDelete(nullptr);
}

// SD_MMC keeps the FatFs drive it mounted the card on to itself.
struct DriveOf : fs::SDMMCFS {
  static uint8_t of(fs::SDMMCFS &card) { return card.*(&DriveOf::_pdrv); }
};

}  // namespace

bool configured() { return wlan::configured(); }

std::string password() {
  if (!cachedPassword.empty()) return cachedPassword;
  Preferences prefs;
  prefs.begin("webdav", false);
  std::string pass = prefs.isKey("password") ? prefs.getString("password").c_str() : "";
  if (pass.size() != 14) {
    // No 0/o, 1/l/i: it's read off the panel and typed in.
    static const char kAlphabet[] = "abcdefghjkmnpqrstuvwxyz23456789";
    pass.clear();
    for (int i = 0; i < 12; ++i) {
      if (i && i % 4 == 0) pass += '-';
      pass += kAlphabet[esp_random() % (sizeof kAlphabet - 1)];
    }
    prefs.putString("password", pass.c_str());
  }
  prefs.end();
  cachedPassword = pass;
  return pass;
}

bool start() {
  if (!configured()) return false;
  stopRequested = false;  // withdraws a stop the task hasn't acted on yet
  if (running.exchange(true)) return true;
  if (!sdcard::mounted()) {
    setText(resultText, "NO SD CARD");
    phase = Phase::Failed;
    running = false;
    return false;
  }
  drive[0] = static_cast<char>('0' + DriveOf::of(SD_MMC));
  ha1 = md5hex(std::string(kUser) + ":" + kRealm + ":" + password());
  nonce = randomHex(16);
  opaque = randomHex(8);
  closing = false;
  readTotal = writtenTotal = 0;
  anyRead = anyWrite = false;
  badSignIns = 0;
  settingsTouched = podcastsTouched = false;
  setText(resultText, "");
  setText(lastChangeText, "");
  setText(ipText, "");
  phase = Phase::Joining;
  // Wi-Fi and mDNS need more than the default stack.
  if (xTaskCreate(run, "webdav", 6144, nullptr, 1, nullptr) != pdPASS) {
    setText(resultText, "NO MEMORY FOR THE SERVER");
    phase = Phase::Failed;
    running = false;
    return false;
  }
  return true;
}

void stop() {
  if (running) stopRequested = true;
}

Status status() {
  Status s;
  s.phase = phase.load();
  s.readBytes = readTotal.load();
  s.writtenBytes = writtenTotal.load();
  const uint32_t now = millis();
  s.msSinceRead = anyRead ? now - lastReadMs.load() : UINT32_MAX;
  s.msSinceWrite = anyWrite ? now - lastWriteMs.load() : UINT32_MAX;
  s.clients = clients.load();
  s.badSignIns = badSignIns.load();
  if (s.phase == Phase::Serving) {
    const uint32_t idle = idleForMs();
    s.secondsLeft = idle < kIdleMs ? (kIdleMs - idle + 999) / 1000 : 0;
  }
  std::lock_guard<std::mutex> lock(textMutex);
  s.ip = ipText;
  s.result = resultText;
  s.lastChange = lastChangeText;
  return s;
}

bool active() {
  const Phase p = phase.load();
  return p == Phase::Joining || p == Phase::Serving;
}

bool changedSettings() { return settingsTouched; }
bool changedPodcasts() { return podcastsTouched; }

}  // namespace webdav
