// Network shares, which Files lists after ONBOARD FLASH on the card's root: SMB shares (Windows,
// Samba, most NAS boxes) and NFS exports. Each `smb =` or `nfs =` line in SD:/inkdeck.ini's
// [network] section is one share, up to kMaxShares (settings.h reads them):
//
//   smb = smb://erik:PASSWORD@192.168.1.101/share/media?name=onionserve
//   smb = smb://WORKGROUP;erik@nas.local/books     ; a domain before the user, with ;
//   nfs = nfs://nas.local/volume1/books
//   nfs = nfs://192.168.1.20/srv/media?name=Media&uid=1000&gid=1000
//   nfs = nas.local:/volume1/books                 ; mount's server:/export works too
//
// SMB: SMB2 or SMB3 through libsmb2 (lib/libsmb2), signed when the server asks, with a user
// name and password (NTLMSSP). The board takes the password off the card at boot: it keeps it
// (NVS) and rewrites the line without it (settings.h). In the URL, write @ : / ? ; % in the
// password as %40 %3A %2F %3F %3B %25. A path after the share's name opens a folder inside it;
// a port goes after the server (smb://nas.local:445/...).
// NFS: version 3 over TCP (nfsclient.h), with no password: the server trusts the uid and gid it
// is given (options uid and gid, default 0). Its ports come from the server's portmapper unless
// given: nfsport (also nfs://host:2049/...) and mountport.
// Both: option name, the share's name in Files (else the last folder of its path). Options go
// after a ?, joined by &.
//
// A mounted share is a read-only file system at /net/<name> (an ESP-IDF VFS), so DirList, the
// picture viewer, the readers and the player open its files as they open the card's. Wi-Fi is
// on while a share is mounted: from the first acquire() until the last user is done (a Files
// screen on it, an open file: the player's track, a book in the reader), plus kIdleMs, so the
// next track or picture needn't reconnect. Modem sleep is off meanwhile (as for updates over
// Wi-Fi): it costs power, but every read would otherwise wait for the access point's next
// beacon.
#pragma once
#include <stdint.h>

#include <memory>
#include <string>
#include <vector>

namespace shares {

constexpr const char *kMountPoint = "/net";
constexpr int kMaxShares = 4;
constexpr uint32_t kIdleMs = 30000;

enum class Kind : uint8_t { Smb, Nfs };

struct Share {
  Kind kind = Kind::Smb;
  std::string name;  // unique among the shares; the folder under /net
  std::string host;  // name or IPv4 address
  uint16_t port = 0;  // 0: the protocol's usual (SMB 445; NFS from the portmapper, else 2049)
  // SMB: the share and any folder in it ("share/media"); NFS: the export ("/volume1/books").
  std::string path;
  std::string domain;  // SMB
  std::string user;    // SMB
  uint32_t uid = 0;    // NFS
  uint32_t gid = 0;    // NFS
  uint16_t mountPort = 0;  // NFS

  std::string mountPath() const { return std::string(kMountPoint) + "/" + name; }
  const char *tag() const { return kind == Kind::Smb ? "SMB" : "NFS"; }
  // The URL without a password, as settings.cpp keeps it and the screen shows it.
  std::string url() const;
  // Which stored password is an SMB share's: "domain;user@host/share", lower case.
  std::string credentialId() const;
};

// Reads one `smb =` or `nfs =` value. False, with a few words in error, if it isn't a share.
// password: the one in an SMB URL, if any ("" else).
bool parse(Kind kind, const std::string &spec, Share &out, std::string &password,
           std::string &error);

// An SMB URL with its password taken out ("smb://erik:pw@nas/x" -> "smb://erik@nas/x"); any
// other text as it is. What settings.cpp keeps, and writes back to the card.
std::string withoutPassword(const std::string &spec);

// The shares SD:/inkdeck.ini lists, as read at boot, names made unique.
const std::vector<Share> &all();
// The share whose mountPath() holds path, or -1.
int shareOf(const std::string &path);

// Joins Wi-Fi and mounts share i, or counts one more user if it's mounted already. Blocks for
// up to half a minute: call from a background task (or use Lease). On failure error says why
// in a few words ("WI-FI: NETWORK NOT FOUND", "ACCESS DENIED").
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

}  // namespace shares
