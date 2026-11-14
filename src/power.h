// Power-off and wake-up (AGENTS.md, "Power architecture"). The PMIC's soft power-off cuts every
// rail but the RTC's; the panel keeps its last image. The RTC alarm, diode-OR'd onto the PMIC's
// PWRON, boots the board again, as do PWR held 1 s and plugging in USB.
//
// Two ways to be off:
//   Sleep    the Sleep screen (the frog); wakes only for an alarm.
//   Standby  the Clock face; wakes every minute to redraw it, and for alarms. No radio, unless
//            INKDECK_STANDBY_WEATHER_HOURS (forecast.h) has it fetch the forecast.
#pragma once
#include <stdint.h>
#include <time.h>

namespace power {

enum class Wake : uint8_t { Reset, PowerKey, Alarm, Usb, Other };
enum class Mode : uint8_t { On, Sleep, Standby };

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
// ringing, no time or weather sync or podcast download. While reading (the Reader is open) or while music plays, only once
// the battery is below kLowBatteryPercent as well.
bool idle(bool reading);

// Light sleep: the CPUs stop, RAM, PSRAM and peripherals keep their state, and the board draws
// a fraction of a milliamp plus whatever the rails feed. A button (dial, press, BOOT, PWR)
// wakes it within a few milliseconds, and the press still reaches `input` as a key. Otherwise
// it sleeps maxMs. Returns true if a button woke it. Call with no button down.
bool nap(uint32_t maxMs);

}  // namespace power
