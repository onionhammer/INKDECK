#include "timeutil.h"

#include <stdio.h>

namespace timeutil {

long daysFromCivil(int year, int month, int day) {
  const int y = year - (month <= 2);
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097L + static_cast<long>(doe) - 719468;
}

time_t utcSeconds(const tm &t) {
  const long days = daysFromCivil(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
  return static_cast<time_t>(days) * 86400 + t.tm_hour * 3600 + t.tm_min * 60 + t.tm_sec;
}

int daysInMonth(int year, int month) {
  static constexpr int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
  return month == 2 && leap ? 29 : kDays[(month - 1) % 12];
}

time_t localAt(const tm &date, int hour, int minute, int second) {
  tm t = {};
  t.tm_year = date.tm_year;
  t.tm_mon = date.tm_mon;
  t.tm_mday = date.tm_mday;
  t.tm_hour = hour;
  t.tm_min = minute;
  t.tm_sec = second;
  t.tm_isdst = -1;
  return mktime(&t);
}

const char *const kWeekdayShort[7] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
const char *const kMonthNames[12] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                     "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};

std::string hhmm(int hour, int minute) {
  char buf[8];
  snprintf(buf, sizeof buf, "%02d:%02d", hour, minute);
  return buf;
}

std::string hhmm(time_t t) {
  tm l;
  localtime_r(&t, &l);
  return hhmm(l.tm_hour, l.tm_min);
}

bool sameDay(const tm &a, const tm &b) { return a.tm_year == b.tm_year && a.tm_yday == b.tm_yday; }

bool sameLocalDay(time_t a, time_t b) {
  tm x, y;
  localtime_r(&a, &x);
  localtime_r(&b, &y);
  return sameDay(x, y);
}

std::string clockText(uint32_t s, bool padMinutes) {
  char buf[16];
  if (s >= 3600) {
    snprintf(buf, sizeof buf, "%lu:%02lu:%02lu", static_cast<unsigned long>(s / 3600),
             static_cast<unsigned long>(s / 60 % 60), static_cast<unsigned long>(s % 60));
  } else {
    snprintf(buf, sizeof buf, padMinutes ? "%02lu:%02lu" : "%lu:%02lu",
             static_cast<unsigned long>(s / 60), static_cast<unsigned long>(s % 60));
  }
  return buf;
}

}  // namespace timeutil
