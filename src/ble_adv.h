// Bluetooth LE advertising data: the AD structures a device sends (Core Specification
// Supplement, part A), and what they say about it, for the Sensors screen's BLE list and
// detail (apps/ble_devices.h). Pure functions; no radio.
#pragma once
#include <stdint.h>

#include <string>
#include <vector>

namespace bleadv {

// One AD structure: its type byte and data (the length byte and the type left out). `at` is its
// offset in the payload, length byte included, for a hex dump.
struct Field {
  uint8_t type;
  const uint8_t *data;
  int len;
  int at;
};
// The structures in an advert or scan response, in order. Stops at a zero length or one that
// runs past the end.
std::vector<Field> fields(const uint8_t *payload, int len);
std::string fieldLabel(uint8_t type);  // "FLAGS", "UUID16", "NAME", "MAKER"; "TYPE 2A" if unknown

struct Info {
  std::string name;  // the complete or shortened local name, "" if none
  bool nameComplete = false;
  bool nameInResponse = false;
  int8_t txPower = 127;  // dBm; 127 if it doesn't say
  int appearance = -1;   // -1 if it doesn't say
  int flags = -1;
  std::vector<uint16_t> services;  // 16-bit service UUIDs, from the lists and service data
  int services128 = 0;             // 128-bit service UUIDs listed
  int company = -1;                // manufacturer data's company identifier
  std::vector<uint8_t> makerData;  // after the identifier
  // Decoded, for the rows: a short tag ("APPLE", "HID", "BEACON"; "" when nothing says) and what
  // it is ("NEARBY INFO", "HEART RATE", "iBEACON 1/4"; "" when nothing says).
  std::string kind;
  std::string what;
};
Info decode(const uint8_t *adv, int advLen, const uint8_t *rsp, int rspLen);

const char *companyName(int id);       // "NORDIC SEMICONDUCTOR"; nullptr if not in the short list
const char *serviceName(uint16_t uuid);  // "HEART RATE"; nullptr if not in the short list
std::string appearanceName(int appearance);  // "HEART RATE BELT", "WATCH"; "" for unknown
std::string flagsText(int flags);      // "LE GENERAL · NO BR/EDR"

}  // namespace bleadv
