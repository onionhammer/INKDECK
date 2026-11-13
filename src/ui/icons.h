// Stroke icons in the design's style (docs/ui-design.md, "Icons"): drawn on a 48-unit grid with
// square caps and crisp edges, scaled to any size, ink only.
#pragma once
#include "ui/draw.h"

namespace ui::icons {

// Weather (the "Weather" artboard): sun, cloud, partly cloudy, rain and showers come from the
// canvas; moon, fog, snow and storm are drawn to match.
enum class Weather : uint8_t {
  Sun, Moon, Partly, PartlyNight, Cloud, Fog, Showers, Rain, Snow, Storm
};

// Draws the icon in the size x size square at (x, y). strokeUnits is the line width in grid
// units (the canvas uses 3 at 100 px, 4 at 28 px); it never goes below 2 px.
void drawWeather(Canvas &c, int x, int y, int size, Weather icon, float strokeUnits);

}  // namespace ui::icons
