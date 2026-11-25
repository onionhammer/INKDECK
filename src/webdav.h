// The microSD card as a network drive: a WebDAV server on port 80 that the computer mounts like a
// disk (Finder: Go > Connect to Server, http://inkdeck.local; Windows: Map network drive; Linux:
// davfs2 or the file manager's dav://). System > WI-FI DRIVE opens its screen (apps/wifi_drive.h),
// which calls start(): the board joins the network, announces _webdav._tcp over mDNS and serves
// until stop(), or until kIdleMinutes pass with no file read or written and no key pressed.
//
// The board keeps the card mounted, so nothing restarts: the server works on it through FatFs
// (directly, on SD_MMC's drive, so a folder lists with its sizes and dates in one pass), alongside
// the rest of the firmware. The computer sees folders and files, never the FAT, so a big card
// mounts at once. As a USB drive (mass storage at full speed, about 0.85 MB/s) it couldn't: macOS
// reads the whole FAT before it mounts a FAT32 volume and gives up after 20 s, and a 256 GB card's
// FAT is 30 MB.
//
// Speed: about 0.5 MB/s each way on a -70 dBm link (2026-11-17). The network sets it: the
// framework's lwIP is built with a 5.7 KB TCP window and send buffer (no per-socket override), so
// each connection waits on the round trip. The card reads at about 2.8 MB/s and writes at 1.8
// through these buffers.
//
// What clients need, and get:
//   - Class 2 (LOCK and UNLOCK, advisory only): macOS mounts a class 1 server read-only.
//   - Chunked request bodies: Finder sends its PUTs that way (ESP-IDF's httpd refuses them).
//   - Digest sign-in (MD5, qop=auth) as user kUser with password(): no password crosses the
//     network in the clear, and Windows refuses Basic over plain HTTP. OPTIONS needs none.
//   - PROPFIND at depth 0 and 1 (infinity counts as 1), with the card's free space (quota
//     properties) for every client but macOS: told it, the Mac's mount hangs 90 s in the kernel
//     (fromMacMount() in webdav.cpp), so Finder shows the free space as unknown.
//   - GET with a byte range, PUT, DELETE (folders with everything in them), MKCOL, COPY and MOVE
//     (Overwrite honoured), PROPPATCH (accepted, nothing stored). A PUT that breaks off leaves no
//     file behind.
// Up to kMaxClients connections at once (Finder keeps several open), each on its own task with
// its stack and a 32 KB transfer buffer in PSRAM: with Wi-Fi up, about 55 KB of internal RAM is
// left, and its largest block is under 32 KB.
#pragma once
#include <stdint.h>

#include <string>

namespace webdav {

constexpr const char *kHostname = "inkdeck";  // mDNS: inkdeck.local
constexpr uint16_t kPort = 80;
constexpr const char *kUser = "inkdeck";
constexpr uint32_t kIdleMinutes = 10;
constexpr int kMaxClients = 8;

enum class Phase : uint8_t {
  Idle,     // not serving (never started, or stopped)
  Joining,  // joining Wi-Fi
  Serving,  // listening on kPort
  Failed,   // gave up: Wi-Fi, the socket, or kIdleMinutes without use; result says which
};

struct Status {
  Phase phase = Phase::Idle;
  std::string ip;      // once joined: "192.168.1.42"
  std::string result;  // Failed: why ("WI-FI: NETWORK NOT FOUND", "STOPPED AFTER 10 MIN IDLE")
  // What the computer last changed, for the screen: "wrote Books/Dune.epub", "deleted Old".
  // Finder's own files (.DS_Store, ._*) don't count.
  std::string lastChange;
  uint64_t readBytes = 0;     // file contents sent, since start()
  uint64_t writtenBytes = 0;  // file contents received
  uint32_t msSinceRead = UINT32_MAX;  // UINT32_MAX: none yet
  uint32_t msSinceWrite = UINT32_MAX;
  uint32_t secondsLeft = 0;   // Serving: until the idle stop
  int clients = 0;            // connections open now
  uint32_t badSignIns = 0;    // wrong user or password, since start()
};

bool configured();  // a network to join (wlan.h)

// The sign-in password: generated on first use (12 letters and digits in groups of four, about
// 59 bits) and kept in NVS, so the computer can remember it. UI task (it may write NVS).
std::string password();

// Starts serving on a background task; if it's already running, it carries on (and a stop()
// still pending is withdrawn). False if the task couldn't start. UI task.
bool start();
// Stops serving and turns the radio off (unless something else uses it). Transfers under way are
// cut off; the computer sees the server go.
void stop();

Status status();
bool active();  // Joining or Serving: keeps the board on (power.h)

// What the computer changed that the firmware reads only at boot: SD:/inkdeck.ini (settings.h)
// and SD:/Podcasts (podcast_library.h). Cleared by start().
bool changedSettings();
bool changedPodcasts();

}  // namespace webdav
