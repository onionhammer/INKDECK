// The forecast, from Open-Meteo (open-meteo.com: no API key, free for non-commercial use) for
// LATITUDE/LONGITUDE in .env: current conditions, 48 hours and 7 days. Values stay in SI (°C,
// km/h, mm); units.h converts them for the screens. Inside the contiguous US it asks for the
// National Blend of Models (NBM), elsewhere Open-Meteo's best match (forecast.cpp, modelFor()).
//
// The last forecast is kept in NVS, so the Weather screen opens at once and works offline;
// when the "current" block is more than an hour old, the hour of the forecast stands in for
// it. While the board is on, poll() syncs once an hour, or every half hour on USB (join Wi-Fi,
// one HTTPS request, Wi-Fi off) on its own task; loop() doesn't call it while the Reader naps
// (apps::isNapping()), so that sync waits for a key. In standby, syncInStandby() can do the same
// every few hours (INKDECK_STANDBY_WEATHER_HOURS below).
#pragma once
#include <stdint.h>
#include <time.h>

#include <string>

// -DINKDECK_STANDBY_WEATHER_HOURS=N: in standby, fetch the forecast every N hours, so the
// standby face's outside weather stays current. Each fetch costs about as much as 10 minute
// wakes. 0 (the default): never; the face uses the forecast saved while the board was on, which
// runs out 48 hours after that. Set in platformio.ini.
#ifndef INKDECK_STANDBY_WEATHER_HOURS
#define INKDECK_STANDBY_WEATHER_HOURS 0
#endif

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
  float lat = 0;       // where it's for: a forecast for another place isn't shown
  float lon = 0;
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
// Dev tool: drops the saved forecast (RAM and NVS), as on a new board.
void forget();
// Dev tool: asks Open-Meteo for the sky now from each of `models` (comma-separated model
// names) and logs the reply, to compare them against the window.
void compareModels(const std::string &models);

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

// From loop(): starts the scheduled sync while the board is on (hourly, half-hourly on USB).
// Skipped on battery below power::kLowBatteryPercent.
void poll();

// From a standby wake (no task, nothing else running): syncs now, blocking, if one is due every
// `hours`, on the boundary after the last forecast as poll() does it. A shorter Wi-Fi timeout
// than poll()'s, and after a failure it waits `hours` again, doubling with each further one up
// to a day, so a board away from its network doesn't spend a timeout every time. Skipped on
// battery below power::kLowBatteryPercent. Call net::begin() first. True if it fetched one.
bool syncInStandby(int hours);

}  // namespace weather
