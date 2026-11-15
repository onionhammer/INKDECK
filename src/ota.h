// Firmware updates over Wi-Fi, pushed from the computer that builds them. System → UPDATE OVER
// WI-FI opens the Updater screen, which calls start(): the board joins the network from .env and
// listens for espota (ArduinoOTA) on inkdeck.local:3232 for up to 10 minutes. Then, on the computer:
//
//   pio run -t upload --upload-port inkdeck.local
//
// PlatformIO switches to espota for a network upload port, and scripts/gen_secrets.py adds
// OTA_PASSWORD from .env (the firmware holds only its SHA-256). The image goes into the idle
// OTA slot; only a complete, verified image (espota's MD5, then the app image checks) switches
// the boot slot, so an interrupted transfer leaves the running firmware as it was. After a good
// one the board restarts into it.
//
// Rollback: the first boot of a new image is on probation. confirmBoot(), at the end of
// setup(), keeps it; if it crashes or resets before that, the bootloader goes back to the
// previous slot (sysinfo::otaState() then says ROLLED BACK).
#pragma once
#include <stdint.h>

#include <string>

namespace ota {

constexpr const char *kHostname = "inkdeck";  // mDNS: inkdeck.local
constexpr uint16_t kPort = 3232;              // espota's default for the ESP32
constexpr uint32_t kListenMinutes = 10;

enum class Phase : uint8_t {
  Idle,       // not listening (never started, or stopped)
  Joining,    // joining Wi-Fi
  Listening,  // waiting for espota
  Receiving,  // writing an image to the idle slot
  Done,       // image written and verified: restarting into it
  Failed,     // gave up (Wi-Fi, or nothing arrived in kListenMinutes)
};

struct Status {
  Phase phase = Phase::Idle;
  uint32_t bytes = 0;        // Receiving: written so far
  uint32_t total = 0;        // Receiving: the image's size
  uint32_t secondsLeft = 0;  // Listening: of the window (each attempt restarts it)
  std::string ip;            // once joined: "192.168.1.42"
  // Failed: why ("WI-FI: NETWORK NOT FOUND", "NOTHING ARRIVED IN 10 MIN"). Listening: what
  // went wrong with the last attempt, if one did ("WRONG PASSWORD").
  std::string result;
};

// .env names a network and sets OTA_PASSWORD.
bool configured();

// Starts listening on a background task. If it's already running, it carries on (and a stop()
// still pending is withdrawn). False if the task couldn't start.
bool start();
// Stops listening and turns the radio off (unless something else uses it). A transfer under
// way finishes first; if it was good, the board still restarts into it.
void stop();

Status status();
bool active();  // Joining, Listening or Receiving: keeps the board awake (power.h, apps/idle.h)

// The Updater screen has put DONE on the panel; the task restarts the board once it has (or
// after a few seconds anyway).
void doneShown();

// End of setup(): marks a new image good, ending its probation. Harmless otherwise.
void confirmBoot();

}  // namespace ota
