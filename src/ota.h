// Firmware updates from GitHub. Pushing a v<major>.<minor>.<patch> tag runs the release workflow
// (.github/workflows/release.yml): it builds the firmware and attaches two files to a release,
// the image and manifest.json:
//
//   {"version": "0.2.0", "build": "1a2b3c4", "date": "2026-11-25", "size": 2351104,
//    "sha256": "<64 hex digits>", "url": "https://github.com/.../inkdeck-0.2.0.bin"}
//
// System → CHECK FOR UPDATE opens the Updater screen, which calls check(): the board joins Wi-Fi
// and fetches github.com/<kRepo>/releases/latest/download/manifest.json (GitHub's "latest"
// skips drafts and pre-releases), then compares its version with the running firmware's
// (sysinfo::firmwareVersion()). install() downloads the image over HTTPS (the core's certificate
// bundle) straight into the idle OTA slot, checks its size and SHA-256 against the manifest, and
// only then has the app image checks pass and switches the boot slot. Anything short of that
// (a lost connection, a cancel, a wrong checksum) leaves the running firmware as it was. After a
// good one the board restarts into it.
//
// Release images are built without .env: settings.h keeps the Wi-Fi, time zone and location of
// the last image that had them, so the update doesn't lose the network it came over.
//
// Rollback: the first boot of a new image is on probation. confirmBoot(), at the end of
// setup(), keeps it; if it crashes or resets before that, the bootloader goes back to the
// previous slot (sysinfo::otaState() then says ROLLED BACK).
#pragma once
#include <stdint.h>

#include <string>

#ifndef INKDECK_UPDATE_REPO
#define INKDECK_UPDATE_REPO "onionhammer/INKDECK"  // platformio.ini; a fork names its own
#endif

namespace ota {

constexpr const char *kRepo = INKDECK_UPDATE_REPO;

enum class Phase : uint8_t {
  Idle,         // nothing checked yet, or a check was cancelled
  Joining,      // joining Wi-Fi
  Checking,     // fetching the latest release's manifest
  Checked,      // knows the latest release: Status::release, Status::order
  Downloading,  // writing it to the idle slot
  Done,         // written and verified: restarting into it
  Failed,       // Status::result says why
};

// The latest release, as its manifest.json describes it.
struct Release {
  std::string version;  // "0.2.0"
  std::string build;    // the commit it was built from, "1a2b3c4"
  std::string date;     // "2026-11-25"
  std::string url;      // the image
  std::string sha256;   // the image's, 64 lowercase hex digits
  uint32_t size = 0;    // the image's, in bytes
};

struct Status {
  Phase phase = Phase::Idle;
  Release release;     // once a check has found it (empty before)
  int order = 0;       // the release against the running firmware: > 0 newer, 0 same, < 0 older
  uint32_t bytes = 0;  // Downloading: written so far, of release.size
  // Failed: why ("WI-FI: NETWORK NOT FOUND", "NO RELEASE FOUND", "CHECKSUM DOESN'T MATCH").
  std::string result;
};

// A Wi-Fi network to fetch over (settings.h).
bool configured();

// Starts a check on a background task. If a check or a download is already running, it carries
// on (and a stop() still pending is withdrawn). False if the task couldn't start.
bool check();
// Once a check has found a release: downloads and installs it on a background task, whatever
// its version. False if there's nothing to install or something is already running.
bool install();
// Cancels a check or a download and turns the radio off (unless something else uses it). The
// running firmware stays; a download that has already passed its checks still restarts into
// the new image.
void stop();

Status status();
bool active();  // Joining, Checking, Downloading or Done: keeps the board awake (power.h, idle.h)

// The Updater screen has put DONE on the panel; the task restarts the board once it has (or
// after a few seconds anyway).
void doneShown();

// End of setup(): marks a new image good, ending its probation. Harmless otherwise.
void confirmBoot();

// Where the manifest comes from: GitHub's latest release, or what setManifestUrl() gave. Dev
// command !update URL sets one until the next boot, to try a build from a computer on the LAN
// before releasing it ("" goes back to GitHub's).
std::string manifestUrl();
void setManifestUrl(const std::string &url);

// Compares two firmware versions: "0.2.0", "0.2.0+5" (five commits after 0.2.0; newer than it,
// older than 0.2.1). > 0 if a is newer, < 0 if b is, 0 if they're the same or one won't parse.
int compareVersions(const std::string &a, const std::string &b);

}  // namespace ota
