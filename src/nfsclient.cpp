#include "nfsclient.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#ifdef ARDUINO
#include <esp32-hal-log.h>
#include <esp_random.h>
#define nfs_random() esp_random()
#else  // a desktop build, for testing
#include <stdio.h>
#include <stdlib.h>
#define log_i(fmt, ...) fprintf(stderr, "[I] " fmt "\n", ##__VA_ARGS__)
#define log_w(fmt, ...) fprintf(stderr, "[W] " fmt "\n", ##__VA_ARGS__)
#define nfs_random() static_cast<uint32_t>(random())
#endif

namespace {

constexpr uint32_t kPortmapProg = 100000;
constexpr uint32_t kPortmapVers = 2;
constexpr uint32_t kPortmapGetPort = 3;
constexpr uint32_t kMountProg = 100005;
constexpr uint32_t kMountVers = 3;
constexpr uint32_t kMountMnt = 1;
constexpr uint32_t kMountUmnt = 3;
constexpr uint32_t kNfsProg = 100003;
constexpr uint32_t kNfsVers = 3;
constexpr uint32_t kGetattr = 1;
constexpr uint32_t kLookup = 3;
constexpr uint32_t kRead = 6;
constexpr uint32_t kReaddir = 16;
constexpr uint32_t kReaddirPlus = 17;
constexpr uint32_t kFsinfo = 19;
constexpr uint32_t kNotSupp = 10004;  // nfsstat3 NFS3ERR_NOTSUPP

constexpr uint32_t kMaxRecord = 1u << 20;  // a reply bigger than this is garbage
constexpr uint32_t kMaxRead = 32768;       // bytes per READ, whatever the server allows
constexpr uint32_t kListBytes = 32768;     // READDIR(PLUS) reply size we ask for

// call()'s own codes, apart from errno values: the connection failed or went quiet.
constexpr int kNetDown = -1;
constexpr int kNetTimeout = -2;

// --- XDR (RFC 4506): big-endian 32-bit words, opaque data padded to a multiple of 4 ---------

struct XdrOut {
  std::string b;
  void u32(uint32_t v) {
    const char c[4] = {static_cast<char>(v >> 24), static_cast<char>(v >> 16),
                       static_cast<char>(v >> 8), static_cast<char>(v)};
    b.append(c, 4);
  }
  void u64(uint64_t v) {
    u32(static_cast<uint32_t>(v >> 32));
    u32(static_cast<uint32_t>(v));
  }
  void fixed(const void *p, size_t n) {
    b.append(static_cast<const char *>(p), n);
    b.append((4 - n % 4) % 4, '\0');
  }
  void opaque(const void *p, size_t n) {
    u32(static_cast<uint32_t>(n));
    fixed(p, n);
  }
  void str(const std::string &s) { opaque(s.data(), s.size()); }
  void fh(const NfsClient::Fh &f) { opaque(f.data, f.len); }
};

struct XdrIn {
  const uint8_t *p;
  const uint8_t *end;
  bool ok = true;

  XdrIn(const std::string &s, size_t from)
      : p(reinterpret_cast<const uint8_t *>(s.data()) + from),
        end(reinterpret_cast<const uint8_t *>(s.data()) + s.size()) {}

  bool need(size_t n) {
    if (ok && static_cast<size_t>(end - p) >= n) return true;
    ok = false;
    return false;
  }
  uint32_t u32() {
    if (!need(4)) return 0;
    const uint32_t v = (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
                       (static_cast<uint32_t>(p[2]) << 8) | p[3];
    p += 4;
    return v;
  }
  uint64_t u64() {
    const uint64_t hi = u32();
    return (hi << 32) | u32();
  }
  bool flag() { return u32() != 0; }
  void skip(size_t n) {
    n = (n + 3) & ~static_cast<size_t>(3);
    if (need(n)) p += n;
  }
  // A variable-length opaque: its bytes, or nullptr if it's longer than max or cut short.
  const uint8_t *opaque(uint32_t &len, uint32_t max) {
    len = u32();
    if (!ok || len > max) {
      ok = false;
      return nullptr;
    }
    const uint8_t *d = p;
    skip(len);
    return ok ? d : nullptr;
  }
  std::string str(uint32_t max) {
    uint32_t n;
    const uint8_t *d = opaque(n, max);
    return d ? std::string(reinterpret_cast<const char *>(d), n) : std::string();
  }
  bool fh(NfsClient::Fh &f) {
    uint32_t n;
    const uint8_t *d = opaque(n, sizeof f.data);
    if (!d) return false;
    f.len = static_cast<uint8_t>(n);
    memcpy(f.data, d, n);
    return true;
  }
};

// fattr3
void readAttr(XdrIn &x, NfsClient::Attr &a) {
  const uint32_t type = x.u32();
  a.type = type == 1   ? NfsClient::Type::File
           : type == 2 ? NfsClient::Type::Dir
           : type == 5 ? NfsClient::Type::Link
                       : NfsClient::Type::Other;
  a.mode = x.u32();
  x.u32();  // nlink
  x.u32();  // uid
  x.u32();  // gid
  a.size = x.u64();
  x.u64();  // used
  x.u64();  // rdev
  x.u64();  // fsid
  a.fileid = x.u64();
  x.u64();  // atime
  a.mtime = x.u32();
  x.u32();  // mtime's nanoseconds
  x.u64();  // ctime
}

// post_op_attr: true if the attributes were there.
bool readPostOpAttr(XdrIn &x, NfsClient::Attr &a) {
  if (!x.flag()) return false;
  readAttr(x, a);
  return x.ok;
}

int nfsErrno(uint32_t status) {
  switch (status) {
    case 1: return EPERM;
    case 2: return ENOENT;
    case 6: return ENXIO;
    case 13: return EACCES;
    case 17: return EEXIST;
    case 20: return ENOTDIR;
    case 21: return EISDIR;
    case 22: return EINVAL;
    case 27: return EFBIG;
    case 28: return ENOSPC;
    case 30: return EROFS;
    case 63: return ENAMETOOLONG;
    case 66: return ENOTEMPTY;
    case 70:     // NFS3ERR_STALE
    case 10001:  // NFS3ERR_BADHANDLE
      return ESTALE;
    case kNotSupp: return ENOTSUP;
    case 10008: return EAGAIN;  // NFS3ERR_JUKEBOX: the file is being fetched from slow storage
    default: return EIO;
  }
}

// The socket's last error, as call()'s code.
int netError() { return errno == EAGAIN || errno == EWOULDBLOCK ? kNetTimeout : kNetDown; }

bool sendAll(int fd, const char *p, size_t n) {
#ifdef MSG_NOSIGNAL
  const int flags = MSG_NOSIGNAL;
#else
  const int flags = 0;
#endif
  while (n > 0) {
    const ssize_t k = send(fd, p, n, flags);
    if (k <= 0) {
      if (k < 0 && errno == EINTR) continue;
      return false;
    }
    p += k;
    n -= static_cast<size_t>(k);
  }
  return true;
}

bool recvAll(int fd, char *p, size_t n) {
  while (n > 0) {
    const ssize_t k = recv(fd, p, n, 0);
    if (k <= 0) {
      if (k < 0 && errno == EINTR) continue;
      if (k == 0) errno = ECONNRESET;
      return false;
    }
    p += k;
    n -= static_cast<size_t>(k);
  }
  return true;
}

// Binds fd to a free port below 1024. The ports rotate, so a quick reconnect doesn't meet the
// last connection's TIME_WAIT. False where the OS won't allow it (a desktop without root).
bool bindReserved(int fd) {
  static uint16_t next = 0;
  if (next == 0) next = static_cast<uint16_t>(665 + nfs_random() % 358);  // 665-1022
  for (int tries = 0; tries < 64; ++tries) {
    const uint16_t port = next;
    next = next <= 665 ? 1022 : next - 1;
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, reinterpret_cast<sockaddr *>(&a), sizeof a) == 0) return true;
    if (errno == EACCES || errno == EPERM) break;
  }
  log_w("nfs: no reserved port to call from; a `secure` export will refuse");
  return false;
}

}  // namespace

// --- Connections and RPC ---------------------------------------------------------------------

bool NfsClient::resolve(std::string &error) {
  addrinfo hints = {};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo *res = nullptr;
  if (getaddrinfo(opt_.host.c_str(), nullptr, &hints, &res) != 0 || !res) {
    log_w("nfs: can't resolve %s", opt_.host.c_str());
    error = "SERVER NOT FOUND";
    return false;
  }
  ip_ = reinterpret_cast<const sockaddr_in *>(res->ai_addr)->sin_addr.s_addr;
  freeaddrinfo(res);
  return true;
}

int NfsClient::connectTo(uint16_t port, bool reserved, uint32_t timeoutMs) {
  const int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd < 0) return -1;
  if (reserved) bindReserved(fd);
  sockaddr_in a = {};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = ip_;
  a.sin_port = htons(port);
  const int flags = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  int rc = ::connect(fd, reinterpret_cast<sockaddr *>(&a), sizeof a);
  if (rc != 0 && errno == EINPROGRESS) {
    fd_set w;
    FD_ZERO(&w);
    FD_SET(fd, &w);
    timeval tv = {static_cast<time_t>(timeoutMs / 1000),
                  static_cast<suseconds_t>(timeoutMs % 1000 * 1000)};
    rc = select(fd + 1, nullptr, &w, nullptr, &tv);
    if (rc == 1) {
      int err = 0;
      socklen_t len = sizeof err;
      getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
      rc = err == 0 ? 0 : -1;
      errno = err;
    } else {
      if (rc == 0) errno = ETIMEDOUT;
      rc = -1;
    }
  }
  if (rc != 0) {
    const int err = errno;
    log_w("nfs: can't connect to %s:%u (errno %d)", inet_ntoa(a.sin_addr), port, err);
    close(fd);
    errno = err;
    return -1;
  }
  fcntl(fd, F_SETFL, flags);
  timeval tv = {static_cast<time_t>(timeoutMs / 1000),
                static_cast<suseconds_t>(timeoutMs % 1000 * 1000)};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
  const int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#ifdef SO_NOSIGPIPE
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
  return fd;
}

int NfsClient::call(int fd, uint32_t prog, uint32_t vers, uint32_t proc, const std::string &args,
                    size_t &bodyAt) {
  const uint32_t xid = ++xid_;
  XdrOut h;
  h.u32(0);  // record mark, filled in below
  h.u32(xid);
  h.u32(0);  // CALL
  h.u32(2);  // RPC version
  h.u32(prog);
  h.u32(vers);
  h.u32(proc);
  if (prog == kPortmapProg) {
    h.u32(0);  // AUTH_NONE
    h.u32(0);
  } else {
    XdrOut cred;  // AUTH_SYS: stamp, machine name, uid, gid, extra gids
    cred.u32(0);
    cred.str("inkdeck");
    cred.u32(opt_.uid);
    cred.u32(opt_.gid);
    cred.u32(1);
    cred.u32(opt_.gid);
    h.u32(1);
    h.opaque(cred.b.data(), cred.b.size());
  }
  h.u32(0);  // verifier: AUTH_NONE
  h.u32(0);
  h.b += args;
  const uint32_t mark = static_cast<uint32_t>(h.b.size() - 4) | 0x80000000u;  // one fragment
  for (int i = 0; i < 4; ++i) h.b[i] = static_cast<char>(mark >> (24 - 8 * i));
  if (!sendAll(fd, h.b.data(), h.b.size())) return netError();

  // A late reply to an earlier call that timed out can come first: skip those.
  for (int tries = 0; tries < 4; ++tries) {
    reply_.clear();
    bool last = false;
    while (!last) {
      uint8_t m[4];
      if (!recvAll(fd, reinterpret_cast<char *>(m), 4)) return netError();
      const uint32_t frag = (static_cast<uint32_t>(m[0]) << 24) |
                            (static_cast<uint32_t>(m[1]) << 16) |
                            (static_cast<uint32_t>(m[2]) << 8) | m[3];
      last = (frag & 0x80000000u) != 0;
      const uint32_t n = frag & 0x7fffffffu;
      if (reply_.size() + n > kMaxRecord) return kNetDown;
      const size_t at = reply_.size();
      reply_.resize(at + n);
      if (!recvAll(fd, &reply_[at], n)) return netError();
    }
    XdrIn x(reply_, 0);
    if (x.u32() != xid) continue;
    if (x.u32() != 1) return kNetDown;  // not a REPLY
    if (x.u32() != 0) {                 // MSG_DENIED
      const uint32_t why = x.u32();
      log_w("nfs: call denied (%s)", why == 1 ? "auth" : "RPC version");
      return why == 1 ? EACCES : EPROTONOSUPPORT;
    }
    x.u32();  // verifier flavour
    uint32_t n;
    x.opaque(n, 400);
    const uint32_t accept = x.u32();
    if (!x.ok) return kNetDown;
    if (accept != 0) {
      log_w("nfs: program %u v%u proc %u not accepted (%u)", static_cast<unsigned>(prog),
            static_cast<unsigned>(vers), static_cast<unsigned>(proc),
            static_cast<unsigned>(accept));
      // PROG_UNAVAIL, PROG_MISMATCH: no such service or version; PROC_UNAVAIL: no such call.
      return accept == 1 || accept == 2 ? EPROTONOSUPPORT : accept == 3 ? ENOTSUP : EIO;
    }
    bodyAt = static_cast<size_t>(x.p - reinterpret_cast<const uint8_t *>(reply_.data()));
    return 0;
  }
  return kNetDown;
}

void NfsClient::closeNfs() {
  if (nfsFd_ >= 0) close(nfsFd_);
  nfsFd_ = -1;
}

int NfsClient::nfsCall(uint32_t proc, const std::string &args, size_t &bodyAt) {
  if (!mounted_) return ENOTCONN;
  for (int attempt = 0; attempt < 2; ++attempt) {
    if (nfsFd_ < 0) {
      nfsFd_ = connectTo(opt_.nfsPort, true, opt_.timeoutMs);
      if (nfsFd_ < 0) return errno == ETIMEDOUT ? ETIMEDOUT : EIO;
    }
    const int rc = call(nfsFd_, kNfsProg, kNfsVers, proc, args, bodyAt);
    if (rc >= 0) return rc;
    // The connection is in an unknown state. A server closes idle ones after a few minutes,
    // which shows as kNetDown on the next call: that's worth one more try on a new one.
    closeNfs();
    if (rc == kNetTimeout) return ETIMEDOUT;
  }
  return EIO;
}

int NfsClient::getPort(int fd, uint32_t prog, uint32_t vers, uint16_t &port) {
  XdrOut a;
  a.u32(prog);
  a.u32(vers);
  a.u32(IPPROTO_TCP);
  a.u32(0);
  size_t at;
  const int rc = call(fd, kPortmapProg, kPortmapVers, kPortmapGetPort, a.b, at);
  if (rc != 0) return rc;
  XdrIn x(reply_, at);
  const uint32_t p = x.u32();
  if (!x.ok || p > 65535) return EIO;
  port = static_cast<uint16_t>(p);
  return 0;
}

// --- Mounting --------------------------------------------------------------------------------

bool NfsClient::mount(const Options &options, std::string &error) {
  unmount();
  opt_ = options;
  if (opt_.exportPath.empty()) opt_.exportPath = "/";
  if (!resolve(error)) return false;

  // The portmapper says where MOUNT and NFS listen. Without one, NFS is on 2049, and MOUNT is
  // tried on the same port (some small servers serve both there).
  uint16_t nfsPort = opt_.nfsPort;
  uint16_t mountPort = opt_.mountPort;
  if (nfsPort == 0 || mountPort == 0) {
    const int fd = connectTo(111, false, opt_.timeoutMs);
    if (fd >= 0) {
      uint16_t p = 0;
      if (nfsPort == 0 && getPort(fd, kNfsProg, kNfsVers, p) == 0) {
        if (p == 0) {
          close(fd);
          log_w("nfs: %s has no NFS v3 over TCP", opt_.host.c_str());
          error = "SERVER HAS NO NFS V3";
          return false;
        }
        nfsPort = p;
      }
      p = 0;
      if (mountPort == 0 && getPort(fd, kMountProg, kMountVers, p) == 0) mountPort = p;
      close(fd);
    } else if (errno == ETIMEDOUT) {
      error = "NO ANSWER FROM THE SERVER";
      return false;
    }
  }
  if (nfsPort == 0) nfsPort = 2049;
  if (mountPort == 0) mountPort = nfsPort;
  opt_.nfsPort = nfsPort;
  opt_.mountPort = mountPort;

  const int fd = connectTo(mountPort, true, opt_.timeoutMs);
  if (fd < 0) {
    error = errno == ETIMEDOUT ? "NO ANSWER FROM THE SERVER" : "SERVER REFUSED THE CONNECTION";
    return false;
  }
  XdrOut a;
  a.str(opt_.exportPath);
  size_t at;
  int rc = call(fd, kMountProg, kMountVers, kMountMnt, a.b, at);
  close(fd);
  if (rc != 0) {
    error = rc == kNetTimeout       ? "NO ANSWER FROM THE SERVER"
            : rc == EACCES          ? "ACCESS DENIED"
            : rc == EPROTONOSUPPORT ? "SERVER HAS NO NFS V3"
                                    : "MOUNT FAILED";
    return false;
  }
  XdrIn x(reply_, at);
  const uint32_t status = x.u32();
  if (status != 0) {
    log_w("nfs: mounting %s:%s: status %u", opt_.host.c_str(), opt_.exportPath.c_str(),
          static_cast<unsigned>(status));
    error = status == 2                   ? "NO SUCH EXPORT"
            : status == 1 || status == 13 ? "ACCESS DENIED"
            : status == 20                ? "EXPORT ISN'T A FOLDER"
                                          : "MOUNT FAILED (" + std::to_string(status) + ")";
    return false;
  }
  if (!x.fh(root_)) {
    error = "BAD REPLY FROM THE SERVER";
    return false;
  }
  // The security flavours the export takes. AUTH_SYS (1) or AUTH_NONE (0) will do; an export
  // that only takes Kerberos won't.
  const uint32_t flavours = x.u32();
  bool usable = flavours == 0 || !x.ok;
  for (uint32_t i = 0; i < flavours && i < 16 && x.ok; ++i) {
    const uint32_t f = x.u32();
    if (f == 0 || f == 1) usable = true;
  }
  if (!usable) {
    error = "EXPORT NEEDS KERBEROS";
    return false;
  }
  mounted_ = true;

  // FSINFO: how much one READ may carry.
  XdrOut f;
  f.fh(root_);
  rc = nfsCall(kFsinfo, f.b, at);
  if (rc == ETIMEDOUT || rc == EIO) {
    mounted_ = false;
    closeNfs();
    error = rc == ETIMEDOUT ? "NO ANSWER FROM THE SERVER" : "CAN'T REACH NFS ON THE SERVER";
    return false;
  }
  if (rc == 0) {
    XdrIn y(reply_, at);
    if (y.u32() == 0) {
      Attr ignored;
      readPostOpAttr(y, ignored);
      const uint32_t rtmax = y.u32();
      const uint32_t rtpref = y.u32();
      uint32_t size = rtpref ? rtpref : rtmax;
      if (rtmax && size > rtmax) size = rtmax;
      if (size > kMaxRead || size == 0) size = kMaxRead;
      if (size < 4096) size = 4096;
      if (y.ok) readSize_ = size;
    }
  }
  log_i("nfs: mounted %s:%s (nfs port %u, mount port %u, reads of %u bytes)", opt_.host.c_str(),
        opt_.exportPath.c_str(), nfsPort, mountPort, static_cast<unsigned>(readSize_));
  return true;
}

void NfsClient::unmount() {
  closeNfs();
  if (!mounted_) return;
  mounted_ = false;
  // UMNT only tidies the server's list of clients: a short try, and no matter if it fails.
  const int fd = connectTo(opt_.mountPort, true, 2000);
  if (fd < 0) return;
  XdrOut a;
  a.str(opt_.exportPath);
  size_t at;
  call(fd, kMountProg, kMountVers, kMountUmnt, a.b, at);
  close(fd);
}

// --- NFS calls -------------------------------------------------------------------------------

int NfsClient::getattr(const Fh &fh, Attr &out) {
  XdrOut a;
  a.fh(fh);
  size_t at;
  const int rc = nfsCall(kGetattr, a.b, at);
  if (rc != 0) return rc;
  XdrIn x(reply_, at);
  const uint32_t status = x.u32();
  if (status != 0) return nfsErrno(status);
  readAttr(x, out);
  return x.ok ? 0 : EIO;
}

int NfsClient::lookup(const Fh &dir, const std::string &name, Fh &out, Attr &attr) {
  XdrOut a;
  a.fh(dir);
  a.str(name);
  size_t at;
  const int rc = nfsCall(kLookup, a.b, at);
  if (rc != 0) return rc;
  XdrIn x(reply_, at);
  const uint32_t status = x.u32();
  if (status != 0) return nfsErrno(status);
  if (!x.fh(out)) return EIO;
  const bool hasAttr = readPostOpAttr(x, attr);
  if (!x.ok) return EIO;
  return hasAttr ? 0 : getattr(out, attr);
}

int NfsClient::readdir(const Fh &dir, std::vector<Entry> &out, size_t maxEntries) {
  out.clear();
  uint64_t cookie = 0;
  uint8_t verf[8] = {};
  for (;;) {
    XdrOut a;
    a.fh(dir);
    a.u64(cookie);
    a.fixed(verf, 8);
    a.u32(kListBytes / 4);  // dircount: the names and cookies alone
    a.u32(kListBytes);      // maxcount: the whole reply
    size_t at;
    const int rc = nfsCall(kReaddirPlus, a.b, at);
    if (rc == ENOTSUP && cookie == 0) return readdirPlain(dir, out, maxEntries);
    if (rc != 0) return rc;
    XdrIn x(reply_, at);
    const uint32_t status = x.u32();
    if (status == kNotSupp && cookie == 0) return readdirPlain(dir, out, maxEntries);
    if (status != 0) return nfsErrno(status);
    Attr dirAttr;
    readPostOpAttr(x, dirAttr);
    if (!x.need(8)) return EIO;
    memcpy(verf, x.p, 8);
    x.p += 8;
    int onPage = 0;
    while (x.flag()) {
      Entry e;
      x.u64();  // fileid
      e.name = x.str(255);
      cookie = x.u64();
      e.hasAttr = readPostOpAttr(x, e.attr);
      if (x.flag()) e.hasFh = x.fh(e.fh);
      if (!x.ok) return EIO;
      ++onPage;
      if (e.name == "." || e.name == "..") continue;
      if (out.size() < maxEntries) out.push_back(std::move(e));
    }
    const bool eof = x.flag();
    if (!x.ok) return EIO;
    if (eof || out.size() >= maxEntries) return 0;
    if (onPage == 0) return EIO;  // no progress: the server sent an empty page that isn't the end
  }
}

// Plain READDIR, for a server without READDIRPLUS: names only, then a LOOKUP each for what
// they are.
int NfsClient::readdirPlain(const Fh &dir, std::vector<Entry> &out, size_t maxEntries) {
  out.clear();
  uint64_t cookie = 0;
  uint8_t verf[8] = {};
  for (bool eof = false; !eof && out.size() < maxEntries;) {
    XdrOut a;
    a.fh(dir);
    a.u64(cookie);
    a.fixed(verf, 8);
    a.u32(kListBytes);
    size_t at;
    const int rc = nfsCall(kReaddir, a.b, at);
    if (rc != 0) return rc;
    XdrIn x(reply_, at);
    const uint32_t status = x.u32();
    if (status != 0) return nfsErrno(status);
    Attr dirAttr;
    readPostOpAttr(x, dirAttr);
    if (!x.need(8)) return EIO;
    memcpy(verf, x.p, 8);
    x.p += 8;
    int onPage = 0;
    while (x.flag()) {
      Entry e;
      x.u64();  // fileid
      e.name = x.str(255);
      cookie = x.u64();
      if (!x.ok) return EIO;
      ++onPage;
      if (e.name == "." || e.name == "..") continue;
      if (out.size() < maxEntries) out.push_back(std::move(e));
    }
    eof = x.flag();
    if (!x.ok) return EIO;
    if (!eof && onPage == 0) return EIO;
  }
  for (Entry &e : out) {
    const int rc = lookup(dir, e.name, e.fh, e.attr);
    if (rc == ETIMEDOUT || rc == EIO) return rc;
    e.hasFh = e.hasAttr = rc == 0;
  }
  return 0;
}

int NfsClient::read(const Fh &fh, uint64_t offset, uint32_t count, uint8_t *dst, uint32_t &got,
                    bool &eof) {
  got = 0;
  eof = false;
  if (count > readSize_) count = readSize_;
  XdrOut a;
  a.fh(fh);
  a.u64(offset);
  a.u32(count);
  size_t at;
  const int rc = nfsCall(kRead, a.b, at);
  if (rc != 0) return rc;
  XdrIn x(reply_, at);
  const uint32_t status = x.u32();
  if (status != 0) return nfsErrno(status);
  Attr fileAttr;
  readPostOpAttr(x, fileAttr);
  x.u32();  // count, again
  eof = x.flag();
  uint32_t n;
  const uint8_t *data = x.opaque(n, count);
  if (!data) return EIO;
  memcpy(dst, data, n);
  got = n;
  return 0;
}
