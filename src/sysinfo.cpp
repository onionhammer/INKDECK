#include "sysinfo.h"

#include <esp_app_desc.h>
#include <esp_heap_caps.h>
#include <esp_idf_version.h>
#include <esp_mac.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <stdio.h>
#include <string.h>

#include "pmic.h"
#include "rtc.h"
#include "wlan.h"

namespace sysinfo {

std::string idfVersion() {
  char buf[16];
  snprintf(buf, sizeof buf, "%d.%d.%d", ESP_IDF_VERSION_MAJOR, ESP_IDF_VERSION_MINOR,
           ESP_IDF_VERSION_PATCH);
  return buf;
}

std::string bootReason() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: {
      std::string on = pmic::powerOnSource();
      if (on == "PWRON KEY/RTC") on = rtc::alarm().fired ? "RTC ALARM" : "PWR KEY";
      if (on.empty()) on = "POWER ON";
      const std::string off = pmic::powerOffSource();
      return off.empty() ? on : on + " \xC2\xB7 OFF: " + off;
    }
    case ESP_RST_SW: return "SOFTWARE RESET";
    case ESP_RST_USB: return "USB RESET";
    case ESP_RST_PANIC: return "CRASH";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT: return "WATCHDOG";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_DEEPSLEEP: return "DEEP-SLEEP WAKE";
    case ESP_RST_EXT: return "EXTERNAL RESET";
    case ESP_RST_JTAG: return "JTAG RESET";
    default: return "RESET " + std::to_string(static_cast<int>(esp_reset_reason()));
  }
}

std::string otaState() {
  const esp_partition_t *run = esp_ota_get_running_partition();
  const esp_partition_t *other = esp_ota_get_next_update_partition(nullptr);
  if (!run) return "?";
  std::string s = std::string(run->label) + " active";
  if (other) {
    esp_app_desc_t desc;
    const bool image = esp_ota_get_partition_description(other, &desc) == ESP_OK;
    s += std::string(" \xC2\xB7 ") + other->label + (image ? " idle" : " empty");
  }
  return s;
}

std::string memory() {
  const size_t heap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  const size_t psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  char buf[48];
  snprintf(buf, sizeof buf, "heap %u KB \xC2\xB7 PSRAM %.1f MB free",
           static_cast<unsigned>(heap / 1024), psram / 1048576.0);
  return buf;
}

std::string wifi() {
  // With a network from .env, its state; the System row has no room for the MAC as well.
  if (wlan::configured()) return wlan::describe();
  uint8_t mac[6];
  if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) return "off";
  char buf[40];
  snprintf(buf, sizeof buf, "off \xC2\xB7 MAC %02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1],
           mac[2], mac[3], mac[4], mac[5]);
  return buf;
}

uint32_t uptimeSeconds() { return static_cast<uint32_t>(esp_timer_get_time() / 1000000); }

std::string uptime() {
  const uint32_t m = uptimeSeconds() / 60;
  char buf[16];
  if (m < 60) {
    snprintf(buf, sizeof buf, "%uM", static_cast<unsigned>(m));
  } else if (m < 24 * 60) {
    snprintf(buf, sizeof buf, "%uH %uM", static_cast<unsigned>(m / 60),
             static_cast<unsigned>(m % 60));
  } else {
    snprintf(buf, sizeof buf, "%uD %uH", static_cast<unsigned>(m / 1440),
             static_cast<unsigned>(m / 60 % 24));
  }
  return buf;
}

}  // namespace sysinfo
