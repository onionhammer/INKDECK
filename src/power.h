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
//   Edition  News's morning edition (apps/news_edition.h); wakes at news_edition each day to sync
//            the feeds and draw the next one, and for alarms. HOLD PWR opens News.
// Sleep, Standby, Book and Edition also wake when inkharness asks for a check-in (a schedule that
// wakes the board).
#pragma once
#include <stdint.h>
#include <time.h>

namespace power {

enum class Wake : uint8_t { Reset, PowerKey, Alarm, Usb, Other };
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

constexpr int kLowBatteryPercent = 15;

// True when the board should power itself off: on battery, no key for 10 minutes, no alarm
// ringing, no time, weather or news sync, podcast download or Wi-Fi drive (webdav.h). While reading
// (the Reader is open) or while music plays, only once the battery is below kLowBatteryPercent
// as well.
bool idle(bool reading);

// True when an open book should close (apps/closed_book.h): on battery, no key for 15 minutes,
// and nothing playing, ringing, recording, syncing or downloading. A night of naps under an
// open book took 3 % of the battery (2026-11-17); off, the board draws about 40 uA.
bool idleBook();

// Light sleep: the CPUs stop, RAM, PSRAM and peripherals keep their state, and the board draws
// a fraction of a milliamp plus whatever the rails feed. A button (dial, press, BOOT, PWR)
// wakes it within a few milliseconds, and the press still reaches `input` as a key. With
// motion, so does the IMU's wake-on-motion pin (arm it first: qmi8658::wakeOnMotion()).
// Otherwise it sleeps maxMs. Call with no button down.
enum class NapEnd : uint8_t { Timer, Button, Motion };
NapEnd nap(uint32_t maxMs, bool motion = false);

}  // namespace power
