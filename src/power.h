// Power-off and wake-up (AGENTS.md, "Power architecture"). The PMIC's soft power-off cuts every
// rail but the RTC's; the panel keeps its last image. The RTC alarm, diode-OR'd onto the PMIC's
// PWRON, boots the board again, as do PWR held 1 s and plugging in USB.
//
// Three ways to be off:
//   Sleep    the Sleep screen (the frog); wakes only for an alarm.
//   Standby  the Clock face; wakes every minute to redraw it, and for alarms. No radio, unless
//            INKDECK_STANDBY_WEATHER_HOURS (forecast.h) has it fetch the forecast.
//   Book     a book's cover (apps/closed_book.h); wakes only for an alarm, and the next start
//            opens the book again, whatever started the board.
//   Agent    the Agent face (apps/agent_face.h); wakes to check in with inkharness, and for
//            alarms. HOLD PWR opens the Agent.
//   Edition  News's off face (apps/news_edition.h); wakes every minute to redraw the clock, show
//            the next story or fetch the feeds, and for alarms. HOLD PWR opens News on the story.
// Sleep, Standby, Book and Edition also wake when inkharness asks for a check-in (a schedule that
// wakes the board).
//
// Reachable on battery (mesh_reachable, comms.h): off() keeps the board in light sleep instead of
// the PMIC's power-off, the face on the panel as before, when comms is built in, mesh_reachable is
// on (or !commsreach), a group is configured, the board is on battery (or pretending: !reachtest)
// and the battery is at kLowBatteryPercent or more. off() restarts the board, and the sleep runs
// first thing in setup() (reachResume(): nothing else has started), polling the other boards.
// Its off face's next wake (the time the RTC alarm would have had) restarts the board as an RTC
// wake boots it (wake() is Alarm), PWR as a power-on does (PowerKey), USB power as plugging in
// does (Usb). An answer with something waiting goes on booting as wake() Comms: the face comes
// back (apps::reachFace()), comms listens, the sheets rise over the face, and with no key the
// face powers off again once it's over (reachHold()); a key makes the board an ordinary one
// that's on. Below kLowBatteryPercent, or with the setting off, nothing of this happens.
#pragma once
#include <stdint.h>
#include <time.h>

namespace pmic {
struct Status;
}

namespace power {

enum class Wake : uint8_t { Reset, PowerKey, Alarm, Usb, Other, Comms };
enum class Mode : uint8_t { On, Sleep, Standby, Book, Agent, Edition };

// Early in setup(), after Wire.begin(), pmic::configure() and rtc::begin(): works out why the
// board started and releases the RTC's interrupt (while it holds PWRON low, 4 s of that would
// force the PMIC off). Loads what the last power-off left in NVS.
void begin();

Wake wake();
const char *wakeName(Wake w);  // "PWR KEY", "RTC ALARM", ...
Mode lastMode();     // how the board powered off last time (On if it didn't)
time_t armedFor();   // the wake that was armed for this start; 0 if none
int standbyLeft();   // dev tool: standby wakes left before staying on (0 = no limit)

// Clears the record of the last power-off, once this start has used it.
void markOn();

// Powers the board off: music and Wi-Fi off, the sensor log closed, the RTC armed for `wakeAt`
// (0: no wake; it's moved on a minute if it's less than 2 s away, since a missed alarm only
// fires next month), then the PMIC's soft power-off. Returns only if that failed.
// standbyCycles: dev tool, see standbyLeft().
void off(Mode mode, time_t wakeAt, int standbyCycles = 0);

// The earlier of two wakes, 0 meaning none.
inline time_t earliest(time_t a, time_t b) { return !a ? b : !b ? a : (a < b ? a : b); }

constexpr int kLowBatteryPercent = 15;

// On battery, and below kLowBatteryPercent: no wakes but alarms, no check-ins, no fetches.
bool lowBattery(const pmic::Status &p);
bool lowBattery();  // from pmic::status()

// PWR is down now (PWR_KEY_SENSE, set as an input): a wake that should boot instead.
bool pwrHeld();

// True when the board should power itself off: on battery, no key for 10 minutes, no alarm
// ringing, no time, weather or news sync, podcast download or Wi-Fi drive (webdav.h). While reading
// (the Reader is open) or while music plays, only once the battery is below kLowBatteryPercent
// as well.
bool idle(bool reading);

// True when an open book should close (apps/closed_book.h): on battery, no key for 15 minutes,
// and nothing playing, ringing, recording, syncing or downloading. A night of naps under an
// open book took 3 % of the battery (2026-11-17); off, the board draws about 40 uA.
bool idleBook();

// --- Reachable on battery (above) ---------------------------------------------------------------

// From setup(), right after begin(): when off() restarted the board to sleep reachable, the sleep
// (see above). Returns when an answer woke the board (wake() is then Comms) or when the board
// isn't sleeping; restarts it for anything else, or powers it off with the PMIC below
// kLowBatteryPercent.
void reachResume();
// The off mode the board slept in before a Comms wake, for apps::reachFace().
Mode reachMode();
// From setup(), after comms::begin(), on a Comms wake: the board stays up, holding comms.
void reachWokenBegin();
// From setup()'s end: off() from now on restarts the board to sleep (before, it sleeps in place).
void bootDone();

// Up after a Comms wake: Stay while the face holds (no power-off, no idle rules, sheets rise
// over it); Sleep once it's over without a key (the face powers off again: its tick, or
// pollPower() pops the sheets over it first); None otherwise (a key made the board an ordinary
// one: the face goes).
enum class Held : uint8_t { None, Stay, Sleep };
Held reachHold();
// From pollPower(), each turn: steps the hold with what the board does (a key, an alarm).
void reachStep();

// Dev tool !reachtest: on USB, the board behaves as on battery (pmic::pretendBattery()) and,
// with this feature on, sleeps reachable when it would power off: Pretend with a delay for the
// light sleep (the radio off, the console alive; 'o' on the console is PWR), Sleep with real light
// sleep for `minutes` (the console off the bus meanwhile), then on as after PWR. Kept in NVS
// until !reachtest off.
enum class ReachTest : uint8_t { Off, Pretend, Sleep };
ReachTest reachTest();
void setReachTest(ReachTest t, int minutes = 0);
// After light sleep on USB (the tests, !reachbench): the USB-Serial-JTAG back to its reset state,
// then a power cycle (the PMIC off, the RTC alarm back 20 s later), else a restart. Never returns.
[[noreturn]] void consoleRecover();

// Light sleep: the CPUs stop, RAM, PSRAM and peripherals keep their state, and the board draws
// a fraction of a milliamp plus whatever the rails feed. A button (dial, press, BOOT, PWR)
// wakes it within a few milliseconds, and the press still reaches `input` as a key. With
// motion, so does the IMU's wake-on-motion pin (arm it first: qmi8658::wakeOnMotion()).
// Otherwise it sleeps maxMs. Call with no button down.
enum class NapEnd : uint8_t { Timer, Button, Motion };
NapEnd nap(uint32_t maxMs, bool motion = false);

}  // namespace power
