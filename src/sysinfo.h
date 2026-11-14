// Firmware and chip facts for the System screen and Home's preview: version, boot reason,
// OTA slots, memory, uptime. Read-only.
#pragma once
#include <stdint.h>

#include <string>

namespace sysinfo {

inline constexpr const char *kFirmwareVersion = "0.1.0";

// The git commit the firmware was built from, "+" if the tree had uncommitted changes
// ("425d25c+"); scripts/gen_build_info.py. Tells you an update over Wi-Fi took.
const char *buildId();

std::string idfVersion();  // "5.5.1"

// Why the chip started: the ESP32's reset reason, and for a cold start the PMIC's power-on
// source ("PWR KEY", "RTC ALARM", "USB PLUGGED IN") plus why it last powered off, if known.
std::string bootReason();

// "app0 active · app1 empty"; after an update, "UNCONFIRMED" until setup() has kept it, or
// "ROLLED BACK FROM app1" if the new image failed and the bootloader came back to this one.
std::string otaState();
std::string memory();    // "heap 182 KB · PSRAM 7.6 MB free"
std::string wifi();      // wlan::describe() with a .env network, else "off · MAC 34:85:18:..."

uint32_t uptimeSeconds();
std::string uptime();  // "14M", "2H 14M", "3D 2H"

}  // namespace sysinfo
