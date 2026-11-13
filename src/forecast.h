// The forecast, from Open-Meteo (open-meteo.com: no API key, free for non-commercial use) for
// LATITUDE/LONGITUDE in .env: current conditions, 48 hours and 7 days. Values stay in SI (°C,
// km/h, mm); units.h converts them for the screens.
//
// The last forecast is kept in NVS, so the Weather screen opens at once and works offline;
// when the "current" block is more than an hour old, the hour of the forecast stands in for
// it. While the board is on, poll() syncs once an hour (join Wi-Fi, one HTTPS request, Wi-Fi
// off) on its own task.
#pragma once
#include <stdint.h>
#include <time.h>

#include <string>

namespace weather {

constexpr int kHours = 48;
constexpr int kDays = 7;
constexpr uint8_t kUnknown = 0xFF;  // a rain chance the model doesn't give

// Which icon a WMO weather code gets.
enum class Sky : uint8_t { Clear, Partly, Cloud, Fog, Showers, Rain, Snow, Storm };

struct Hour {
  time_t time = 0;  // start of the hour (for the current block: the reading's time)
  float tempC = 0;
  float feelsC = 0;
  float windKmh = 0;
  uint16_t windDeg = 0;   // where the wind comes from
  uint8_t rh = 0;         // relative humidity, %
  uint8_t rain = kUnknown;  // chance of precipitation, %
  uint8_t code = 0;       // WMO weather code
  bool day = true;        // the sun is up
};

struct Day {
  time_t date = 0;  // midnight at the start of the day, local to the location
  float loC = 0;
  float hiC = 0;
  float precipMm = 0;
  float windMaxKmh = 0;
  uint16_t windDeg = 0;  // dominant direction
  uint8_t rain = kUnknown;  // highest chance of precipitation that day, %
  uint8_t code = 0;
  time_t sunrise = 0;
  time_t sunset = 0;
};

struct Forecast {
  time_t fetched = 0;  // 0: none
  Hour current;
  int hourCount = 0;
  Hour hours[kHours];
  int dayCount = 0;
  Day days[kDays];
};

// .env gives a location and a Wi-Fi network.
bool configured();

// The last forecast (loaded from NVS the first time). False if there's none.
bool get(Forecast &out);

// Conditions at t: the current block if the forecast is under an hour old, otherwise the hour
// that contains t. nullptr if the forecast doesn't reach t.
const Hour *at(const Forecast &f, time_t t);
// The hours that start after t, up to max. Returns how many.
int hoursAfter(const Forecast &f, time_t t, const Hour *out[], int max);
// Today (the local day containing t) and the days after it, up to max.
int daysFrom(const Forecast &f, time_t t, const Day *out[], int max);

const char *condition(uint8_t code);  // "OVERCAST", "LIGHT SNOW"
Sky sky(uint8_t code);
const char *compass(uint16_t degrees);  // "SW"

// --- Syncing.
enum class Phase : uint8_t { Idle, Connecting, Fetching, Done, Failed };
struct Status {
  Phase phase = Phase::Idle;
  std::string result;  // Done or Failed: "UPDATED 14:02", "WI-FI: NETWORK NOT FOUND"
  time_t at = 0;       // when it finished
};

bool start();  // sync now (HOLD ● on the screen); false if one is already running
Status status();
bool syncing();

// When poll() syncs next; 0 if it won't (not configured, or the clock isn't set).
time_t nextSync();

// From loop(): starts the hourly sync while the board is on. Skipped on battery below
// power::kLowBatteryPercent.
void poll();

}  // namespace weather
