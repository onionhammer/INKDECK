// A small NFSv3 client over TCP (RFC 1813; ONC RPC, RFC 5531): enough to list folders and read
// files on a share. AUTH_SYS credentials (uid/gid, no password: NFS trusts the client's word).
// Ports come from the server's portmapper unless given. Calls go out from a reserved local port
// (below 1024), because Linux exports default to `secure`, which turns other ports away.
//
// Plain POSIX sockets, so it also builds on a desktop for testing against a real server. Not
// thread-safe: one caller at a time (nfs.cpp holds a lock around it).
#pragma once
#include <stdint.h>

#include <string>
#include <vector>

class NfsClient {
 public:
  struct Options {
    std::string host;        // name or IPv4 address
    std::string exportPath;  // "/volume1/books"
    uint32_t uid = 0;
    uint32_t gid = 0;
    uint16_t nfsPort = 0;    // 0: ask the portmapper (else 2049)
    uint16_t mountPort = 0;  // 0: ask the portmapper (else the NFS port)
    uint32_t timeoutMs = 8000;
  };

  // A file handle: opaque to the client, up to 64 bytes in NFSv3.
  struct Fh {
    uint8_t len = 0;
    uint8_t data[64] = {};
  };

  enum class Type : uint8_t { Other, File, Dir, Link };

  struct Attr {
    Type type = Type::Other;
    uint32_t mode = 0;  // permission bits
    uint64_t size = 0;
    uint64_t fileid = 0;
    uint32_t mtime = 0;  // seconds since 1970
  };

  struct Entry {
    std::string name;
    Fh fh;
    Attr attr;
    bool hasFh = false;  // READDIRPLUS may leave either out; lookup() fills them in
    bool hasAttr = false;
  };

  NfsClient() = default;
  ~NfsClient() { unmount(); }
  NfsClient(const NfsClient &) = delete;
  NfsClient &operator=(const NfsClient &) = delete;

  // Finds the ports, mounts the export (MOUNT v3) and connects to the NFS service. On failure
  // error says why in a few words, for the screen ("SERVER NOT FOUND", "ACCESS DENIED").
  bool mount(const Options &options, std::string &error);
  // Tells the server (best effort, briefly) and closes the connection.
  void unmount();
  bool mounted() const { return mounted_; }

  const Fh &root() const { return root_; }
  uint32_t readSize() const { return readSize_; }  // bytes per READ, from the server's FSINFO

  // Each returns 0 or an errno value (ENOENT, EACCES, ESTALE, EIO, ETIMEDOUT...). A dropped
  // connection is opened again once per call: these calls are all idempotent.
  int lookup(const Fh &dir, const std::string &name, Fh &out, Attr &attr);
  int getattr(const Fh &fh, Attr &out);
  // The folder's entries, "." and ".." left out, at most maxEntries of them.
  int readdir(const Fh &dir, std::vector<Entry> &out, size_t maxEntries);
  // Up to count bytes (no more than readSize()) from offset into dst.
  int read(const Fh &fh, uint64_t offset, uint32_t count, uint8_t *dst, uint32_t &got,
           bool &eof);

 private:
  // One RPC on socket fd: sends the call and waits for its reply, which is left in reply_ with
  // the result from bodyAt on. 0, an errno value from the server's answer, or (below 0) the
  // connection failed or timed out.
  int call(int fd, uint32_t prog, uint32_t vers, uint32_t proc, const std::string &args,
           size_t &bodyAt);
  // An NFS call on the kept connection, reconnecting once if it has dropped. 0 or an errno.
  int nfsCall(uint32_t proc, const std::string &args, size_t &bodyAt);
  int connectTo(uint16_t port, bool reserved, uint32_t timeoutMs);
  bool resolve(std::string &error);
  int getPort(int fd, uint32_t prog, uint32_t vers, uint16_t &port);
  // The directory a page at a time, without . and .., up to maxEntries: READDIRPLUS (plus:
  // attributes and handles too) or READDIR (names only). ENOTSUP from a server without
  // READDIRPLUS.
  int listPages(const Fh &dir, bool plus, std::vector<Entry> &out, size_t maxEntries);
  int readdirPlain(const Fh &dir, std::vector<Entry> &out, size_t maxEntries);
  void closeNfs();

  Options opt_;
  uint32_t ip_ = 0;  // the server's IPv4 address, network byte order
  int nfsFd_ = -1;
  bool mounted_ = false;
  Fh root_;
  uint32_t readSize_ = 32768;
  uint32_t xid_ = 0;
  std::string reply_;  // the last reply record, kept to save reallocating
};
