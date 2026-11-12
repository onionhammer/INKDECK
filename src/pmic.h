// TG28 PMIC (AXP2101 register-compatible, I2C 0x34): battery/USB status for the status bar,
// the detail the System screen shows (ADC voltages, rails, charger settings, why the board
// last powered on and off), and the few settings configure() writes at boot.
#pragma once
#include <Print.h>
#include <stdint.h>

namespace pmic {

struct Status {
  bool ok = false;        // PMIC answered
  bool battery = false;   // battery detected
  int percent = -1;       // fuel gauge, 0-100; -1 without a battery
  bool vbus = false;      // USB power present
  bool charging = false;
  bool aldo1 = false;     // AudioCTR_VCC rail on (LDO enable register 0x90, bit 0)
};

// Writes the settings agreed for this board (references/power.md §2 compares them with the
// chip's defaults and Waveshare's axp_init()). Only the fields named here change; rails,
// cut-off voltage, input limits and everything else stay at the chip's defaults:
//   PWR key: 1 s press powers on, 4 s hold forces off      (0x27, chip default 128 ms / 6 s)
//   ADC: USB (VBUS) and system (VSYS) voltage channels on  (0x30, chip default battery only)
//   charging, for the 1500 mAh cell: precharge 50 mA, constant current 500 mA,
//   stop at 25 mA                                          (0x61-0x63, default 125/200/125 mA)
// Call once at boot, after Wire.begin(). Writes only registers that differ, and logs them.
void configure();

// Cached: reads the PMIC at most every few seconds. Begin Wire first.
Status status();

// Battery current direction, register 0x01 bits 6:5.
enum class Flow : uint8_t { Standby, Charging, Discharging };
// Charger state, register 0x01 bits 2:0.
enum class Charger : uint8_t { Trickle, Precharge, ConstantCurrent, ConstantVoltage, Done, Off };

struct Rail {
  bool on = false;
  int mv = 0;  // set voltage
};

struct Detail {
  bool ok = false;
  Status status;
  Flow flow = Flow::Standby;
  Charger charger = Charger::Off;
  int vbatMv = -1;  // ADC; -1 when its channel is off or there's no battery
  int vbusMv = -1;
  int vsysMv = -1;
  // Charger and power-path settings as the PMIC has them now.
  int chargeMa = 0;     // constant-current charge (0x62)
  int chargeToMv = 0;   // charge target voltage (0x64)
  int cutoffMv = 0;     // VSYS power-off threshold (0x24)
  int inputLimitMa = 0; // VBUS current limit (0x16)
  Rail dc1, aldo1, aldo2, aldo3;
};

// Uncached full read (about 30 registers).
Detail detail();

// Why the PMIC last powered on (0x20) and off (0x21), as short labels ("PWRON KEY/RTC",
// "VBUS", "VSYS LOW", ...). Empty when no bit is set or the PMIC didn't answer.
const char *powerOnSource();
const char *powerOffSource();

// Logs the configuration and status registers as hex, for checking against the datasheet.
void dump(Print &out);

}  // namespace pmic
