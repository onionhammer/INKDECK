// Weather (the "Weather" artboard): the temperature now in 130 px with an icon, the condition,
// feels-like, wind, humidity and the next sunrise or sunset; the next 12 hours (temperature and
// rain-chance bars); six days with lo-hi range bars. The forecast comes from weather.h.
//   ▲▼ day   ● detail (a sheet for the selected day)   HOLD ● sync now   BOOT back
#pragma once
#include <memory>
#include <string>

#include "ui/nav.h"

namespace apps {

std::unique_ptr<ui::Screen> makeWeather();

// "12°", "−4°" in the display unit (units.h); "--°" for a missing value.
std::string temperatureText(float celsius);

}  // namespace apps
