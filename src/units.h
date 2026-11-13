// Display units, chosen at build time. Readings are kept in SI internally (and in the CSV log);
// these only convert what the screens show.
//   -DINKDECK_FAHRENHEIT=1   US units: temperatures in °F, wind in mph, rain in inches
//                            (default 0: °C, km/h, mm). Set in platformio.ini.
#pragma once

#ifndef INKDECK_FAHRENHEIT
#define INKDECK_FAHRENHEIT 0
#endif

namespace units {

inline constexpr bool kFahrenheit = INKDECK_FAHRENHEIT != 0;

inline float temperature(float celsius) { return kFahrenheit ? celsius * 9 / 5 + 32 : celsius; }
inline const char *temperatureUnit() { return kFahrenheit ? "\xC2\xB0" "F" : "\xC2\xB0" "C"; }

inline float windSpeed(float kmh) { return kFahrenheit ? kmh / 1.609344f : kmh; }
inline const char *windUnit() { return kFahrenheit ? "MPH" : "KM/H"; }

inline float precipitation(float mm) { return kFahrenheit ? mm / 25.4f : mm; }
inline const char *precipitationUnit() { return kFahrenheit ? "IN" : "MM"; }
inline int precipitationDecimals() { return kFahrenheit ? 2 : 1; }

}  // namespace units
