#include "suntimes.h"

#include <math.h>

#include "settings.h"  // the location: SD:/inkdeck.ini, else LATITUDE/LONGITUDE from .env
#include "timeutil.h"

namespace sun {
namespace {

constexpr double kDeg = M_PI / 180.0;
constexpr double kZenith = 90.833;  // the disc's top edge, with refraction

double wrap(double v, double range) {
  v = fmod(v, range);
  return v < 0 ? v + range : v;
}

// Hours after 00:00 UTC of the event on day-of-year `n` (1-366); NAN if it doesn't happen
// (cosH tells which way: > 1 never rises, < -1 never sets).
double eventUtcHours(int n, bool rising, double lat, double lon, double &cosH) {
  const double lngHour = lon / 15.0;
  const double t = n + ((rising ? 6.0 : 18.0) - lngHour) / 24.0;
  const double m = 0.9856 * t - 3.289;  // mean anomaly
  const double l = wrap(m + 1.916 * sin(m * kDeg) + 0.020 * sin(2 * m * kDeg) + 282.634, 360);
  double ra = wrap(atan(0.91764 * tan(l * kDeg)) / kDeg, 360);
  ra += floor(l / 90) * 90 - floor(ra / 90) * 90;  // same quadrant as l
  ra /= 15.0;
  const double sinDec = 0.39782 * sin(l * kDeg);
  const double cosDec = cos(asin(sinDec));
  cosH = (cos(kZenith * kDeg) - sinDec * sin(lat * kDeg)) / (cosDec * cos(lat * kDeg));
  if (cosH > 1 || cosH < -1) return NAN;
  const double h = (rising ? 360 - acos(cosH) / kDeg : acos(cosH) / kDeg) / 15.0;
  const double localMean = h + ra - 0.06571 * t - 6.622;
  return wrap(localMean - lngHour, 24);
}

// The event as an instant on the local day `day` (UTC hours can land on the next or the
// previous UTC date, so nudge it onto the right local day).
time_t onLocalDay(const tm &day, double utcHours) {
  tm midnight = {};
  midnight.tm_year = day.tm_year;
  midnight.tm_mon = day.tm_mon;
  midnight.tm_mday = day.tm_mday;
  time_t when = timeutil::utcSeconds(midnight) + static_cast<time_t>(lround(utcHours * 3600));
  for (int i = 0; i < 2; ++i) {
    tm local;
    localtime_r(&when, &local);
    const long have =
        timeutil::daysFromCivil(local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
    const long want = timeutil::daysFromCivil(day.tm_year + 1900, day.tm_mon + 1, day.tm_mday);
    if (have == want) break;
    when += (want - have) * 86400;
  }
  return when;
}

}  // namespace

bool configured() { return settings::hasLocation(); }

Day on(time_t when) {
  Day d;
  if (!configured()) return d;
  tm day;
  localtime_r(&when, &day);
  double cosRise = 0, cosSet = 0;
  const double lat = settings::latitude(), lon = settings::longitude();
  const double rise = eventUtcHours(day.tm_yday + 1, true, lat, lon, cosRise);
  const double set = eventUtcHours(day.tm_yday + 1, false, lat, lon, cosSet);
  if (isnan(rise) || isnan(set)) {
    d.alwaysUp = cosRise < -1;
    d.alwaysDown = cosRise > 1;
    return d;
  }
  d.ok = true;
  d.rise = onLocalDay(day, rise);
  d.set = onLocalDay(day, set);
  return d;
}

}  // namespace sun
