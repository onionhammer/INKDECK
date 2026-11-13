// Calendar arithmetic shared by the RTC driver, the Clock and the alarms. Instants are time_t
// (UTC); local dates and times come from localtime_r()/mktime(), which follow TIMEZONE.
#pragma once
#include <time.h>

namespace timeutil {

// Days since 1970-01-01 for a civil date, any year (Howard Hinnant's days_from_civil).
long daysFromCivil(int year, int month, int day);

// The instant for a broken-down UTC time; mktime() would read it as local time.
time_t utcSeconds(const tm &t);

int daysInMonth(int year, int month);  // month 1-12

// Monday = 0 ... Sunday = 6 (tm_wday counts from Sunday).
inline int mondayIndex(const tm &t) { return (t.tm_wday + 6) % 7; }

// The instant of a local wall-clock time on the local date of `t` (tm_isdst worked out by
// mktime, so it follows daylight saving).
time_t localAt(const tm &date, int hour, int minute, int second = 0);

}  // namespace timeutil
