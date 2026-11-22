// SMB shares (shares.h) through libsmb2's synchronous calls (lib/libsmb2). SMB works by path, so
// unlike NFS there's no handle cache here: shares.cpp keeps the listing's attributes. A dropped
// connection is made again once per call; files opened before it can't be read after it.
#include <Arduino.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include <algorithm>
#include <new>

// libsmb2's headers want the standard types first, and smb2.h before libsmb2.h.
#include <smb2/smb2.h>
#include <smb2/libsmb2.h>

#include "share_backend.h"

namespace shares {
namespace {

constexpr int kTimeoutS = 10;
constexpr uint32_t kMaxRead = 32768;  // bytes per read, whatever the server allows

Attr attrOf(const smb2_stat_64 &st) {
  Attr a;
  a.type = st.smb2_type == SMB2_TYPE_FILE        ? Type::File
           : st.smb2_type == SMB2_TYPE_DIRECTORY ? Type::Dir
           : st.smb2_type == SMB2_TYPE_LINK      ? Type::Link
                                                 : Type::Other;
  a.size = st.smb2_size;
  a.mtime = static_cast<uint32_t>(st.smb2_mtime);
  a.fileid = st.smb2_ino;
  return a;
}

// The connection's dropped (or never answered): worth a new one.
bool connectionLost(int err) {
  return err == ENOTCONN || err == ECONNRESET || err == EPIPE || err == ETIMEDOUT;
}

class SmbBackend : public Backend {
 public:
  ~SmbBackend() override { disconnect(); }

  bool mount(const Share &share, const std::string &password, std::string &error) override {
    share_ = share;
    password_ = password;
    const size_t slash = share.path.find('/');
    shareName_ = share.path.substr(0, slash);
    base_ = slash == std::string::npos ? "" : share.path.substr(slash + 1);
    if (!connect(error)) return false;
    if (!base_.empty()) {
      smb2_stat_64 st;
      if (smb2_stat(ctx_, base_.c_str(), &st) < 0 || st.smb2_type != SMB2_TYPE_DIRECTORY) {
        log_w("smb: %s has no folder %s", shareName_.c_str(), base_.c_str());
        error = "NO SUCH FOLDER IN THE SHARE";
        disconnect();
        return false;
      }
    }
    return true;
  }

  int stat(const std::string &path, Attr &out) override {
    return retry([&] {
      smb2_stat_64 st;
      const int rc = smb2_stat(ctx_, full(path).c_str(), &st);
      if (rc < 0) return -rc;
      out = attrOf(st);
      return 0;
    });
  }

  int list(const std::string &path, std::vector<DirEntry> &out, size_t max) override {
    return retry([&] {
      out.clear();
      smb2dir *dir = smb2_opendir(ctx_, full(path).c_str());
      if (!dir) return lastError();
      while (smb2dirent *e = smb2_readdir(ctx_, dir)) {
        if (strcmp(e->name, ".") == 0 || strcmp(e->name, "..") == 0) continue;
        if (out.size() >= max) break;
        out.push_back({e->name, attrOf(e->st)});
      }
      smb2_closedir(ctx_, dir);
      return 0;
    });
  }

  int open(const std::string &path, void *&handle, Attr &attr) override {
    return retry([&] {
      smb2fh *fh = smb2_open(ctx_, full(path).c_str(), O_RDONLY);
      if (!fh) return lastError();
      smb2_stat_64 st;
      const int rc = smb2_fstat(ctx_, fh, &st);
      if (rc < 0) {
        smb2_close(ctx_, fh);
        return -rc;
      }
      auto *h = new (std::nothrow) Handle{fh, generation_};
      if (!h) {
        smb2_close(ctx_, fh);
        return ENOMEM;
      }
      handle = h;
      attr = attrOf(st);
      return 0;
    });
  }

  int read(void *handle, uint64_t offset, uint32_t count, uint8_t *dst, uint32_t &got,
           bool &eof) override {
    got = 0;
    eof = false;
    const Handle *h = static_cast<Handle *>(handle);
    if (!ctx_ || h->generation != generation_) return EIO;  // opened before a reconnect
    if (count > readSize_) count = readSize_;
    const int rc = smb2_pread(ctx_, h->fh, dst, count, offset);
    if (rc < 0) {
      log_w("smb: read: %s", smb2_get_error(ctx_));
      // Reconnect for the next caller; this file's handle went with the old connection.
      if (connectionLost(-rc)) reconnect();
      return -rc;
    }
    got = static_cast<uint32_t>(rc);
    eof = got < count;
    return 0;
  }

  void close(void *handle) override {
    Handle *h = static_cast<Handle *>(handle);
    if (ctx_ && h->generation == generation_) smb2_close(ctx_, h->fh);
    delete h;
  }

  uint32_t readSize() const override { return readSize_; }

 private:
  struct Handle {
    smb2fh *fh;
    uint32_t generation;  // the connection it was opened on
  };

  // A path below the share's root, inside the URL's folder.
  std::string full(const std::string &path) const {
    if (base_.empty()) return path;
    return path.empty() ? base_ : base_ + "/" + path;
  }

  // The last failed call's errno, from its NT status (libsmb2's NULL-returning calls).
  int lastError() const {
    const int nt = smb2_get_nterror(ctx_);
    const int err = nt ? nterror_to_errno(static_cast<uint32_t>(nt)) : 0;
    return err > 0 ? err : EIO;
  }

  // op() once, and again on a new connection if the old one had dropped.
  template <typename Op>
  int retry(Op op) {
    if (!ctx_ && !reconnect()) return EIO;
    int rc = op();
    if (connectionLost(rc) && reconnect()) rc = op();
    return rc;
  }

  bool reconnect() {
    disconnect();
    ++generation_;
    std::string error;
    const bool ok = connect(error);
    log_i("smb: reconnecting to %s: %s", share_.url().c_str(), ok ? "ok" : error.c_str());
    return ok;
  }

  bool connect(std::string &error) {
    ctx_ = smb2_init_context();
    if (!ctx_) {
      error = "OUT OF MEMORY";
      return false;
    }
    smb2_set_security_mode(ctx_, SMB2_NEGOTIATE_SIGNING_ENABLED);
    smb2_set_timeout(ctx_, kTimeoutS);
    smb2_set_workstation(ctx_, "INKDECK");
    if (!share_.user.empty()) smb2_set_user(ctx_, share_.user.c_str());
    if (!share_.domain.empty()) smb2_set_domain(ctx_, share_.domain.c_str());
    smb2_set_password(ctx_, password_.c_str());
    const std::string server =
        share_.host + (share_.port ? ":" + std::to_string(share_.port) : std::string());
    const int rc = smb2_connect_share(ctx_, server.c_str(), shareName_.c_str(),
                                      share_.user.empty() ? nullptr : share_.user.c_str());
    if (rc < 0) {
      const uint32_t nt = static_cast<uint32_t>(smb2_get_nterror(ctx_));
      log_w("smb: connecting to %s: %s (errno %d, NT 0x%08lx)", share_.url().c_str(),
            smb2_get_error(ctx_), -rc, static_cast<unsigned long>(nt));
      error = describe(-rc, nt, smb2_get_error(ctx_));
      smb2_destroy_context(ctx_);
      ctx_ = nullptr;
      return false;
    }
    const uint32_t max = smb2_get_max_read_size(ctx_);
    readSize_ = std::max<uint32_t>(4096, std::min(max ? max : kMaxRead, kMaxRead));
    return true;
  }

  void disconnect() {
    if (!ctx_) return;
    smb2_disconnect_share(ctx_);
    smb2_destroy_context(ctx_);
    ctx_ = nullptr;
  }

  // A failed connect, in a few words for the screen.
  static std::string describe(int err, uint32_t nt, const char *message) {
    // libsmb2 reports a failed sign-in as ECONNREFUSED too: the NT status tells them apart.
    if (nt == SMB2_STATUS_LOGON_FAILURE || nt == SMB2_STATUS_WRONG_PASSWORD) {
      return "SIGN-IN FAILED (USER OR PASSWORD?)";
    }
    if (nt == SMB2_STATUS_BAD_NETWORK_NAME) return "NO SUCH SHARE";
    if (err == EACCES || err == EPERM) return "ACCESS DENIED";
    if (err == ECONNREFUSED) return "SERVER REFUSED THE CONNECTION";
    if (err == ETIMEDOUT) return "NO ANSWER FROM THE SERVER";
    if (err == ENOENT) return "NO SUCH SHARE";
    if (message && (strstr(message, "getaddrinfo") || strstr(message, "resolve"))) {
      return "SERVER NOT FOUND";
    }
    return "CAN'T CONNECT (ERROR " + std::to_string(err) + ")";
  }

  Share share_;
  std::string password_;
  std::string shareName_;  // the share itself
  std::string base_;       // the folder in it the URL opens, "" for its root
  smb2_context *ctx_ = nullptr;
  uint32_t generation_ = 0;
  uint32_t readSize_ = kMaxRead;
};

}  // namespace

std::unique_ptr<Backend> makeSmbBackend() { return std::make_unique<SmbBackend>(); }

// smb://[domain;][user[:password]@]host[:port]/share[/folder][?options]
bool parseSmb(const std::string &specIn, Share &out, std::string &password, std::string &error) {
  if (strncasecmp(specIn.c_str(), "smb://", 6) != 0) {
    error = "write smb://user@server/share";
    return false;
  }
  std::string spec = specIn.substr(6);
  std::string query;
  const size_t q = spec.find('?');
  if (q != std::string::npos) {
    query = spec.substr(q + 1);
    spec.erase(q);
  }
  const size_t slash = spec.find('/');
  std::string authority = spec.substr(0, slash);
  std::string path = slash == std::string::npos ? "" : percentDecode(spec.substr(slash + 1));
  while (!path.empty() && path.back() == '/') path.pop_back();
  while (!path.empty() && path.front() == '/') path.erase(0, 1);
  if (path.empty()) {
    error = "no share after the server (smb://server/share)";
    return false;
  }

  // The last @: a password with a stray @ in it still reads right.
  const size_t at = authority.rfind('@');
  if (at != std::string::npos) {
    std::string who = authority.substr(0, at);
    authority.erase(0, at + 1);
    const size_t semi = who.find(';');
    if (semi != std::string::npos) {
      out.domain = percentDecode(who.substr(0, semi));
      who.erase(0, semi + 1);
    }
    const size_t colon = who.find(':');
    out.user = percentDecode(who.substr(0, colon));
    if (colon != std::string::npos) password = percentDecode(who.substr(colon + 1));
  }
  if (!parseHostPort(authority, out, error)) return false;
  out.host = authority;
  out.path = path;
  return parseOptions(query, out, error, [&](const std::string &key, const std::string &) {
    log_w("shares: ignoring option \"%s\" in an smb line", key.c_str());
    return true;
  });
}

std::string withoutPassword(const std::string &spec) {
  if (strncasecmp(spec.c_str(), "smb://", 6) != 0) return spec;
  const size_t end = spec.find_first_of("/?", 6);
  const size_t at = spec.rfind('@', end == std::string::npos ? std::string::npos : end - 1);
  if (at == std::string::npos || at < 6) return spec;
  const size_t semi = spec.find(';', 6);
  const size_t userStart = semi != std::string::npos && semi < at ? semi + 1 : 6;
  const size_t colon = spec.find(':', userStart);
  if (colon == std::string::npos || colon > at) return spec;
  return spec.substr(0, colon) + spec.substr(at);
}

}  // namespace shares
