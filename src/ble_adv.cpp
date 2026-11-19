#include "ble_adv.h"
#include "features.h"

#if INKDECK_BLE

#include <stdio.h>

#include <algorithm>

namespace bleadv {
namespace {

// AD types (Bluetooth Assigned Numbers, "Common Data Types").
constexpr uint8_t kFlags = 0x01;
constexpr uint8_t kUuid16Some = 0x02;
constexpr uint8_t kUuid16All = 0x03;
constexpr uint8_t kUuid32Some = 0x04;
constexpr uint8_t kUuid32All = 0x05;
constexpr uint8_t kUuid128Some = 0x06;
constexpr uint8_t kUuid128All = 0x07;
constexpr uint8_t kNameShort = 0x08;
constexpr uint8_t kNameComplete = 0x09;
constexpr uint8_t kTxPower = 0x0A;
constexpr uint8_t kServiceData16 = 0x16;
constexpr uint8_t kAppearance = 0x19;
constexpr uint8_t kMaker = 0xFF;

constexpr int kApple = 0x004C;
constexpr int kMicrosoft = 0x0006;

struct Company {
  uint16_t id;
  const char *name;
  const char *tag;
};
// A few of the Bluetooth SIG's company identifiers: the chip makers and the brands seen most.
constexpr Company kCompanies[] = {
    {0x0001, "NOKIA", "NOKIA"},
    {0x0002, "INTEL", "INTEL"},
    {0x0003, "IBM", "IBM"},
    {0x0004, "TOSHIBA", "TOSHBA"},
    {0x0006, "MICROSOFT", "MSFT"},
    {0x0008, "MOTOROLA", "MOTO"},
    {0x0009, "INFINEON", "INFNON"},
    {0x000A, "QUALCOMM (CSR)", "QCOM"},
    {0x000D, "TEXAS INSTRUMENTS", "TI"},
    {0x000F, "BROADCOM", "BRCM"},
    {0x001D, "QUALCOMM", "QCOM"},
    {0x0046, "MEDIATEK", "MTK"},
    {0x004C, "APPLE", "APPLE"},
    {0x0057, "HARMAN", "HARMAN"},
    {0x0059, "NORDIC SEMICONDUCTOR", "NORDIC"},
    {0x0075, "SAMSUNG", "SAMSNG"},
    {0x0087, "GARMIN", "GARMIN"},
    {0x009E, "BOSE", "BOSE"},
    {0x00C4, "LG ELECTRONICS", "LG"},
    {0x00E0, "GOOGLE", "GOOGLE"},
    {0x012D, "SONY", "SONY"},
    {0x0157, "HUAMI", "HUAMI"},
    {0x0171, "AMAZON", "AMAZON"},
    {0x027D, "HUAWEI", "HUAWEI"},
    {0x02E5, "ESPRESSIF", "ESP"},
    {0x038F, "XIAOMI", "XIAOMI"},
    {0x0499, "RUUVI", "RUUVI"},
};

const Company *company(int id) {
  for (const Company &c : kCompanies) {
    if (c.id == id) return &c;
  }
  return nullptr;
}

struct Service {
  uint16_t uuid;
  const char *name;
  const char *tag;  // what the service says the device is, or nullptr
};
// SIG-assigned 16-bit services, and member UUIDs (0xFCxx-0xFExx) of well-known protocols.
constexpr Service kServices[] = {
    {0x1800, "GENERIC ACCESS", nullptr},
    {0x1801, "GENERIC ATTRIBUTE", nullptr},
    {0x1802, "IMMEDIATE ALERT", "TAG"},
    {0x1803, "LINK LOSS", nullptr},
    {0x1804, "TX POWER", nullptr},
    {0x1805, "CURRENT TIME", nullptr},
    {0x1808, "GLUCOSE", "HEALTH"},
    {0x1809, "THERMOMETER", "SENSOR"},
    {0x180A, "DEVICE INFO", nullptr},
    {0x180D, "HEART RATE", "HR"},
    {0x180F, "BATTERY", nullptr},
    {0x1810, "BLOOD PRESSURE", "HEALTH"},
    {0x1812, "HID", "HID"},
    {0x1814, "RUNNING SPEED", "SPORT"},
    {0x1816, "CYCLING SPEED", "BIKE"},
    {0x1818, "CYCLING POWER", "BIKE"},
    {0x1819, "LOCATION", nullptr},
    {0x181A, "ENV SENSING", "SENSOR"},
    {0x181B, "BODY COMPOSITION", "HEALTH"},
    {0x181D, "WEIGHT SCALE", "HEALTH"},
    {0x1822, "PULSE OXIMETER", "HEALTH"},
    {0x1826, "FITNESS MACHINE", "SPORT"},
    {0x1844, "VOLUME CONTROL", "AUDIO"},
    {0x184E, "LE AUDIO STREAM", "AUDIO"},
    {0x184F, "LE AUDIO BROADCAST", "AUDIO"},
    {0x1850, "LE AUDIO", "AUDIO"},
    {0x1852, "LE AUDIO BROADCAST", "AUDIO"},
    {0x1853, "LE AUDIO", "AUDIO"},
    {0xFCD2, "BTHOME", "SENSOR"},
    {0xFD6F, "EXPOSURE NOTIF", "EN"},
    {0xFE2C, "FAST PAIR", "GOOGLE"},
    {0xFE95, "MIBEACON", "XIAOMI"},
    {0xFE9F, "GOOGLE", "GOOGLE"},
    {0xFEAA, "EDDYSTONE", "BEACON"},
    {0xFEEC, "TILE", "TILE"},
    {0xFEED, "TILE", "TILE"},
    {0xFEF3, "GOOGLE", "GOOGLE"},
};

const Service *service(uint16_t uuid) {
  for (const Service &s : kServices) {
    if (s.uuid == uuid) return &s;
  }
  return nullptr;
}

// Appearance categories (the value's top 10 bits).
const char *category(int cat) {
  switch (cat) {
    case 0x01: return "PHONE";
    case 0x02: return "COMPUTER";
    case 0x03: return "WATCH";
    case 0x04: return "CLOCK";
    case 0x05: return "DISPLAY";
    case 0x06: return "REMOTE CONTROL";
    case 0x07: return "EYE-GLASSES";
    case 0x08: return "TAG";
    case 0x09: return "KEYRING";
    case 0x0A: return "MEDIA PLAYER";
    case 0x0B: return "BARCODE SCANNER";
    case 0x0C: return "THERMOMETER";
    case 0x0D: return "HEART RATE SENSOR";
    case 0x0E: return "BLOOD PRESSURE";
    case 0x0F: return "HID";
    case 0x10: return "GLUCOSE METER";
    case 0x11: return "RUNNING SENSOR";
    case 0x12: return "CYCLING";
    case 0x13: return "CONTROL DEVICE";
    case 0x14: return "NETWORK DEVICE";
    case 0x15: return "SENSOR";
    case 0x16: return "LIGHT FIXTURE";
    case 0x17: return "FAN";
    case 0x18: return "HVAC";
    case 0x19: return "AIR CONDITIONING";
    case 0x1A: return "HUMIDIFIER";
    case 0x1B: return "HEATING";
    case 0x1C: return "ACCESS CONTROL";
    case 0x1D: return "MOTORIZED DEVICE";
    case 0x1E: return "POWER DEVICE";
    case 0x1F: return "LIGHT SOURCE";
    case 0x20: return "WINDOW COVERING";
    case 0x21: return "AUDIO SINK";
    case 0x22: return "AUDIO SOURCE";
    case 0x23: return "MOTORIZED VEHICLE";
    case 0x24: return "DOMESTIC APPLIANCE";
    case 0x25: return "WEARABLE AUDIO";
    case 0x26: return "AIRCRAFT";
    case 0x27: return "AV EQUIPMENT";
    case 0x28: return "DISPLAY EQUIPMENT";
    case 0x29: return "HEARING AID";
    case 0x2A: return "GAMING";
    case 0x2B: return "SIGNAGE";
    case 0x31: return "PULSE OXIMETER";
    case 0x32: return "WEIGHT SCALE";
    case 0x33: return "PERSONAL MOBILITY";
    case 0x34: return "GLUCOSE MONITOR";
    case 0x35: return "INSULIN PUMP";
    case 0x36: return "MEDICATION DELIVERY";
    case 0x37: return "SPIROMETER";
    case 0x51: return "OUTDOOR SPORTS";
    default: return nullptr;
  }
}

// The tag an appearance category suggests, or nullptr.
const char *categoryTag(int cat) {
  switch (cat) {
    case 0x01: return "PHONE";
    case 0x02: return "PC";
    case 0x03: return "WATCH";
    case 0x05:
    case 0x28: return "SCREEN";
    case 0x06: return "REMOTE";
    case 0x08:
    case 0x09: return "TAG";
    case 0x0A:
    case 0x21:
    case 0x22:
    case 0x25: return "AUDIO";
    case 0x0C:
    case 0x15: return "SENSOR";
    case 0x0D: return "HR";
    case 0x0E:
    case 0x10:
    case 0x29:
    case 0x31:
    case 0x32:
    case 0x34:
    case 0x35:
    case 0x36:
    case 0x37: return "HEALTH";
    case 0x0F: return "HID";
    case 0x11:
    case 0x51: return "SPORT";
    case 0x12: return "BIKE";
    case 0x16:
    case 0x1F: return "LIGHT";
    case 0x2A: return "GAME";
    default: return nullptr;
  }
}

std::string hex4(int v) {
  char buf[8];
  snprintf(buf, sizeof buf, "%04X", v & 0xFFFF);
  return buf;
}

// Apple's Continuity messages in its manufacturer data: type, length, data, repeated.
void decodeApple(const std::vector<uint8_t> &d, Info &info) {
  info.kind = "APPLE";
  if (d.size() < 2) return;
  const uint8_t type = d[0];
  if (type == 0x02 && d[1] == 0x15 && d.size() >= 23) {  // iBeacon: uuid, major, minor, power
    info.kind = "BEACON";
    const int major = d[18] << 8 | d[19];
    const int minor = d[20] << 8 | d[21];
    info.what = "iBEACON " + std::to_string(major) + "/" + std::to_string(minor);
    return;
  }
  switch (type) {
    case 0x05: info.what = "AIRDROP"; break;
    case 0x07: info.what = "PROXIMITY PAIRING"; break;
    case 0x09: info.what = "AIRPLAY TARGET"; break;
    case 0x0A: info.what = "AIRPLAY SOURCE"; break;
    case 0x0B: info.what = "MAGIC SWITCH"; break;
    case 0x0C: info.what = "HANDOFF"; break;
    case 0x0D: info.what = "HOTSPOT TARGET"; break;
    case 0x0E: info.what = "INSTANT HOTSPOT"; break;
    case 0x0F: info.what = "NEARBY ACTION"; break;
    case 0x10: info.what = "NEARBY INFO"; break;
    case 0x12: info.what = "FIND MY"; break;
    default: info.what = "CONTINUITY " + hex4(type).substr(2); break;
  }
}

void decodeMicrosoft(const std::vector<uint8_t> &d, Info &info) {
  info.kind = "MSFT";
  if (d.empty()) return;
  if (d[0] == 0x01) info.what = "CONNECTED DEVICES";
  if (d[0] == 0x03) info.what = "SWIFT PAIR";
}

void addService(Info &info, uint16_t uuid) {
  if (std::find(info.services.begin(), info.services.end(), uuid) == info.services.end()) {
    info.services.push_back(uuid);
  }
}

void scan(const uint8_t *payload, int len, bool response, Info &info, int &eddystoneFrame) {
  for (const Field &f : fields(payload, len)) {
    switch (f.type) {
      case kFlags:
        if (f.len >= 1) info.flags = f.data[0];
        break;
      case kUuid16Some:
      case kUuid16All:
        for (int i = 0; i + 1 < f.len; i += 2) addService(info, f.data[i] | f.data[i + 1] << 8);
        break;
      case kUuid128Some:
      case kUuid128All: info.services128 += f.len / 16; break;
      case kNameShort:
      case kNameComplete:
        // A complete name beats a shortened one, wherever each came from.
        if (info.name.empty() || (f.type == kNameComplete && !info.nameComplete)) {
          info.name.assign(reinterpret_cast<const char *>(f.data), f.len);
          info.nameComplete = f.type == kNameComplete;
          info.nameInResponse = response;
        }
        break;
      case kTxPower:
        if (f.len >= 1) info.txPower = static_cast<int8_t>(f.data[0]);
        break;
      case kServiceData16:
        if (f.len >= 2) {
          const uint16_t uuid = f.data[0] | f.data[1] << 8;
          addService(info, uuid);
          if (uuid == 0xFEAA && f.len >= 3) eddystoneFrame = f.data[2];
        }
        break;
      case kAppearance:
        if (f.len >= 2) info.appearance = f.data[0] | f.data[1] << 8;
        break;
      case kMaker:
        if (f.len >= 2 && info.company < 0) {
          info.company = f.data[0] | f.data[1] << 8;
          info.makerData.assign(f.data + 2, f.data + f.len);
        }
        break;
      default: break;
    }
  }
}

}  // namespace

std::vector<Field> fields(const uint8_t *payload, int len) {
  std::vector<Field> out;
  int at = 0;
  while (at < len) {
    const int n = payload[at];
    if (n == 0 || at + 1 + n > len) break;
    out.push_back({payload[at + 1], payload + at + 2, n - 1, at});
    at += 1 + n;
  }
  return out;
}

std::string fieldLabel(uint8_t type) {
  switch (type) {
    case kFlags: return "FLAGS";
    case kUuid16Some:
    case kUuid16All: return "UUID16";
    case kUuid32Some:
    case kUuid32All: return "UUID32";
    case kUuid128Some:
    case kUuid128All: return "UUID128";
    case kNameShort:
    case kNameComplete: return "NAME";
    case kTxPower: return "TX PWR";
    case kServiceData16: return "SVC DATA";
    case 0x20: return "SVC DATA";
    case 0x21: return "SVC DATA";
    case kAppearance: return "APPEAR";
    case 0x1B: return "LE ADDR";
    case 0x24: return "URI";
    case kMaker: return "MAKER";
    default: return "TYPE " + hex4(type).substr(2);
  }
}

Info decode(const uint8_t *adv, int advLen, const uint8_t *rsp, int rspLen) {
  Info info;
  int eddystone = -1;
  scan(adv, advLen, false, info, eddystone);
  scan(rsp, rspLen, true, info, eddystone);

  // Protocols first: they say the most.
  if (info.company == kApple) {
    decodeApple(info.makerData, info);
  } else if (info.company == kMicrosoft) {
    decodeMicrosoft(info.makerData, info);
  }
  if (info.kind.empty() || info.what.empty()) {
    for (const uint16_t uuid : info.services) {
      if (uuid == 0xFEAA) {
        info.kind = "BEACON";
        info.what = eddystone == 0x00   ? "EDDYSTONE UID"
                    : eddystone == 0x10 ? "EDDYSTONE URL"
                    : eddystone == 0x20 ? "EDDYSTONE TLM"
                    : eddystone == 0x30 ? "EDDYSTONE EID"
                                        : "EDDYSTONE";
        break;
      }
      const Service *s = service(uuid);
      if (s && uuid >= 0xFC00) {  // a member UUID: the protocol is the point
        if (info.kind.empty() && s->tag) info.kind = s->tag;
        if (info.what.empty()) info.what = s->name;
        break;
      }
    }
  }
  // Then what the device says it is: its appearance, then its services.
  const int cat = info.appearance >= 0 ? info.appearance >> 6 : 0;
  if (cat != 0) {
    if (info.kind.empty() && categoryTag(cat)) info.kind = categoryTag(cat);
    if (info.what.empty()) info.what = appearanceName(info.appearance);
  }
  for (const uint16_t uuid : info.services) {
    const Service *s = service(uuid);
    if (!s || !s->tag) continue;
    if (info.kind.empty()) info.kind = s->tag;
    if (info.what.empty()) info.what = s->name;
    break;
  }
  // Then who made it.
  if (const Company *c = company(info.company)) {
    if (info.kind.empty()) info.kind = c->tag;
    if (info.what.empty()) info.what = c->name;
  }
  return info;
}

const char *companyName(int id) {
  const Company *c = company(id);
  return c ? c->name : nullptr;
}

const char *serviceName(uint16_t uuid) {
  const Service *s = service(uuid);
  return s ? s->name : nullptr;
}

std::string appearanceName(int appearance) {
  switch (appearance) {
    case 0x00C1: return "SPORTS WATCH";
    case 0x00C2: return "SMARTWATCH";
    case 0x0341: return "HEART RATE BELT";
    case 0x03C1: return "KEYBOARD";
    case 0x03C2: return "MOUSE";
    case 0x03C3: return "JOYSTICK";
    case 0x03C4: return "GAMEPAD";
    case 0x0841: return "SPEAKER";
    case 0x0941: return "EARBUD";
    case 0x0942: return "HEADSET";
    case 0x0943: return "HEADPHONES";
    default: break;
  }
  const char *c = appearance > 0 ? category(appearance >> 6) : nullptr;
  return c ? c : "";
}

std::string flagsText(int flags) {
  if (flags < 0) return "";
  std::string s;
  auto add = [&](const char *t) {
    if (!s.empty()) s += " \xC2\xB7 ";
    s += t;
  };
  if (flags & 0x01) add("LE LIMITED");
  if (flags & 0x02) add("LE GENERAL");
  if (flags & 0x04) add("NO BR/EDR");
  if (flags & 0x18) add("LE + BR/EDR");
  if (s.empty()) add("NOT DISCOVERABLE");
  return s;
}

}  // namespace bleadv

#endif  // INKDECK_BLE
