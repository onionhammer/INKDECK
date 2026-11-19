#include "ble_survey.h"
#include "features.h"

#if INKDECK_BLE

#include <Arduino.h>
#include <host/ble_gap.h>
#include <host/ble_hs.h>
#include <nimble/nimble_port.h>
#include <nimble/nimble_port_freertos.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <mutex>

// Keep the BLE controller's memory: without this the core releases it at boot (initArduino()),
// and the controller can't start until the next one.
extern "C" bool bleInUse(void) { return true; }

namespace blesurvey {
namespace {

// Continuous scanning: the window fills the interval, so the radio listens all the time
// (rotating over the three advertising channels).
constexpr uint16_t kScanInterval = 0x50;  // 50 ms, in 0.625 ms units
constexpr uint32_t kSyncTimeoutMs = 3000;

std::mutex mutex;  // guards the table and the users
std::vector<Device> table;
int users = 0;
uint32_t lastUserGoneMs = 0;
std::string lastError;
TaskHandle_t worker = nullptr;

std::atomic<bool> synced{false};
std::atomic<bool> up{false};
std::atomic<bool> discovering{false};
uint8_t ownAddrType = BLE_OWN_ADDR_PUBLIC;

void wake() {
  if (worker) xTaskNotifyGive(worker);
}

void onSync() { synced = true; }
void onReset(int reason) {
  synced = false;
  log_w("blesurvey: host reset (%d)", reason);
}

void hostTask(void *) {
  nimble_port_run();  // returns once nimble_port_stop() runs
  nimble_port_freertos_deinit();
}

// In NimBLE's host task, for every advert and scan response.
void record(const ble_gap_disc_desc &d) {
  const bool response = d.event_type == BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP;
  uint8_t addr[6];
  for (int i = 0; i < 6; ++i) addr[i] = d.addr.val[5 - i];
  const uint8_t type = d.addr.type;
  const uint32_t now = millis();
  const int len = std::min<int>(d.length_data, 31);

  std::lock_guard<std::mutex> lock(mutex);
  auto it = std::find_if(table.begin(), table.end(),
                         [&](const Device &x) { return sameDevice(x, addr, type); });
  if (it == table.end()) {
    if (static_cast<int>(table.size()) >= kMaxDevices) {
      it = std::min_element(table.begin(), table.end(), [](const Device &a, const Device &b) {
        return a.heardMs < b.heardMs;
      });
      *it = Device();
    } else {
      table.emplace_back();
      it = table.end() - 1;
    }
    memcpy(it->addr, addr, 6);
    it->addrType = type;
    it->firstMs = now;
    it->rssi = d.rssi;
  }
  Device &dev = *it;
  dev.heardMs = now;
  if (d.rssi != 127) dev.rssi = static_cast<int8_t>(lroundf(dev.rssi * 0.7f + d.rssi * 0.3f));
  if (response) {
    memcpy(dev.rsp, d.data, len);
    dev.rspLen = len;
    return;
  }
  memcpy(dev.adv, d.data, len);
  dev.advLen = len;
  dev.connectable = d.event_type == BLE_HCI_ADV_RPT_EVTYPE_ADV_IND ||
                    d.event_type == BLE_HCI_ADV_RPT_EVTYPE_DIR_IND;
  dev.scannable = d.event_type == BLE_HCI_ADV_RPT_EVTYPE_ADV_IND ||
                  d.event_type == BLE_HCI_ADV_RPT_EVTYPE_SCAN_IND;
  ++dev.adverts;
  if (dev.advMs != 0) {
    const uint32_t gap = now - dev.advMs;
    if (gap >= 15 && (dev.gapMs == 0 || gap < dev.gapMs)) dev.gapMs = gap;  // 20 ms is the least allowed
  }
  dev.advMs = now;
}

int onGap(ble_gap_event *event, void *) {
  switch (event->type) {
    case BLE_GAP_EVENT_DISC: record(event->disc); break;
    case BLE_GAP_EVENT_DISC_COMPLETE:
      discovering = false;
      wake();
      break;
    default: break;
  }
  return 0;
}

bool startStack(std::string &error) {
  synced = false;
  const esp_err_t e = nimble_port_init();
  if (e != ESP_OK) {
    log_e("blesurvey: nimble_port_init: %s", esp_err_to_name(e));
    error = "STACK WOULDN'T START";
    return false;
  }
  ble_hs_cfg.sync_cb = onSync;
  ble_hs_cfg.reset_cb = onReset;
  nimble_port_freertos_init(hostTask);
  const uint32_t start = millis();
  while (!synced && millis() - start < kSyncTimeoutMs) delay(10);
  if (!synced) {
    log_e("blesurvey: host didn't sync");
    nimble_port_stop();
    nimble_port_deinit();
    error = "CONTROLLER DIDN'T ANSWER";
    return false;
  }
  if (ble_hs_id_infer_auto(0, &ownAddrType) != 0) ownAddrType = BLE_OWN_ADDR_PUBLIC;
  up = true;
  log_i("blesurvey: stack up in %lu ms, internal heap %u", static_cast<unsigned long>(millis() - start),
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
  return true;
}

void stopStack() {
  if (discovering) ble_gap_disc_cancel();
  discovering = false;
  up = false;
  nimble_port_stop();
  nimble_port_deinit();
  synced = false;
  log_i("blesurvey: stack down, internal heap %u",
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
}

bool startScan() {
  ble_gap_disc_params params = {};
  params.itvl = kScanInterval;
  params.window = kScanInterval;
  params.passive = 0;  // ask for scan responses: that's where many names are
  params.filter_duplicates = 0;
  const int rc = ble_gap_disc(ownAddrType, BLE_HS_FOREVER, &params, onGap, nullptr);
  if (rc != 0 && rc != BLE_HS_EALREADY) {
    log_w("blesurvey: ble_gap_disc: %d", rc);
    return false;
  }
  discovering = true;
  return true;
}

void prune() {
  const uint32_t now = millis();
  std::lock_guard<std::mutex> lock(mutex);
  table.erase(std::remove_if(table.begin(), table.end(),
                             [&](const Device &d) { return now - d.heardMs > kForgetMs; }),
              table.end());
}

void run(void *) {
  uint32_t failedAt = 0;
  for (;;) {
    bool wanted;
    uint32_t gone;
    {
      std::lock_guard<std::mutex> lock(mutex);
      wanted = users > 0;
      gone = lastUserGoneMs;
    }
    if (wanted && !up && (failedAt == 0 || millis() - failedAt > 10000)) {
      std::string error;
      const bool ok = startStack(error);
      failedAt = ok ? 0 : millis();
      std::lock_guard<std::mutex> lock(mutex);
      lastError = error;
    }
    if (wanted && up && !discovering && !startScan()) {
      std::lock_guard<std::mutex> lock(mutex);
      lastError = "SCAN WOULDN'T START";
    }
    if (!wanted && up && millis() - gone >= kLingerMs) stopStack();
    prune();
    if (!wanted && !up) {
      // Nobody left and the stack is down: the task ends, so its stack doesn't sit in the
      // internal heap between uses. begin() starts another.
      std::lock_guard<std::mutex> lock(mutex);
      if (users == 0) {
        worker = nullptr;
        break;
      }
      continue;
    }
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
  }
  vTaskDelete(nullptr);
}

}  // namespace

void begin() {
  std::lock_guard<std::mutex> lock(mutex);
  ++users;
  if (!worker) xTaskCreate(run, "blesurvey", 4096, nullptr, 1, &worker);
  wake();
}

void end() {
  std::lock_guard<std::mutex> lock(mutex);
  if (users == 0) return;
  if (--users == 0) lastUserGoneMs = millis();
  wake();
}

std::vector<Device> devices() {
  std::vector<Device> out;
  {
    const uint32_t now = millis();
    std::lock_guard<std::mutex> lock(mutex);
    for (const Device &d : table) {
      if (now - d.heardMs <= kForgetMs) out.push_back(d);
    }
  }
  std::stable_sort(out.begin(), out.end(),
                   [](const Device &a, const Device &b) { return a.rssi > b.rssi; });
  return out;
}

bool device(const uint8_t addr[6], uint8_t addrType, Device &out) {
  std::lock_guard<std::mutex> lock(mutex);
  for (const Device &d : table) {
    if (sameDevice(d, addr, addrType)) {
      out = d;
      return true;
    }
  }
  return false;
}

int count() {
  const uint32_t now = millis();
  std::lock_guard<std::mutex> lock(mutex);
  return static_cast<int>(std::count_if(table.begin(), table.end(), [&](const Device &d) {
    return now - d.heardMs <= kForgetMs;
  }));
}

bool running() { return up && discovering; }

std::string error() {
  std::lock_guard<std::mutex> lock(mutex);
  return lastError;
}

const char *addressKind(const Device &d) {
  if ((d.addrType & 1) == 0) return "PUBLIC";
  switch (d.addr[0] >> 6) {
    case 3: return "STATIC";
    case 1: return "RPA";
    case 0: return "NRPA";
    default: return "RANDOM";
  }
}

std::string addressText(const Device &d) {
  char buf[18];
  snprintf(buf, sizeof buf, "%02X:%02X:%02X:%02X:%02X:%02X", d.addr[0], d.addr[1], d.addr[2],
           d.addr[3], d.addr[4], d.addr[5]);
  return buf;
}

bool sameDevice(const Device &d, const uint8_t addr[6], uint8_t addrType) {
  return d.addrType == addrType && memcmp(d.addr, addr, 6) == 0;
}

}  // namespace blesurvey

#endif  // INKDECK_BLE
