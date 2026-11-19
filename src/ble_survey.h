// Bluetooth LE survey for the Sensors screen (apps/sensors.h, apps/ble_devices.h): the devices
// advertising nearby, from a continuous active scan on NimBLE (the host stack the core's
// ESP32-S3 libraries are built with). The S3 has no Bluetooth Classic.
//
// The stack runs only while someone uses it (begin()/end() pairs): the first begin() starts the
// controller and the host and scans until kLingerMs after the last end(); then both stop and
// their memory goes back to the heap. Every advert is reported (no duplicate filter), so a
// device's signal, its advertising interval and its last advert and scan response stay current.
//
// The core frees the controller's memory at boot unless something says BLE is in use; this
// module says so (bleInUse()), which keeps that memory reserved.
#pragma once
#include <stdint.h>

#include <string>
#include <vector>

namespace blesurvey {

struct Device {
  uint8_t addr[6] = {};  // most significant byte first, as printed
  uint8_t addrType = 0;  // 0 public, 1 random (NimBLE's BLE_ADDR_*)
  int8_t rssi = -127;    // smoothed over its recent adverts
  bool connectable = false;
  bool scannable = false;
  uint8_t adv[31] = {};
  uint8_t advLen = 0;
  uint8_t rsp[31] = {};  // its scan response, if it sent one
  uint8_t rspLen = 0;
  uint32_t firstMs = 0;  // millis() when first heard
  uint32_t heardMs = 0;  // and last
  uint32_t advMs = 0;    // when its last advert came (heardMs counts scan responses too)
  uint32_t adverts = 0;  // reports heard (scan responses not counted)
  uint32_t gapMs = 0;    // the shortest time seen between two of its adverts; 0 until two
};

constexpr int kMaxDevices = 64;        // past this, the one heard longest ago makes room
constexpr uint32_t kForgetMs = 60000;  // a device not heard this long is dropped
constexpr uint32_t kLingerMs = 4000;

void begin();  // a user; the first starts the stack and the scan
void end();

std::vector<Device> devices();  // heard in the last kForgetMs, strongest first
bool device(const uint8_t addr[6], uint8_t addrType, Device &out);
int count();         // devices() without the copy
bool running();      // the stack is up and scanning
std::string error();  // why it couldn't start ("STACK WOULDN'T START"); "" if it did

// "PUBLIC", "STATIC" (random static), "RPA" (resolvable private) or "NRPA" (non-resolvable).
const char *addressKind(const Device &d);
std::string addressText(const Device &d);  // "E8:4F:21:9C:2F:1A"
bool sameDevice(const Device &d, const uint8_t addr[6], uint8_t addrType);

}  // namespace blesurvey
