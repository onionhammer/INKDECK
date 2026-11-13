#include "timesync.h"

#include <Arduino.h>
#include <Preferences.h>
#include <esp_netif_sntp.h>
#include <esp_timer.h>
#include <math.h>
#include <stdio.h>
#include <sys/time.h>
#include <time.h>

#include <atomic>
#include <mutex>

#include "rtc.h"
#include "wlan.h"

namespace timesync {
namespace {

constexpr uint32_t kConnectTimeoutMs = 20000;
// lwIP holds the first request back by a random 0-5 s (CONFIG_LWIP_SNTP_MAXIMUM_STARTUP_DELAY).
constexpr uint32_t kSntpTimeoutMs = 15000;

constexpr const char *kNamespace = "clock";
constexpr const char *kSyncedKey = "synced";

std::atomic<Phase> phase{Phase::Idle};
std::atomic<int64_t> lastSynced{-1};  // lastSync() cache; -1 = not read from NVS yet
std::mutex resultMutex;
std::string result;

void finish(Phase p, std::string text) {
  {
    std::lock_guard<std::mutex> lock(resultMutex);
    result = std::move(text);
  }
  phase = p;
}

double seconds(const timeval &tv) { return tv.tv_sec + tv.tv_usec / 1e6; }

// The SNTP query, on a connected network. esp_netif_sntp runs lwIP's SNTP in the TCP/IP task,
// so unlike Arduino's configTime() it needs no core locking and leaves TZ alone.
bool askNtp() {
  esp_sntp_config_t cfg = {};
  cfg.wait_for_sync = true;
  cfg.start = true;
  cfg.num_of_servers = 3;
  cfg.servers[0] = "pool.ntp.org";
  cfg.servers[1] = "time.cloudflare.com";
  cfg.servers[2] = "time.google.com";
  esp_err_t err = esp_netif_sntp_init(&cfg);
  if (err == ESP_OK) err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(kSntpTimeoutMs));
  esp_netif_sntp_deinit();
  if (err != ESP_OK) log_w("timesync: no NTP answer (%s)", esp_err_to_name(err));
  return err == ESP_OK;
}

void run(void *) {
  std::string error;
  if (!wlan::connect(kConnectTimeoutMs, error)) {
    finish(Phase::Failed, "WI-FI: " + error);
    vTaskDelete(nullptr);
    return;
  }

  phase = Phase::Asking;
  const bool wasValid = rtc::timeValid();
  timeval before;
  gettimeofday(&before, nullptr);
  const int64_t monoBefore = esp_timer_get_time();
  const bool synced = askNtp();
  wlan::disconnect();
  if (!synced) {
    finish(Phase::Failed, "NTP DIDN'T ANSWER");
    vTaskDelete(nullptr);
    return;
  }

  // How far SNTP stepped the clock: wall time gained minus real time elapsed. Positive means
  // the clock was behind.
  timeval after;
  gettimeofday(&after, nullptr);
  const double step =
      seconds(after) - seconds(before) - (esp_timer_get_time() - monoBefore) / 1e6;

  // Store it on a second boundary, so the RTC's seconds turn over close to the real ones.
  timeval now;
  gettimeofday(&now, nullptr);
  delay((1000000 - now.tv_usec) / 1000 + 1);
  const time_t utc = time(nullptr);
  const bool stored = rtc::set(utc);
  if (stored) {
    Preferences prefs;
    if (prefs.begin(kNamespace, false)) {
      prefs.putULong64(kSyncedKey, static_cast<uint64_t>(utc));
      prefs.end();
    }
    lastSynced = utc;
  }

  tm local;
  localtime_r(&utc, &local);
  char clock[16];
  strftime(clock, sizeof clock, "%H:%M %Z", &local);
  char drift[40] = "";
  if (wasValid) {
    const long s = lround(fabs(step));
    if (s == 0) {
      snprintf(drift, sizeof drift, " \xC2\xB7 WAS RIGHT");
    } else {
      snprintf(drift, sizeof drift, " \xC2\xB7 WAS %ld S %s", s, step > 0 ? "SLOW" : "FAST");
    }
  }
  log_i("timesync: %s, step %+.3f s%s", clock, step, stored ? "" : ", RTC write failed");
  finish(stored ? Phase::Done : Phase::Failed,
         stored ? std::string("TIME SET \xC2\xB7 ") + clock + drift
                : std::string("GOT ") + clock + " BUT THE RTC WRITE FAILED");
  vTaskDelete(nullptr);
}

}  // namespace

bool start() {
  Phase p = phase.load();
  if (p == Phase::Connecting || p == Phase::Asking) return false;
  phase = Phase::Connecting;
  // Wi-Fi and SNTP calls need more than the default 2 KB; 6 KB like the sensor sampler.
  if (xTaskCreate(run, "timesync", 6144, nullptr, 1, nullptr) != pdPASS) {
    finish(Phase::Failed, "NO MEMORY FOR THE SYNC TASK");
    return false;
  }
  return true;
}

time_t lastSync() {
  int64_t t = lastSynced.load();
  if (t < 0) {
    t = 0;
    Preferences prefs;
    // Read-write: a read-only open fails (and logs an error) until the namespace exists.
    if (prefs.begin(kNamespace, false)) {
      if (prefs.isKey(kSyncedKey)) t = static_cast<int64_t>(prefs.getULong64(kSyncedKey));
      prefs.end();
    }
    lastSynced = t;
  }
  return static_cast<time_t>(t);
}

Status status() {
  Status s;
  s.phase = phase.load();
  if (s.phase == Phase::Done || s.phase == Phase::Failed) {
    std::lock_guard<std::mutex> lock(resultMutex);
    s.result = result;
  }
  return s;
}

}  // namespace timesync
