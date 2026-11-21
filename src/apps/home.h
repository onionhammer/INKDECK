// Home (the "Main" artboard): preview of the selected module above the module list (12 rows).
// The navigator's root screen.
#pragma once
#include <memory>
#include <string>

#include "ui/nav.h"

namespace apps {

std::unique_ptr<ui::Screen> makeHome();

// A module's status-bar tag with its place on Home: moduleTag("CLOCK") is "05 CLOCK". Screens
// use it, so reordering Home renumbers them all.
std::string moduleTag(const char *label);

}  // namespace apps
