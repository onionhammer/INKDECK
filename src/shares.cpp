#include "shares.h"

#include <Arduino.h>
#include <WiFi.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <esp_vfs.h>
#include <fcntl.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include <algorithm>
#include <map>
#include <mutex>
#include <new>

#include "settings.h"
#include "share_backend.h"
#include "wlan.h"

namespace shares {
namespace {

constexpr uint32_t kConnectTimeoutMs = 20000;
constexpr uint32_t kCacheMs = 60000;  // how long a listing's sizes are trusted
constexpr size_t kCacheMax = 3000;
constexpr size_t kMaxListing = 2000;  // entries read from one folder (DirList shows 1000)
constexpr int kMaxFiles = 8;

struct Cached {
  Attr attr;
  uint32_t at = 0;  // millis() when it was read
};

struct Mount {
  Share share;
  std::unique_ptr<Backend> backend;  // set while mounted
  bool mounted = false;
  int users = 0;  // acquire()s not yet released, and open files
  uint32_t idleSince = 0;
  std::map<std::string, Cached> cache;  // attributes by path below the root ("" = the root)
};

struct File {
  Mount *mount = nullptr;  // nullptr: the slot is free
  void *handle = nullptr;  // the backend's
  Attr attr;
  uint64_t pos = 0;
  std::unique_ptr<uint8_t[]> buf;  // read-ahead: one read's worth, in PSRAM (psram_new.cpp)
  uint64_t bufOff = 0;
  uint32_t bufLen = 0;
  bool eofKnown = false;
  uint64_t eofAt = 0;
};

struct Dir {
  DIR base;  // first: the VFS layer keeps its index here
  std::vector<DirEntry> entries;
  size_t next = 0;
  dirent ent;
};

std::mutex lock;       // the mounts' state, caches, open files and backends
std::mutex mountLock;  // one acquire() mounting at a time
std::mutex loadLock;
bool loaded = false;
std::vector<Share> shareList;
std::vector<std::unique_ptr<Mount>> mounts;  // one per share, made with shareList
// The open-file table, in PSRAM rather than internal RAM (it's 1 KB+).
std::unique_ptr<File[]> files;
bool reaperRunning = false;

void load() {
  std::lock_guard<std::mutex> g(loadLock);
  if (loaded) return;
  loaded = true;
  files.reset(new File[kMaxFiles]);
  for (const settings::ShareLine &line : settings::shareLines()) {
    Share s;
    std::string password, error;
    if (!parse(line.kind, line.url, s, password, error)) continue;  // settings.cpp logged it
    if (static_cast<int>(shareList.size()) >= kMaxShares) break;
    const std::string base = s.name;
    for (int n = 2;; ++n) {
      const bool taken = std::any_of(shareList.begin(), shareList.end(), [&](const Share &o) {
        return strcasecmp(o.name.c_str(), s.name.c_str()) == 0;
      });
      if (!taken) break;
      s.name = base + " " + std::to_string(n);
    }
    shareList.push_back(s);
    auto m = std::make_unique<Mount>();
    m->share = s;
    mounts.push_back(std::move(m));
  }
}

// --- Paths -----------------------------------------------------------------------------------

// "/Books/Wells/a.txt" (below /net) -> Books's mount, rel "Wells/a.txt". Caller holds lock.
Mount *split(const char *path, std::string &rel) {
  while (*path == '/') ++path;
  const char *slash = strchr(path, '/');
  const std::string name = slash ? std::string(path, slash - path) : std::string(path);
  rel = slash ? slash + 1 : "";
  while (!rel.empty() && rel.back() == '/') rel.pop_back();
  for (auto &m : mounts) {
    if (m->share.name == name) return m.get();
  }
  return nullptr;
}

// rel's attributes: from the last listing of its folder if that's recent, else from the
// server. Caller holds lock.
int statPath(Mount &m, const std::string &rel, Attr &out) {
  if (!m.mounted) return ENOTCONN;
  const auto it = m.cache.find(rel);
  if (it != m.cache.end() && millis() - it->second.at <= kCacheMs) {
    out = it->second.attr;
    return 0;
  }
  const int rc = m.backend->stat(rel, out);
  if (rc == 0) m.cache[rel] = Cached{out, millis()};
  return rc;
}

void fillStat(const Attr &a, struct stat *st) {
  memset(st, 0, sizeof *st);
  const mode_t type = a.type == Type::Dir ? S_IFDIR : a.type == Type::Link ? S_IFLNK : S_IFREG;
  const uint32_t perms = a.mode ? a.mode & 0777 : (a.type == Type::Dir ? 0755 : 0644);
  st->st_mode = type | perms;
  st->st_size = static_cast<off_t>(std::min<uint64_t>(a.size, 0x7fffffff));  // off_t is 32-bit
  st->st_mtime = static_cast<time_t>(a.mtime);
  st->st_ino = static_cast<ino_t>(a.fileid);
  st->st_nlink = 1;
}

// --- The VFS ---------------------------------------------------------------------------------

File *fileAt(int fd) {
  return files && fd >= 0 && fd < kMaxFiles && files[fd].mount ? &files[fd] : nullptr;
}

int vfsOpen(const char *path, int flags, int /*mode*/) {
  if ((flags & O_ACCMODE) != O_RDONLY || (flags & (O_CREAT | O_TRUNC | O_APPEND))) {
    errno = EROFS;
    return -1;
  }
  std::lock_guard<std::mutex> g(lock);
  std::string rel;
  Mount *m = split(path, rel);
  if (!m || !m->mounted) {
    errno = m ? ENOTCONN : ENOENT;
    return -1;
  }
  int fd = 0;
  while (fd < kMaxFiles && files[fd].mount) ++fd;
  if (fd == kMaxFiles) {
    errno = ENFILE;
    return -1;
  }
  File &f = files[fd];
  f.buf.reset(new (std::nothrow) uint8_t[m->backend->readSize()]);
  if (!f.buf) {
    errno = ENOMEM;
    return -1;
  }
  void *handle = nullptr;
  Attr attr;
  const int rc = m->backend->open(rel, handle, attr);
  if (rc == 0 && attr.type != Type::File) {  // a folder, or a link we don't follow
    m->backend->close(handle);
    errno = attr.type == Type::Dir ? EISDIR : EINVAL;
    f.buf.reset();
    return -1;
  }
  if (rc != 0) {
    errno = rc;
    f.buf.reset();
    return -1;
  }
  f.mount = m;
  f.handle = handle;
  f.attr = attr;
  f.pos = 0;
  f.bufOff = 0;
  f.bufLen = 0;
  f.eofKnown = false;
  ++m->users;
  return fd;
}

int vfsClose(int fd) {
  std::lock_guard<std::mutex> g(lock);
  File *f = fileAt(fd);
  if (!f) {
    errno = EBADF;
    return -1;
  }
  Mount &m = *f->mount;
  if (m.mounted) m.backend->close(f->handle);
  if (m.users > 0 && --m.users == 0) m.idleSince = millis();
  f->buf.reset();
  f->handle = nullptr;
  f->mount = nullptr;
  return 0;
}

// Reads from pos through the read-ahead buffer: small reads (stdio's) come out of one network
// read. Caller holds lock.
ssize_t readAt(File &f, uint8_t *dst, size_t n, uint64_t &pos) {
  Mount &m = *f.mount;
  if (!m.mounted) {
    errno = EIO;
    return -1;
  }
  const uint32_t chunk = m.backend->readSize();
  size_t done = 0;
  while (done < n) {
    if (pos >= f.bufOff && pos < f.bufOff + f.bufLen) {
      const size_t k = static_cast<size_t>(std::min<uint64_t>(n - done, f.bufOff + f.bufLen - pos));
      memcpy(dst + done, f.buf.get() + (pos - f.bufOff), k);
      done += k;
      pos += k;
      continue;
    }
    if (f.eofKnown && pos >= f.eofAt) break;
    uint32_t got = 0;
    bool eof = false;
    int rc;
    if (n - done >= chunk) {  // a big read goes straight to the caller
      rc = m.backend->read(f.handle, pos, chunk, dst + done, got, eof);
      if (rc == 0 && eof) {
        f.eofKnown = true;
        f.eofAt = pos + got;
      }
      if (rc == 0) {
        done += got;
        pos += got;
      }
    } else {
      rc = m.backend->read(f.handle, pos, chunk, f.buf.get(), got, eof);
      if (rc == 0) {
        f.bufOff = pos;
        f.bufLen = got;
        if (eof) {
          f.eofKnown = true;
          f.eofAt = pos + got;
        }
      }
    }
    if (rc != 0) {
      log_w("shares: read failed (errno %d)", rc);
      if (done > 0) break;
      errno = rc;
      return -1;
    }
    if (got == 0) break;
  }
  return static_cast<ssize_t>(done);
}

ssize_t vfsRead(int fd, void *dst, size_t size) {
  std::lock_guard<std::mutex> g(lock);
  File *f = fileAt(fd);
  if (!f) {
    errno = EBADF;
    return -1;
  }
  return readAt(*f, static_cast<uint8_t *>(dst), size, f->pos);
}

ssize_t vfsPread(int fd, void *dst, size_t size, off_t offset) {
  std::lock_guard<std::mutex> g(lock);
  File *f = fileAt(fd);
  if (!f || offset < 0) {
    errno = f ? EINVAL : EBADF;
    return -1;
  }
  uint64_t pos = static_cast<uint64_t>(offset);
  return readAt(*f, static_cast<uint8_t *>(dst), size, pos);
}

off_t vfsLseek(int fd, off_t offset, int whence) {
  std::lock_guard<std::mutex> g(lock);
  File *f = fileAt(fd);
  if (!f) {
    errno = EBADF;
    return -1;
  }
  int64_t base;
  switch (whence) {
    case SEEK_SET: base = 0; break;
    case SEEK_CUR: base = static_cast<int64_t>(f->pos); break;
    case SEEK_END: base = static_cast<int64_t>(f->attr.size); break;
    default: errno = EINVAL; return -1;
  }
  const int64_t pos = base + offset;
  if (pos < 0 || pos > 0x7fffffff) {
    errno = EINVAL;
    return -1;
  }
  f->pos = static_cast<uint64_t>(pos);
  return static_cast<off_t>(pos);
}

int vfsFstat(int fd, struct stat *st) {
  std::lock_guard<std::mutex> g(lock);
  File *f = fileAt(fd);
  if (!f) {
    errno = EBADF;
    return -1;
  }
  fillStat(f->attr, st);
  return 0;
}

int vfsStat(const char *path, struct stat *st) {
  std::lock_guard<std::mutex> g(lock);
  std::string rel;
  Mount *m = split(path, rel);
  if (!m) {
    errno = ENOENT;
    return -1;
  }
  Attr a;
  const int rc = statPath(*m, rel, a);
  if (rc != 0) {
    errno = rc;
    return -1;
  }
  fillStat(a, st);
  return 0;
}

DIR *vfsOpendir(const char *path) {
  std::lock_guard<std::mutex> g(lock);
  std::string rel;
  Mount *m = split(path, rel);
  if (!m || !m->mounted) {
    errno = m ? ENOTCONN : ENOENT;
    return nullptr;
  }
  std::unique_ptr<Dir> d(new Dir());
  const int rc = m->backend->list(rel, d->entries, kMaxListing);
  if (rc != 0) {
    log_w("shares: can't list %s (errno %d)", path, rc);
    errno = rc;
    return nullptr;
  }

  // Keep the listing's attributes, so the stat() of each row and the open() of one needn't
  // ask again. Devices, sockets and pipes are left out.
  const uint32_t now = millis();
  for (auto it = m->cache.begin(); it != m->cache.end();) {
    it = now - it->second.at > kCacheMs ? m->cache.erase(it) : std::next(it);
  }
  if (m->cache.size() + d->entries.size() > kCacheMax) m->cache.clear();
  const std::string prefix = rel.empty() ? std::string() : rel + "/";
  for (const DirEntry &e : d->entries) m->cache[prefix + e.name] = Cached{e.attr, now};
  d->entries.erase(std::remove_if(d->entries.begin(), d->entries.end(),
                                  [](const DirEntry &e) { return e.attr.type == Type::Other; }),
                   d->entries.end());
  return &d.release()->base;
}

struct dirent *vfsReaddir(DIR *pdir) {
  Dir *d = reinterpret_cast<Dir *>(pdir);
  if (d->next >= d->entries.size()) return nullptr;
  const DirEntry &e = d->entries[d->next++];
  d->ent.d_ino = static_cast<ino_t>(e.attr.fileid);
  d->ent.d_type = e.attr.type == Type::Dir ? DT_DIR : e.attr.type == Type::Link ? DT_LNK : DT_REG;
  strlcpy(d->ent.d_name, e.name.c_str(), sizeof d->ent.d_name);
  return &d->ent;
}

int vfsReaddirR(DIR *pdir, struct dirent *entry, struct dirent **out) {
  struct dirent *e = vfsReaddir(pdir);
  if (e) {
    *entry = *e;
    *out = entry;
  } else {
    *out = nullptr;
  }
  return 0;
}

int vfsClosedir(DIR *pdir) {
  delete reinterpret_cast<Dir *>(pdir);
  return 0;
}

bool registerVfs() {
  static bool registered = false;
  if (registered) return true;
  static esp_vfs_t vfs = {};
  vfs.flags = ESP_VFS_FLAG_DEFAULT | ESP_VFS_FLAG_READONLY_FS;
  vfs.open = vfsOpen;
  vfs.close = vfsClose;
  vfs.read = vfsRead;
  vfs.pread = vfsPread;
  vfs.lseek = vfsLseek;
  vfs.fstat = vfsFstat;
  vfs.stat = vfsStat;
  vfs.opendir = vfsOpendir;
  vfs.readdir = vfsReaddir;
  vfs.readdir_r = vfsReaddirR;
  vfs.closedir = vfsClosedir;
  const esp_err_t err = esp_vfs_register(kMountPoint, &vfs, nullptr);
  if (err != ESP_OK) {
    log_e("shares: can't register %s (%s)", kMountPoint, esp_err_to_name(err));
    return false;
  }
  registered = true;
  return true;
}

// --- Letting go ------------------------------------------------------------------------------

// Unmounts shares nobody has used for kIdleMs, and lets Wi-Fi go with each. Runs while any
// share is mounted.
void reaper(void *) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    std::vector<std::unique_ptr<Backend>> idle;
    bool anyMounted = false;
    {
      std::lock_guard<std::mutex> g(lock);
      for (auto &mp : mounts) {
        Mount &m = *mp;
        if (!m.mounted) continue;
        if (m.users == 0 && millis() - m.idleSince >= kIdleMs) {
          log_i("shares: %s unused for %lu s; unmounting", m.share.name.c_str(),
                static_cast<unsigned long>(kIdleMs / 1000));
          m.mounted = false;
          m.cache.clear();
          idle.push_back(std::move(m.backend));
        } else {
          anyMounted = true;
        }
      }
      if (!anyMounted) reaperRunning = false;
    }
    for (auto &backend : idle) {
      backend.reset();  // unmounts, then the connection closes
      if (!anyMounted && &backend == &idle.back()) WiFi.setSleep(true);  // as acquire() found it
      wlan::disconnect();
    }
    if (!anyMounted) break;
  }
  vTaskDelete(nullptr);
}

// Caller holds lock.
void startReaper() {
  if (reaperRunning) return;
  if (xTaskCreate(reaper, "share-idle", 6144, nullptr, 1, nullptr) == pdPASS) {
    reaperRunning = true;
  } else {
    log_e("shares: no memory for the idle task; shares stay mounted");
  }
}

}  // namespace

// --- Shared by the backends ------------------------------------------------------------------

bool parseNumber(const std::string &s, uint32_t max, uint32_t &out) {
  if (s.empty() || s.size() > 10) return false;
  uint64_t v = 0;
  for (const char c : s) {
    if (c < '0' || c > '9') return false;
    v = v * 10 + static_cast<uint64_t>(c - '0');
  }
  if (v > max) return false;
  out = static_cast<uint32_t>(v);
  return true;
}

std::string percentDecode(const std::string &s) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size() && isxdigit(static_cast<unsigned char>(s[i + 1])) &&
        isxdigit(static_cast<unsigned char>(s[i + 2]))) {
      out += static_cast<char>(strtol(s.substr(i + 1, 2).c_str(), nullptr, 16));
      i += 2;
    } else {
      out += s[i];
    }
  }
  return out;
}

// --- Public ----------------------------------------------------------------------------------

std::string Share::url() const {
  const std::string authority = host + (port ? ":" + std::to_string(port) : std::string());
  if (kind == Kind::Nfs) return "nfs://" + authority + path;
  std::string who;
  if (!domain.empty()) who += domain + ";";
  if (!user.empty()) who += user + "@";
  return "smb://" + who + authority + "/" + path;
}

std::string Share::credentialId() const {
  std::string id = domain + ";" + user + "@" + host + "/" + path.substr(0, path.find('/'));
  for (char &c : id) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
  return id;
}

bool parse(Kind kind, const std::string &spec, Share &out, std::string &password,
           std::string &error) {
  out = Share();
  out.kind = kind;
  password.clear();
  const bool ok = kind == Kind::Smb ? parseSmb(spec, out, password, error)
                                    : parseNfs(spec, out, error);
  if (!ok) return false;
  if (out.name.empty()) {
    const size_t slash = out.path.rfind('/');
    out.name = out.path.substr(slash == std::string::npos ? 0 : slash + 1);
    if (out.name.empty()) out.name = out.host;
  }
  std::replace(out.name.begin(), out.name.end(), '/', '-');
  return true;
}

const std::vector<Share> &all() {
  load();
  return shareList;
}

int shareOf(const std::string &path) {
  load();
  for (size_t i = 0; i < shareList.size(); ++i) {
    const std::string root = shareList[i].mountPath();
    if (path.compare(0, root.size(), root) == 0 &&
        (path.size() == root.size() || path[root.size()] == '/')) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

bool acquireIfMounted(int i) {
  load();
  std::lock_guard<std::mutex> g(lock);
  if (i < 0 || i >= static_cast<int>(mounts.size()) || !mounts[i]->mounted) return false;
  ++mounts[i]->users;
  return true;
}

bool acquire(int i, std::string &error) {
  load();
  if (i < 0 || i >= static_cast<int>(mounts.size())) {
    error = "NO SUCH SHARE";
    return false;
  }
  if (acquireIfMounted(i)) return true;
  std::lock_guard<std::mutex> one(mountLock);
  if (acquireIfMounted(i)) return true;  // another caller mounted it while we waited
  if (!registerVfs()) {
    error = "CAN'T ADD A FILE SYSTEM";
    return false;
  }
  std::string why;
  if (!wlan::connect(kConnectTimeoutMs, why)) {
    error = "WI-FI: " + why;
    return false;
  }

  Mount &m = *mounts[i];
  const uint32_t t0 = millis();
  std::unique_ptr<Backend> backend =
      m.share.kind == Kind::Smb ? makeSmbBackend() : makeNfsBackend();
  const std::string password =
      m.share.kind == Kind::Smb ? settings::sharePassword(m.share.credentialId()) : "";
  if (!backend->mount(m.share, password, error)) {  // outside the lock: others keep going
    log_w("shares: %s: %s", m.share.url().c_str(), error.c_str());
    backend.reset();
    wlan::disconnect();
    return false;
  }
  log_i("shares: %s mounted at %s in %lu ms", m.share.url().c_str(), m.share.mountPath().c_str(),
        static_cast<unsigned long>(millis() - t0));
  // Modem sleep wakes the radio only at the access point's beacons, every 100 ms or more:
  // each read would wait for one (ota.cpp found the same). Off while a share is mounted; the
  // reaper puts it back.
  WiFi.setSleep(false);
  std::lock_guard<std::mutex> g(lock);
  m.backend = std::move(backend);
  m.cache.clear();
  m.mounted = true;
  ++m.users;
  m.idleSince = millis();
  startReaper();
  return true;
}

void release(int i) {
  std::lock_guard<std::mutex> g(lock);
  if (i < 0 || i >= static_cast<int>(mounts.size())) return;
  Mount &m = *mounts[i];
  if (m.users > 0 && --m.users == 0) m.idleSince = millis();
}

bool mounted(int i) {
  load();
  std::lock_guard<std::mutex> g(lock);
  return i >= 0 && i < static_cast<int>(mounts.size()) && mounts[i]->mounted;
}

// --- Lease -----------------------------------------------------------------------------------

struct Lease::Job {
  std::mutex m;
  int share = -1;
  bool done = false;
  bool ok = false;
  bool abandoned = false;  // the Lease went first: release() what the task acquires
  std::string error;
};

void Lease::run(void *arg) {
  {
    std::shared_ptr<Job> job = *static_cast<std::shared_ptr<Job> *>(arg);
    delete static_cast<std::shared_ptr<Job> *>(arg);
    std::string error;
    const bool ok = acquire(job->share, error);
    bool letGo;
    {
      std::lock_guard<std::mutex> g(job->m);
      job->done = true;
      job->ok = ok;
      job->error = error;
      letGo = ok && job->abandoned;
    }
    if (letGo) release(job->share);
  }
  vTaskDelete(nullptr);
}

void Lease::start() {
  if (held_ || state() == State::Connecting) return;
  if (acquireIfMounted(share_)) {
    held_ = true;
    job_.reset();
    return;
  }
  job_ = std::make_shared<Job>();
  job_->share = share_;
  auto *arg = new std::shared_ptr<Job>(job_);
  // Wi-Fi's connect, the sockets and SMB's sign-in (NTLM, signing keys) want room.
  if (xTaskCreate(run, "share-mount", 12288, arg, 1, nullptr) != pdPASS) {
    delete arg;
    job_->done = true;
    job_->error = "OUT OF MEMORY";
  }
}

Lease::State Lease::state() const {
  if (held_) return State::Ready;
  if (!job_) return State::Idle;
  bool ok;
  {
    std::lock_guard<std::mutex> g(job_->m);
    if (!job_->done) return State::Connecting;
    ok = job_->ok;
  }
  if (!ok) return State::Failed;
  held_ = true;  // the task's acquire() is ours now
  job_.reset();
  return State::Ready;
}

std::string Lease::error() const {
  if (!job_) return {};
  std::lock_guard<std::mutex> g(job_->m);
  return job_->error;
}

Lease::~Lease() {
  if (held_) {
    release(share_);
    return;
  }
  if (!job_) return;
  bool letGo = false;
  {
    std::lock_guard<std::mutex> g(job_->m);
    if (job_->done) {
      letGo = job_->ok;
    } else {
      job_->abandoned = true;
    }
  }
  if (letGo) release(share_);
}

}  // namespace shares
