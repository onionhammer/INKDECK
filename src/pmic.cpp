// Register meanings: references/power.md §2 ("Status, battery and USB"), from XPowersLib's
// AXP2101 driver checked against docs/datasheets/TG28-PMIC.pdf. Section numbers below are the
// datasheet's (§6.13.2.x).
#include "pmic.h"

#include <Arduino.h>
#include <Wire.h>

#include "board_pins.h"

namespace pmic {
namespace {

constexpr uint8_t kRegStatus1 = 0x00;  // bit 5 VBUS good, bit 3 battery present (.1)
constexpr uint8_t kRegStatus2 = 0x01;  // 6:5 current direction, 2:0 charger state (.2)
constexpr uint8_t kRegCommon = 0x10;      // bit 0 soft power-off (.7)
constexpr uint8_t kRegInputLimit = 0x16;  // 2:0 VBUS current limit (.12)
constexpr uint8_t kRegPowerOn = 0x20;     // power-on source bits (.17)
constexpr uint8_t kRegPowerOff = 0x21;    // power-off source bits (.18)
constexpr uint8_t kRegKeyLevels = 0x27;   // 3:2 OFFLEVEL, 1:0 ONLEVEL (.24)
constexpr uint8_t kRegPrecharge = 0x61;   // 3:0 precharge current, 25 mA/step (.59)
constexpr uint8_t kRegTermination = 0x63;  // 4 termination on, 3:0 current, 25 mA/step (.61)
constexpr uint8_t kRegCutoff = 0x24;      // 2:0 VSYS power-off voltage, 2.6 V + 0.1 V/step (.21)
constexpr uint8_t kRegAdcEnable = 0x30;   // bit 0 VBAT, 2 VBUS, 3 VSYS (.29)
constexpr uint8_t kRegAdc = 0x34;         // 0x34..0x3B: VBAT, TS, VBUS, VSYS, 14 bit, 1 mV (6.10)
constexpr uint8_t kRegChargeCurrent = 0x62;  // 4:0 constant-current charge (.60)
constexpr uint8_t kRegChargeVoltage = 0x64;  // 2:0 charge target voltage (.62)
constexpr uint8_t kRegDcdcOn = 0x80;         // bit 0 DCDC1 (.68)
constexpr uint8_t kRegDc1Voltage = 0x82;     // 4:0, 1.5 V + 0.1 V/step (.70)
constexpr uint8_t kRegLdoOn = 0x90;          // bit 0 ALDO1 ... bit 3 ALDO4 (.75)
constexpr uint8_t kRegAldo1Voltage = 0x92;   // 0x92..0x94 ALDO1..3: 4:0, 0.5 V + 0.1 V/step
constexpr uint8_t kRegPercent = 0xA4;        // fuel gauge, 0-100 (.88)
constexpr uint32_t kCacheMs = 5000;

Status cached;
uint32_t readAt = 0;
bool everRead = false;

bool read(uint8_t reg, uint8_t *out, uint8_t n) {
  Wire.beginTransmission(board::I2C_ADDR_PMIC);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(board::I2C_ADDR_PMIC, n) != n) return false;
  for (uint8_t i = 0; i < n; ++i) out[i] = Wire.read();
  return true;
}

int read1(uint8_t reg) {
  uint8_t v;
  return read(reg, &v, 1) ? v : -1;
}

bool write1(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(board::I2C_ADDR_PMIC);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

Status readStatus() {
  Status s;
  uint8_t st[2];
  if (!read(kRegStatus1, st, 2)) return s;
  s.ok = true;
  s.vbus = st[0] & (1 << 5);
  s.battery = st[0] & (1 << 3);
  s.charging = ((st[1] >> 5) & 3) == 1;
  const int pct = s.battery ? read1(kRegPercent) : -1;
  if (pct >= 0 && pct <= 100) s.percent = pct;
  const int ldo = read1(kRegLdoOn);
  s.aldo1 = ldo >= 0 && (ldo & 1);
  return s;
}

int adc14(const uint8_t *hl) { return ((hl[0] & 0x3F) << 8) | hl[1]; }

// The first set bit's label, low bit first.
const char *firstBit(int value, const char *const labels[8]) {
  if (value <= 0) return "";
  for (int i = 0; i < 8; ++i) {
    if (value & (1 << i)) return labels[i];
  }
  return "";
}

}  // namespace

void configure() {
  struct Field {
    uint8_t reg;
    uint8_t mask;   // the bits this setting owns; the rest of the register is kept
    uint8_t value;  // already shifted into place
    const char *what;
  };
  static constexpr Field kFields[] = {
      {kRegKeyLevels, 0x0F, 0b0010, "PWR key: 1 s on, 4 s off"},  // OFFLEVEL 00, ONLEVEL 10
      {kRegAdcEnable, 0x0C, 0x0C, "ADC: VBUS and VSYS on"},
      {kRegPrecharge, 0x0F, 2, "precharge 50 mA"},                // 25 mA x 2
      {kRegChargeCurrent, 0x1F, 11, "charge 500 mA"},             // 200 + 100 x (11 - 8)
      {kRegTermination, 0x1F, 0x10 | 1, "stop charging at 25 mA"},  // enable + 25 mA x 1
  };
  for (const Field &f : kFields) {
    const int old = read1(f.reg);
    if (old < 0) {
      log_e("PMIC: can't read 0x%02X, not configured", f.reg);
      return;
    }
    const uint8_t want = (old & ~f.mask) | f.value;
    if (old == want) continue;
    if (!write1(f.reg, want) || read1(f.reg) != want) {
      log_e("PMIC: writing 0x%02X = 0x%02X (%s) failed", f.reg, want, f.what);
      continue;
    }
    log_i("PMIC: 0x%02X %02X -> %02X (%s)", f.reg, old, want, f.what);
  }
}

Status status() {
  if (everRead && millis() - readAt < kCacheMs) return cached;
  readAt = millis();
  everRead = true;

  const Status s = readStatus();
  if (s.ok != cached.ok || s.battery != cached.battery || s.vbus != cached.vbus) {
    log_i("PMIC: %s, battery %s (%d%%), USB %s, %s", s.ok ? "ok" : "no answer",
          s.battery ? "yes" : "no", s.percent, s.vbus ? "yes" : "no",
          s.charging ? "charging" : "not charging");
  }
  cached = s;
  return s;
}

Detail detail() {
  Detail d;
  d.status = readStatus();
  if (!d.status.ok) return d;
  d.ok = true;

  const int st2 = read1(kRegStatus2);
  if (st2 >= 0) {
    const int dir = (st2 >> 5) & 3;
    d.flow = dir == 1 ? Flow::Charging : dir == 2 ? Flow::Discharging : Flow::Standby;
    const int chg = st2 & 7;
    d.charger = chg <= 5 ? static_cast<Charger>(chg) : Charger::Off;
  }

  const int adcOn = read1(kRegAdcEnable);
  uint8_t adc[8];
  if (adcOn >= 0 && read(kRegAdc, adc, sizeof adc)) {
    if ((adcOn & 0x01) && d.status.battery) d.vbatMv = adc14(adc);
    if (adcOn & 0x04) d.vbusMv = adc14(adc + 4);
    if (adcOn & 0x08) d.vsysMv = adc14(adc + 6);
  }

  const int icc = read1(kRegChargeCurrent);
  if (icc >= 0) {
    const int n = icc & 0x1F;
    d.chargeMa = n <= 8 ? 25 * n : 200 + 100 * (n - 8);
  }
  static const int kChargeTo[] = {0, 4000, 4100, 4200, 4350, 4400, 0, 0};  // 000 undocumented
  const int cv = read1(kRegChargeVoltage);
  if (cv >= 0) d.chargeToMv = kChargeTo[cv & 7];
  const int off = read1(kRegCutoff);
  if (off >= 0) d.cutoffMv = 2600 + 100 * (off & 7);
  static const int kInput[] = {100, 500, 900, 1000, 1500, 2000, 0, 0};
  const int lim = read1(kRegInputLimit);
  if (lim >= 0) d.inputLimitMa = kInput[lim & 7];

  const int dcdc = read1(kRegDcdcOn);
  const int dc1 = read1(kRegDc1Voltage);
  d.dc1 = {dcdc >= 0 && (dcdc & 1), dc1 >= 0 ? 1500 + 100 * (dc1 & 0x1F) : 0};
  const int ldo = read1(kRegLdoOn);
  uint8_t aldo[3] = {};
  const bool haveAldo = read(kRegAldo1Voltage, aldo, 3);
  Rail *rails[] = {&d.aldo1, &d.aldo2, &d.aldo3};
  for (int i = 0; i < 3; ++i) {
    rails[i]->on = ldo >= 0 && (ldo & (1 << i));
    rails[i]->mv = haveAldo ? 500 + 100 * (aldo[i] & 0x1F) : 0;
  }
  return d;
}

const char *powerOnSource() {
  static const char *const kLabels[8] = {"PWRON KEY/RTC", "IRQ PIN", "USB PLUGGED IN",
                                         "BATTERY > 3.3 V", "BATTERY INSERTED", "EN PIN", "", ""};
  return firstBit(read1(kRegPowerOn), kLabels);
}

const char *powerOffSource() {
  static const char *const kLabels[8] = {"PWR KEY HELD", "SOFTWARE",        "EN PIN",
                                         "VSYS LOW",     "VBUS OVERVOLTAGE", "DCDC UNDERVOLTAGE",
                                         "DCDC OVERVOLTAGE", "OVERHEAT"};
  return firstBit(read1(kRegPowerOff), kLabels);
}

void dump(Print &out) {
  // Status, power-on/off, power path, ADC, charger, rails and gauge registers.
  static const uint8_t kRegs[] = {0x00, 0x01, 0x10, 0x12, 0x13, 0x14, 0x15, 0x16, 0x18, 0x19,
                                  0x1A, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x30,
                                  0x34, 0x35, 0x38, 0x39, 0x3A, 0x3B, 0x50, 0x58, 0x61, 0x62,
                                  0x63, 0x64, 0x65, 0x67, 0x68, 0x69, 0x6A, 0x80, 0x82, 0x90,
                                  0x91, 0x92, 0x93, 0x94, 0xA2, 0xA4};
  out.print("PMIC regs:");
  int col = 0;
  for (const uint8_t reg : kRegs) {
    const int v = read1(reg);
    if (col++ % 8 == 0) out.print("\n ");
    if (v < 0) {
      out.printf(" %02X=--", reg);
    } else {
      out.printf(" %02X=%02X", reg, v);
    }
  }
  out.println();
  const Detail d = detail();
  out.printf("PMIC: VBAT %d mV, VBUS %d mV, VSYS %d mV, charge %d mA to %d mV, cutoff %d mV, "
             "input limit %d mA, on: %s, last off: %s\n",
             d.vbatMv, d.vbusMv, d.vsysMv, d.chargeMa, d.chargeToMv, d.cutoffMv, d.inputLimitMa,
             powerOnSource(), powerOffSource());
}

int powerOnBits() { return read1(kRegPowerOn); }

bool powerOff() {
  const int v = read1(kRegCommon);
  if (v < 0) return false;
  log_w("PMIC: soft power-off");
  Serial.flush();
  return write1(kRegCommon, static_cast<uint8_t>(v | 0x01));
}

}  // namespace pmic
