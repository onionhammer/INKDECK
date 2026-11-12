// Firmware and chip facts for the System screen and Home's preview: version, boot reason,
// OTA slots, memory, uptime. Read-only.
#pragma once
#include <stdint.h>

#include <string>

namespace sysinfo {

inline constexpr const char *kFirmwareVersion = "0.1.0";

std::string idfVersion();  // "5.5.1"

// Why the chip started: the ESP32's reset reason, and for a cold start the PMIC's power-on
// source ("PWR KEY", "RTC ALARM", "USB PLUGGED IN") plus why it last powered off, if known.
std::string bootReason();

std::string otaState();  // "app0 active · app1 empty"
std::string memory();    // "heap 182 KB · PSRAM 7.6 MB free"
std::string wifi();      // "off · MAC 34:85:18:..."

uint32_t uptimeSeconds();
std::string uptime();  // "14M", "2H 14M", "3D 2H"

}  // namespace sysinfo
