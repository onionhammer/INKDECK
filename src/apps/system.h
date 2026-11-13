// System (the "System" artboard): battery (gauge, voltage, charger settings), the PMIC's power
// rails, system state (sleep, next RTC wake, boot reason, Wi-Fi, OTA slots, memory) and an
// action list.
//   ▲▼ move   ● run   BOOT back   (PWR held 4 s forces the board off, in the PMIC)
// Everything shown is read live from the PMIC, RTC and chip; nothing here writes the PMIC.
#pragma once
#include <memory>

#include "ui/nav.h"

namespace apps {

std::unique_ptr<ui::Screen> makeSystem();

}  // namespace apps
