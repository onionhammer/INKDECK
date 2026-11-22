// The protocols behind shares.h. A backend mounts one share and answers calls by path: below the
// share's root, '/'-separated, "" for the root. shares.cpp makes one call at a time (it holds a
// lock), and each call returns 0 or an errno value.
#pragma once
#include <stdint.h>

#include <memory>
#include <string>
#include <vector>

#include "shares.h"
#include "text.h"

namespace shares {

enum class Type : uint8_t { Other, File, Dir, Link };

struct Attr {
  Type type = Type::Other;
  uint32_t mode = 0;  // permission bits; 0 where the protocol has none (SMB)
  uint64_t size = 0;
  uint32_t mtime = 0;  // seconds since 1970
  uint64_t fileid = 0;
};

struct DirEntry {
  std::string name;
  Attr attr;
};

class Backend {
 public:
  virtual ~Backend() = default;  // unmounts

  // Connects and mounts, with Wi-Fi up. On failure, error says why in a few words for the
  // screen ("ACCESS DENIED", "NO SUCH SHARE"). password: an SMB share's ("" for none).
  virtual bool mount(const Share &share, const std::string &password, std::string &error) = 0;

  virtual int stat(const std::string &path, Attr &out) = 0;
  // A folder's entries, "." and ".." left out, at most max of them.
  virtual int list(const std::string &path, std::vector<DirEntry> &out, size_t max) = 0;
  // Opens a file for reading, with fresh attributes. handle is the backend's, for read() and
  // close().
  virtual int open(const std::string &path, void *&handle, Attr &attr) = 0;
  // Up to count bytes (no more than readSize()) from offset.
  virtual int read(void *handle, uint64_t offset, uint32_t count, uint8_t *dst, uint32_t &got,
                   bool &eof) = 0;
  virtual void close(void *handle) = 0;
  virtual uint32_t readSize() const = 0;
};

std::unique_ptr<Backend> makeNfsBackend();  // nfs_backend.cpp
std::unique_ptr<Backend> makeSmbBackend();  // smb_backend.cpp

// Each protocol's URL (shares.h has the syntax): fills out's host, port, path and options.
// parseSmb() also hands over the password, if the URL has one ("" else).
bool parseNfs(const std::string &spec, Share &out, std::string &error);
bool parseSmb(const std::string &spec, Share &out, std::string &password, std::string &error);

// Shared by both parsers.
bool parseNumber(const std::string &s, uint32_t max, uint32_t &out);
using text::percentDecode;
// A server's "host:port" (the port optional, from 1 to 65535): the port into out.port and the
// host without it into host. False, with error set, if the port is wrong or there's no host.
bool parseHostPort(std::string &host, Share &out, std::string &error);
// The URL's options (after the ?) one at a time: name=value pairs joined by &, values decoded.
// Takes `name` itself; calls option(key, value) for the rest, which returns false (with error
// set) to reject the URL.
template <typename Option>
bool parseOptions(const std::string &query, Share &out, std::string &error, Option option) {
  size_t pos = 0;
  while (pos < query.size()) {
    size_t amp = query.find('&', pos);
    if (amp == std::string::npos) amp = query.size();
    const std::string item = query.substr(pos, amp - pos);
    pos = amp + 1;
    if (item.empty()) continue;
    const size_t eq = item.find('=');
    const std::string key = item.substr(0, eq);
    const std::string value = eq == std::string::npos ? "" : percentDecode(item.substr(eq + 1));
    if (key == "name") {
      out.name = value;
    } else if (!option(key, value)) {
      return false;
    }
  }
  return true;
}

}  // namespace shares
