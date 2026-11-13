// Sunrise and sunset, computed on the board for LATITUDE/LONGITUDE from .env (no network).
// Uses the Almanac for Computers (1990) method, good to about a minute away from the poles.
#pragma once
#include <time.h>

namespace sun {

bool configured();  // .env gives a location

struct Day {
  bool ok = false;      // configured, and the sun both rises and sets that day
  bool alwaysUp = false;    // polar day
  bool alwaysDown = false;  // polar night
  time_t rise = 0;  // instants (UTC)
  time_t set = 0;
};

// Sunrise and sunset on the local calendar day that contains `when`.
Day on(time_t when);

}  // namespace sun
