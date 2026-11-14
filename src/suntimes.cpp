#include "suntimes.h"

#include <math.h>

#include "settings.h"  // the location: SD:/inkdeck.ini, else LATITUDE/LONGITUDE from .env
#include "timeutil.h"

namespace sun {
namespace {

// Single precision on purpose: the double versions of sin, cos, tan and the rest cost about
// 9 KB of flash, and with day-of-year inputs every value stays small. Checked against the
// double version over every day, latitudes to ±66° and longitudes every 15°: within 0.16 s.
constexpr float kDeg = static_cast<float>(M_PI) / 180.0f;
constexpr float kZenith = 90.833f;  // the disc's top edge, with refraction

float wrap(float v, float range) {
  v = fmodf(v, range);
  return v < 0 ? v + range : v;
}

// Hours after 00:00 UTC of the event on day-of-year `n` (1-366); NAN if it doesn't happen
// (cosH tells which way: > 1 never rises, < -1 never sets).
float eventUtcHours(int n, bool rising, float lat, float lon, float &cosH) {
  const float lngHour = lon / 15.0f;
  const float t = n + ((rising ? 6.0f : 18.0f) - lngHour) / 24.0f;
  const float m = 0.9856f * t - 3.289f;  // mean anomaly
  const float l = wrap(m + 1.916f * sinf(m * kDeg) + 0.020f * sinf(2 * m * kDeg) + 282.634f, 360);
  float ra = wrap(atanf(0.91764f * tanf(l * kDeg)) / kDeg, 360);
  ra += floorf(l / 90) * 90 - floorf(ra / 90) * 90;  // same quadrant as l
  ra /= 15.0f;
  const float sinDec = 0.39782f * sinf(l * kDeg);
  const float cosDec = cosf(asinf(sinDec));
  cosH = (cosf(kZenith * kDeg) - sinDec * sinf(lat * kDeg)) / (cosDec * cosf(lat * kDeg));
  if (cosH > 1 || cosH < -1) return NAN;
  const float h = (rising ? 360 - acosf(cosH) / kDeg : acosf(cosH) / kDeg) / 15.0f;
  const float localMean = h + ra - 0.06571f * t - 6.622f;
  return wrap(localMean - lngHour, 24);
}

// The event as an instant on the local day `day` (UTC hours can land on the next or the
// previous UTC date, so nudge it onto the right local day).
time_t onLocalDay(const tm &day, float utcHours) {
  tm midnight = {};
  midnight.tm_year = day.tm_year;
  midnight.tm_mon = day.tm_mon;
  midnight.tm_mday = day.tm_mday;
  time_t when = timeutil::utcSeconds(midnight) + static_cast<time_t>(lroundf(utcHours * 3600));
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
  float cosRise = 0, cosSet = 0;
  const float lat = static_cast<float>(settings::latitude());
  const float lon = static_cast<float>(settings::longitude());
  const float rise = eventUtcHours(day.tm_yday + 1, true, lat, lon, cosRise);
  const float set = eventUtcHours(day.tm_yday + 1, false, lat, lon, cosSet);
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
