// NFS shares on the network, which Files lists after ONBOARD FLASH on the card's root. Each
// `nfs =` line in SD:/inkdeck.ini (settings.h) is one share, up to kMaxShares:
//
//   nfs = nfs://nas.local/volume1/books
//   nfs = nfs://192.168.1.20/srv/media?name=Media&uid=1000&gid=1000
//   nfs = nas.local:/volume1/books        ; mount's server:/export works too
//
// Options after the ?, joined by & (libnfs's names): name (the share's name in Files, else the
// export's last folder), uid and gid (AUTH_SYS: who the server takes us for; default 0),
// nfsport and mountport (else the server's portmapper says; a port in the URL,
// nfs://host:2049/..., is the NFS port).
//
// NFS version 3 over TCP (nfsclient.h), read-only. A mounted share is a file system at
// /nfs/<name> (an ESP-IDF VFS), so DirList, the picture viewer, the readers and the player open
// its files as they open the card's. Wi-Fi is on while a share is mounted: from the first
// acquire() until the last user is done (a Files screen on it, an open file: the player's
// track, a book in the reader), plus kIdleMs, so the next track or picture needn't reconnect.
// Modem sleep is off meanwhile (as for updates over Wi-Fi): it costs power, but every READ
// would otherwise wait for the access point's next beacon.
#pragma once
#include <stdint.h>

#include <memory>
#include <string>
#include <vector>

namespace nfs {

constexpr const char *kMountPoint = "/nfs";
constexpr int kMaxShares = 4;
constexpr uint32_t kIdleMs = 30000;

struct Share {
  std::string name;        // unique among the shares; the folder under /nfs
  std::string host;
  std::string exportPath;  // "/volume1/books"
  uint32_t uid = 0;
  uint32_t gid = 0;
  uint16_t nfsPort = 0;    // 0: ask the portmapper
  uint16_t mountPort = 0;

  std::string path() const { return std::string(kMountPoint) + "/" + name; }
  std::string url() const { return "nfs://" + host + exportPath; }  // for logs and the screen
};

// Reads one `nfs =` value. False, with a few words in error, if it isn't a share.
bool parse(const std::string &spec, Share &out, std::string &error);

// The shares SD:/inkdeck.ini lists, as read at boot.
const std::vector<Share> &shares();
// The share whose path() holds path, or -1.
int shareOf(const std::string &path);

// Joins Wi-Fi and mounts share i, or counts one more user if it's mounted already. Blocks for
// up to half a minute: call from a background task (or use Lease). On failure error says why
// in a few words ("WI-FI: NETWORK NOT FOUND", "NO SUCH EXPORT").
bool acquire(int i, std::string &error);
// One more user of share i if it's mounted (no waiting); false if it isn't.
bool acquireIfMounted(int i);
// The user is done. The share is unmounted, and Wi-Fi let go, kIdleMs after the last one.
void release(int i);
bool mounted(int i);

// A screen's hold on a share: start() mounts it in a background task, the screen polls
// state(), and the hold is let go with the Lease (even while the mount is still going).
class Lease {
 public:
  enum class State : uint8_t { Idle, Connecting, Ready, Failed };

  explicit Lease(int share) : share_(share) {}
  ~Lease();
  Lease(const Lease &) = delete;
  Lease &operator=(const Lease &) = delete;

  void start();  // Ready at once if the share is already mounted
  State state() const;
  std::string error() const;  // after Failed
  int share() const { return share_; }

 private:
  struct Job;
  static void run(void *job);  // the mounting task

  int share_;
  mutable bool held_ = false;  // this lease counts as one of the share's users
  mutable std::shared_ptr<Job> job_;
};

}  // namespace nfs
