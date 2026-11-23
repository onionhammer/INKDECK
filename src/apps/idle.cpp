#include "apps/idle.h"

#include <Arduino.h>
#include <driver/usb_serial_jtag.h>
#include <esp_system.h>
#include <sys/time.h>

#include <atomic>

#include "apps/agent_face.h"
#include "apps/clock.h"
#include "apps/closed_book.h"
#include "apps/comms_ui.h"
#include "apps/lock.h"
#include "apps/news_edition.h"
#include "apps/sleep.h"
#include "agent_client.h"
#include "audio.h"
#include "comms.h"
#include "features.h"
#include "file_copy.h"
#include "input.h"
#include "news_library.h"
#include "page_fetch.h"
#include "ota.h"
#include "pmic.h"
#include "podcast_library.h"
#include "power.h"
#include "qmi8658.h"
#include "screen.h"
#include "timesync.h"
#include "forecast.h"
#include "ui/nav.h"
#include "wlan.h"

namespace apps {
namespace {

// No waiting after a page turn: the redraw finishes inside loop() before the nap check runs,
// a button that's down keeps the board awake, and a released press waits in the key queue.
// The guard only covers the first debounce samples after a button wakes the board.
constexpr uint32_t kKeyWakeGuardMs = 30;
// Awake after the IMU's wake on motion, for a screen that turns with the board: the navigator
// ticks at once (the IMU may still be settling), then each second; three more readings let a
// turn that's still going on at the wake finish and hold for two of them.
constexpr uint32_t kMotionAwakeMs = 3500;
constexpr uint32_t kUsbGraceMs = 3000;  // USB power without a host talking, after naps

std::atomic<int> standbyRequest{-1};
std::atomic<bool> closeBookRequest{false};

bool napping = false;
bool napMotion = false;  // this nap's IMU is on wake on motion (else it's off)
bool nappedSinceBoot = false;
uint32_t usbSinceMs = 0;  // when USB power appeared after naps; 0 = not present
uint32_t keyWakeMs = 0;
uint32_t motionWakeMs = 0;  // 0: none yet
std::atomic<uint32_t> naps{0};
std::atomic<uint32_t> buttonWakes{0};
std::atomic<uint32_t> motionWakes{0};
std::atomic<uint32_t> asleepMs{0};

bool readingNow() {
  const ui::Screen *b = ui::nav::base();
  return b && b->reading();
}

bool mayNap() {
  // A paused player doesn't count: pause switches the audio output off (audio.h).
  if (!readingNow() || audio::busy() || wlan::radioOn()) return false;
  if (weather::syncing() || podcasts::busy() || ota::active() || filecopy::busy()) return false;
  if (news::busy() || pagefetch::busy()) return false;
  const timesync::Phase t = timesync::status().phase;
  if (t == timesync::Phase::Connecting || t == timesync::Phase::Asking) return false;
  if (millis() - keyWakeMs < kKeyWakeGuardMs) return false;
  if (motionWakeMs && millis() - motionWakeMs < kMotionAwakeMs) return false;
  if (input::anyDown() || input::pending()) return false;
  // A sheet over the book (the ask sheet) is drawn and answered awake; it goes by itself.
  const ui::Screen *top = ui::nav::top();
  if (top && top->overlay()) return false;
#if INKDECK_AGENT
  if (agent::polling()) return false;  // a check-in due or under way: awake until it's done
#endif
  // On USB the power doesn't matter, and light sleep would drop the console. Plugging USB into
  // a napping board ends the naps at the next wake (a key, or the minute).
  const pmic::Status p = pmic::status();
  return p.ok && p.battery && !p.vbus && !pmic::pretendingBattery();  // !reachtest: still on USB
}

void stopNapping() {
  if (!napping) return;
  napping = false;
  qmi8658::setEnabled(true);  // and out of wake on motion
  log_i("nap: awake (%lu naps, woken by %lu buttons and %lu motions, %lu s asleep since boot)",
        static_cast<unsigned long>(naps.load()), static_cast<unsigned long>(buttonWakes.load()),
        static_cast<unsigned long>(motionWakes.load()),
        static_cast<unsigned long>(asleepMs.load() / 1000));
}

// Light sleep leaves the USB Serial/JTAG controller unable to talk to a host until a reset
// (seen on hardware). So once the board has napped, USB power with no host traffic for a few
// seconds means a clean restart: the Reader saves its place first. Plugged into a charger
// rather than a computer, that's one harmless restart.
void restartIfUsbStuck() {
  if (!nappedSinceBoot) return;
  if (!pmic::status().vbus) {
    usbSinceMs = 0;
    return;
  }
  if (!usbSinceMs) usbSinceMs = millis() | 1;
  if (millis() - usbSinceMs < kUsbGraceMs || usb_serial_jtag_is_connected()) return;
  log_w("nap: USB power but no host after light sleep; restarting");
  if (ui::Screen *b = ui::nav::base()) b->onNap();  // save the place
  delay(50);
  esp_restart();
}

// A key on a board woken for comms made it an ordinary board that's on (power.h, reachable on
// battery): its face goes, with the sheets over it. If the key answered (the board is in a
// conversation, or its mic is on) the Comms screen opens, the place to talk from; else what a
// start shows. While it still rings the ring sheet stays, over the face.
bool wokenOnPending = false;
void wokenOn() {
  const ui::Screen *b = ui::nav::base();
  if (!b || !b->offFace()) {
    wokenOnPending = false;
    return;
  }
#if INKDECK_COMMS
  if (comms::incomingRing().on) return;
#endif
  wokenOnPending = false;
  while (ui::nav::top() != b) ui::nav::pop();
  ui::nav::pop();
#if INKDECK_COMMS
  if (comms::mic().on || comms::conversation(comms::status().group).in) ui::nav::push(makeComms());
#endif
}

}  // namespace

void pollPower() {
  // Up after a Comms wake (power.h, reachable on battery): the face holds, no idle rules; once
  // it's over, the sheets over the face go, and its tick powers it off again.
  const power::Held before = power::reachHold();
  power::reachStep();
  if (before == power::Held::Stay && power::reachHold() == power::Held::None) wokenOnPending = true;
  if (power::reachHold() != power::Held::None) wokenOnPending = false;
  if (wokenOnPending) wokenOn();
  switch (power::reachHold()) {
    case power::Held::Stay: return;
    case power::Held::Sleep: {
      const ui::Screen *b = ui::nav::base();
      if (b && b->offFace()) {
        while (ui::nav::top() != b) ui::nav::pop();
        return;
      }
      reachFace(power::reachMode());  // the face went some other way: it comes again
      return;
    }
    case power::Held::None: break;
  }
  pollAgentFace();  // the dev tools'
  pollEdition();
  const int request = standbyRequest.exchange(-1);
  if (request >= 0) {
    stopNapping();
    enterStandby(request);
    return;
  }
  const ui::Screen *top = ui::nav::top();
  if (top && top->offFace()) return;  // already on its way off
  const ui::Screen *base = ui::nav::base();
  const bool book = base && base->closesWhenIdle();
  if (closeBookRequest.exchange(false) && book) {
    stopNapping();
    closeBook(true);
    return;
  }
  // A book left open: 15 minutes of naps, then off behind its cover.
  if (book && power::idleBook()) {
    log_i("idle on battery with a book open: closing it");
    stopNapping();
    closeBook();
    return;
  }
  if (!power::idle(readingNow())) return;
  stopNapping();
  const pmic::Status p = pmic::status();
  const bool low = power::lowBattery(p);
  // From News, its edition; from the Agent (or with a session asking or working), its face.
  const bool edition = !low && editionWanted();
  const bool agentFace = !low && !edition && agentFaceWanted();
  log_i("idle on battery (%d %%): %s", p.percent,
        edition     ? "the edition"
        : agentFace ? "the Agent face"
        : !low      ? "standby"
        : book      ? "closing the book"
                    : "sleep");
  if (edition) {
    showEdition(false);
  } else if (agentFace) {
    showAgentFace(false);
  } else if (!low) {
    enterStandby(0);
  } else if (book) {
    closeBook();
  } else {
    sleepNow();
  }
}

void powerKey() {
  // Up after a Comms wake: PWR is a key like the others (it makes the board an ordinary one).
  if (power::reachHold() == power::Held::Stay) return;
  if (wakeDesk() || reopenBook() || wakeAgentFace() || wakeEdition()) return;
  // On News, its edition; whether the Agent's face is wanted, worked out before the PIN is
  // forgotten.
  const bool edition = editionWanted();
  const bool agentFace = !edition && agentFaceWanted();
  forgetGuard(Guarded::Agent);  // back from the desk face, the Agent asks for the PIN again
  const pmic::Status p = pmic::status();  // pretends to be on battery under !reachtest
  const ui::Screen *base = ui::nav::base();
  if (p.ok && p.vbus) {
    if (edition) {
      showEdition(true);  // live, in place of the desk face
    } else if (agentFace) {
      showAgentFace(true);  // live, in place of the desk face
    } else {
      enterDesk();
    }
  } else if (base && base->closesWhenIdle()) {
    closeBook();  // a book open in the Reader: off behind its cover, as the idle timeout does
  } else if (edition) {
    showEdition(false);
  } else if (agentFace) {
    showAgentFace(false);
  } else {
    sleepNow();
  }
}

bool napIfIdle() {
  restartIfUsbStuck();
  if (!mayNap()) {
    stopNapping();
    return false;
  }
  if (!napping) {
    napping = true;
    nappedSinceBoot = true;
    ui::Screen *b = ui::nav::base();
    napMotion = b && b->wakeOnMotion() && qmi8658::wakeOnMotion();
    if (!napMotion) qmi8658::setEnabled(false);
    if (b) b->onNap();
    // The panel's power (ALDO3) is off already: every update ends by cutting it (Epd::show()).
  }
  // Until the next minute boundary (an alarm rings on the minute), a little after it.
  timeval tv;
  gettimeofday(&tv, nullptr);
  const uint32_t ms = 60000 - static_cast<uint32_t>(tv.tv_sec % 60) * 1000 -
                      static_cast<uint32_t>(tv.tv_usec / 1000) + 20;
  const uint32_t before = millis();
  const power::NapEnd end = power::nap(ms, napMotion);
  asleepMs += millis() - before;
  ++naps;
  if (end == power::NapEnd::Button) {
    keyWakeMs = millis();
    ++buttonWakes;
  } else if (end == power::NapEnd::Motion) {
    motionWakeMs = millis() | 1;
    ++motionWakes;
  }
  return true;
}

bool isNapping() { return napping; }

void reachFace(power::Mode mode) {
  switch (mode) {
    case power::Mode::Standby: enterStandby(0); break;
    case power::Mode::Book: closeBook(); break;
    case power::Mode::Agent: showAgentFace(false); break;
    case power::Mode::Edition: showEdition(false); break;
    case power::Mode::Sleep:
    case power::Mode::On: sleepNow(); break;
  }
}

NapStats napStats() {
  NapStats s;
  s.naps = naps.load();
  s.buttonWakes = buttonWakes.load();
  s.asleepSeconds = asleepMs.load() / 1000;
  return s;
}

void requestStandby(int cycles) { standbyRequest = cycles; }
void requestCloseBook() { closeBookRequest = true; }

}  // namespace apps
