// TG28 PMIC (AXP2101 register-compatible, I2C 0x34): read-only battery/USB status for the
// status bar. Nothing is configured here; the vendor's charger and rail setup
// (references/power.md §2) arrives with the System screen.
#pragma once

namespace pmic {

struct Status {
  bool ok = false;        // PMIC answered
  bool battery = false;   // battery detected
  int percent = -1;       // fuel gauge, 0-100; -1 without a battery
  bool vbus = false;      // USB power present
  bool charging = false;
  bool aldo1 = false;     // AudioCTR_VCC rail on (LDO enable register 0x90, bit 0)
};

// Cached: reads the PMIC at most every few seconds. Begin Wire first.
Status status();

}  // namespace pmic
