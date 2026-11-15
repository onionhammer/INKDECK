// NFS shares (shares.h): NFSv3 through nfsclient.h. NFS works with file handles, not paths, so
// this keeps the handle each path resolved to (and its attributes) for a minute, filled from
// READDIRPLUS listings, and walks a path from the deepest folder it knows with LOOKUPs.
#include <Arduino.h>
#include <errno.h>
#include <strings.h>

#include <map>
#include <new>

#include "nfsclient.h"
#include "share_backend.h"

namespace shares {
namespace {

constexpr uint32_t kHandleMs = 60000;  // how long a looked-up handle is trusted
constexpr size_t kHandleMax = 3000;

Type typeOf(NfsClient::Type t) {
  switch (t) {
    case NfsClient::Type::File: return Type::File;
    case NfsClient::Type::Dir: return Type::Dir;
    case NfsClient::Type::Link: return Type::Link;
    case NfsClient::Type::Other: break;
  }
  return Type::Other;
}

Attr attrOf(const NfsClient::Attr &a) {
  Attr out;
  out.type = typeOf(a.type);
  out.mode = a.mode;
  out.size = a.size;
  out.mtime = a.mtime;
  out.fileid = a.fileid;
  return out;
}

class NfsBackend : public Backend {
 public:
  bool mount(const Share &share, const std::string &, std::string &error) override {
    NfsClient::Options o;
    o.host = share.host;
    o.exportPath = share.path;
    o.uid = share.uid;
    o.gid = share.gid;
    o.nfsPort = share.port;
    o.mountPort = share.mountPort;
    return client_.mount(o, error);
  }

  int stat(const std::string &path, Attr &out) override {
    Node n;
    const int rc = resolveFresh(path, n);
    if (rc == 0) out = attrOf(n.attr);
    return rc;
  }

  int list(const std::string &path, std::vector<DirEntry> &out, size_t max) override {
    Node dir;
    int rc = resolveFresh(path, dir);
    if (rc == 0 && dir.attr.type != NfsClient::Type::Dir) rc = ENOTDIR;
    std::vector<NfsClient::Entry> entries;
    if (rc == 0) rc = client_.readdir(dir.fh, entries, max);
    if (rc == ESTALE) {
      handles_.clear();
      rc = resolve(path, dir);
      if (rc == 0) rc = client_.readdir(dir.fh, entries, max);
    }
    if (rc != 0) return rc;

    // Keep the handles READDIRPLUS brought; entries it left bare get a LOOKUP.
    const uint32_t now = millis();
    for (auto it = handles_.begin(); it != handles_.end();) {
      it = now - it->second.at > kHandleMs ? handles_.erase(it) : std::next(it);
    }
    if (handles_.size() + entries.size() > kHandleMax) handles_.clear();
    const std::string prefix = path.empty() ? std::string() : path + "/";
    out.clear();
    out.reserve(entries.size());
    for (NfsClient::Entry &e : entries) {
      if (!e.hasFh || !e.hasAttr) {
        e.hasFh = e.hasAttr = client_.lookup(dir.fh, e.name, e.fh, e.attr) == 0;
      }
      if (!e.hasFh || !e.hasAttr) continue;
      handles_[prefix + e.name] = Node{e.fh, e.attr, now};
      out.push_back({e.name, attrOf(e.attr)});
    }
    return 0;
  }

  int open(const std::string &path, void *&handle, Attr &attr) override {
    // Fresh attributes: the size from a listing can be a minute old, and SEEK_END goes by it.
    Node n;
    int rc = resolveFresh(path, n);
    if (rc == 0) rc = client_.getattr(n.fh, n.attr);
    if (rc == ESTALE) {
      handles_.clear();
      rc = resolve(path, n);
    }
    if (rc != 0) return rc;
    auto *fh = new (std::nothrow) NfsClient::Fh(n.fh);
    if (!fh) return ENOMEM;
    handle = fh;
    attr = attrOf(n.attr);
    return 0;
  }

  int read(void *handle, uint64_t offset, uint32_t count, uint8_t *dst, uint32_t &got,
           bool &eof) override {
    return client_.read(*static_cast<NfsClient::Fh *>(handle), offset, count, dst, got, eof);
  }

  void close(void *handle) override { delete static_cast<NfsClient::Fh *>(handle); }

  uint32_t readSize() const override { return client_.readSize(); }

 private:
  struct Node {
    NfsClient::Fh fh;
    NfsClient::Attr attr;
    uint32_t at = 0;  // millis() when looked up
  };

  // What path names: from the cache, or by LOOKUPs from the deepest folder known.
  int resolve(const std::string &path, Node &out) {
    const uint32_t now = millis();
    auto cached = [&](const std::string &key, Node &n) {
      const auto it = handles_.find(key);
      if (it == handles_.end() || now - it->second.at > kHandleMs) return false;
      n = it->second;
      return true;
    };
    Node cur;
    if (!cached("", cur)) {
      cur.fh = client_.root();
      const int rc = client_.getattr(cur.fh, cur.attr);
      if (rc != 0) return rc;
      cur.at = now;
      handles_[""] = cur;
    }
    size_t pos = 0;
    while (pos < path.size()) {
      size_t slash = path.find('/', pos);
      if (slash == std::string::npos) slash = path.size();
      const std::string key = path.substr(0, slash);
      Node next;
      if (!cached(key, next)) {
        if (cur.attr.type != NfsClient::Type::Dir) return ENOTDIR;
        const int rc = client_.lookup(cur.fh, path.substr(pos, slash - pos), next.fh, next.attr);
        if (rc != 0) return rc;
        next.at = now;
        handles_[key] = next;
      }
      cur = next;
      pos = slash + 1;
    }
    out = cur;
    return 0;
  }

  // resolve(), starting over from the root once if a cached handle went stale (the file was
  // replaced on the server).
  int resolveFresh(const std::string &path, Node &out) {
    int rc = resolve(path, out);
    if (rc == ESTALE) {
      handles_.clear();
      rc = resolve(path, out);
    }
    return rc;
  }

  NfsClient client_;
  std::map<std::string, Node> handles_;  // by path below the export ("" = its root)
};

}  // namespace

std::unique_ptr<Backend> makeNfsBackend() { return std::make_unique<NfsBackend>(); }

// nfs://host[:port]/export[?options], or host:/export[?options].
bool parseNfs(const std::string &specIn, Share &out, std::string &error) {
  std::string spec = specIn;
  std::string query;
  const size_t q = spec.find('?');
  if (q != std::string::npos) {
    query = spec.substr(q + 1);
    spec.erase(q);
  }
  std::string host, path;
  if (strncasecmp(spec.c_str(), "nfs://", 6) == 0) {
    const std::string rest = spec.substr(6);
    const size_t slash = rest.find('/');
    host = rest.substr(0, slash);
    path = slash == std::string::npos ? "/" : rest.substr(slash);
  } else {
    const size_t colon = spec.find(":/");
    if (colon == std::string::npos) {
      error = "write nfs://server/export or server:/export";
      return false;
    }
    host = spec.substr(0, colon);
    path = spec.substr(colon + 1);
  }
  const size_t colon = host.find(':');
  if (colon != std::string::npos) {
    uint32_t port;
    if (!parseNumber(host.substr(colon + 1), 65535, port) || port == 0) {
      error = "the port after the server isn't a number from 1 to 65535";
      return false;
    }
    out.port = static_cast<uint16_t>(port);
    host.erase(colon);
  }
  if (host.empty()) {
    error = "no server";
    return false;
  }
  path = percentDecode(path);
  while (path.size() > 1 && path.back() == '/') path.pop_back();
  out.host = host;
  out.path = path;

  return parseOptions(query, out, error, [&](const std::string &key, const std::string &value) {
    uint32_t n = 0;
    if (key == "uid" || key == "gid") {
      if (!parseNumber(value, 0xFFFFFFFFu, n)) {
        error = key + " must be a number";
        return false;
      }
      (key == "uid" ? out.uid : out.gid) = n;
    } else if (key == "nfsport" || key == "mountport") {
      if (!parseNumber(value, 65535, n) || n == 0) {
        error = key + " must be a number from 1 to 65535";
        return false;
      }
      (key == "nfsport" ? out.port : out.mountPort) = static_cast<uint16_t>(n);
    } else if (key == "version") {
      if (value != "3") {
        error = "only NFS version 3 is supported";
        return false;
      }
    } else {
      log_w("shares: ignoring option \"%s\" in %s", key.c_str(), specIn.c_str());
    }
    return true;
  });
}

}  // namespace shares
