// The Clock's alarms: three slots kept in NVS. Each is a local wall-clock time, so it follows
// daylight saving; next() turns them into instants (UTC) for ringing and, later, for arming
// the RTC before a power-off.
#pragma once
#include <stdint.h>
#include <time.h>

#include <string>

namespace alarms {

constexpr int kCount = 3;

// Repeat days as a mask: bit 0 = Monday ... bit 6 = Sunday. 0 = once: rings at the next
// HH:MM, then turns itself off.
constexpr uint8_t kOnce = 0;
constexpr uint8_t kEveryDay = 0x7F;
constexpr uint8_t kWeekdays = 0x1F;
constexpr uint8_t kWeekend = 0x60;

struct Alarm {
  uint8_t hour = 7;
  uint8_t minute = 0;
  uint8_t days = kWeekdays;
  bool on = false;
};

void begin();  // loads the slots from NVS (defaults, all off, the first time)

Alarm get(int slot);
void set(int slot, const Alarm &alarm);  // saves to NVS when it changed

// The first ring of `alarm` strictly after `after`, whether or not it's on; 0 if never (a
// repeat mask with no days).
time_t ringAfter(const Alarm &alarm, time_t after);
// The soonest ring of any alarm that's on (or a snooze), strictly after `after`; 0 if none.
// `slot` gets which one.
time_t next(time_t after, int *slot = nullptr);

// At an RTC wake at `now`: the next alarm from the one the wake was armed for
// (power::armedFor()), else from a minute ago; 0 for none. It's ringing if it's by now + 1.
time_t nextAtWake(time_t now);

// The alarm that rings in (since, now], or -1. Call it with consecutive windows so each ring
// is reported once. A one-off alarm that's due turns itself off, and a due snooze is used up.
int due(time_t since, time_t now);

// Rings `slot` again at `until` (kept in NVS, so a power-off can wake for it). One at a time.
void snooze(int slot, time_t until);
time_t snoozedUntil(int *slot = nullptr);  // 0 if no snooze is pending

// Repeat choices in the order the editor's dial steps through them.
uint8_t stepRepeat(uint8_t days, int delta);
// "Mon–Fri", "Sat–Sun", "every day", "Tue", "Mon, Wed" or "once".
std::string repeatLabel(uint8_t days);

}  // namespace alarms
