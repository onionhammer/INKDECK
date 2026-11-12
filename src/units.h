// Display units, chosen at build time. Readings are kept in SI internally (and in the CSV log);
// these only convert what the screens show.
//   -DINKDECK_FAHRENHEIT=1   temperatures in °F (default 0: °C). Set in platformio.ini.
#pragma once

#ifndef INKDECK_FAHRENHEIT
#define INKDECK_FAHRENHEIT 0
#endif

namespace units {

inline constexpr bool kFahrenheit = INKDECK_FAHRENHEIT != 0;

inline float temperature(float celsius) { return kFahrenheit ? celsius * 9 / 5 + 32 : celsius; }
inline const char *temperatureUnit() { return kFahrenheit ? "\xC2\xB0" "F" : "\xC2\xB0" "C"; }

}  // namespace units
