// Home (the "Main" artboard): preview of the selected module above the 10-row module list.
// The navigator's root screen.
#pragma once
#include <memory>

#include "ui/nav.h"

namespace apps {

std::unique_ptr<ui::Screen> makeHome();

}  // namespace apps
