#include "ota.h"

#include <Arduino.h>
#include <WiFi.h>
#include <cJSON.h>
#include <ctype.h>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <mbedtls/sha256.h>

#include <atomic>
#include <mutex>

#include "audio.h"
#include "json_fields.h"
#include "netget.h"
#include "sysinfo.h"
#include "wlan.h"

// The core marks a new image good in initArduino(), before setup(), unless this says the app
// will do it (esp32-hal-misc.c). confirmBoot() does, once setup() has run.
extern "C" bool verifyRollbackLater() { return true; }

namespace ota {
namespace {

constexpr uint32_t kConnectTimeoutMs = 20000;
constexpr uint32_t kShownWaitMs = 6000;  // for the DONE frame; a full refresh takes about 3 s
constexpr size_t kMaxManifest = 4096;
constexpr size_t kChunk = 4096;
constexpr const char *kUserAgent = "INKDECK-updater/1.0";

enum class Job : uint8_t { Check, Install };

std::atomic<Phase> phase{Phase::Idle};
std::atomic<bool> running{false};  // the task exists
std::atomic<bool> stopRequested{false};
std::atomic<bool> shown{false};
std::atomic<uint32_t> bytes{0};
std::atomic<Job> job{Job::Check};
std::mutex textMutex;  // guards the rest
std::string resultText;
std::string devManifestUrl;
Release latest;
int latestOrder = 0;

void fail(std::string why) {
  log_w("ota: %s", why.c_str());
  {
    std::lock_guard<std::mutex> lock(textMutex);
    resultText = std::move(why);
  }
  phase = Phase::Failed;
}

std::string upper(const char *s) {
  std::string out = s ? s : "";
  for (char &c : out) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
  return out;
}

// --- Versions ----------------------------------------------------------------------------

struct Version {
  bool ok = false;
  int part[4] = {};  // major, minor, patch, commits after the tag
};

bool number(const char *&p, int &out) {
  if (!isdigit(static_cast<unsigned char>(*p))) return false;
  long n = 0;
  while (isdigit(static_cast<unsigned char>(*p))) {
    if (n < 1000000) n = n * 10 + (*p - '0');
    ++p;
  }
  out = static_cast<int>(n);
  return true;
}

// "0.2.0", "0.2.0+5", "0.3.0-rc1" (a pre-release suffix is ignored).
Version parseVersion(const std::string &s) {
  Version v;
  const char *p = s.c_str();
  for (int i = 0; i < 3; ++i) {
    if (!number(p, v.part[i])) return v;
    if (i < 2) {
      if (*p != '.') return v;
      ++p;
    }
  }
  v.ok = true;
  const char *plus = strchr(p, '+');
  if (plus) {
    ++plus;
    number(plus, v.part[3]);
  }
  return v;
}

// --- The manifest ------------------------------------------------------------------------

bool isSha256(const std::string &s) {
  if (s.size() != 64) return false;
  for (char c : s) {
    if (!isxdigit(static_cast<unsigned char>(c))) return false;
  }
  return true;
}

bool fetchManifest(Release &out) {
  const std::string url = manifestUrl();
  log_i("ota: checking %s", url.c_str());
  net::Get get;
  get.setUserAgent(kUserAgent);
  std::string error;
  if (!get.open(url, error)) {
    // GitHub answers 404 until a release has a manifest.json.
    fail(get.status() == 404 ? "NO RELEASE FOUND" : error);
    return false;
  }
  std::string body;
  char buf[512];
  for (;;) {
    const int n = get.read(buf, sizeof buf);
    if (n < 0) {
      fail("CONNECTION LOST");
      return false;
    }
    if (n == 0) break;
    body.append(buf, n);
    if (body.size() > kMaxManifest) {
      fail("MANIFEST TOO BIG");
      return false;
    }
  }
  get.close();

  cJSON *json = cJSON_Parse(body.c_str());
  if (!json) {
    fail("MANIFEST ISN'T JSON");
    return false;
  }
  out.version = jsonf::str(json, "version");
  out.build = jsonf::str(json, "build");
  out.date = jsonf::str(json, "date");
  out.url = jsonf::str(json, "url");
  out.sha256 = jsonf::str(json, "sha256");
  const int size = jsonf::integer(json, "size", 0);
  cJSON_Delete(json);
  out.size = size > 0 ? static_cast<uint32_t>(size) : 0;
  for (char &c : out.sha256) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));

  const bool https = out.url.rfind("https://", 0) == 0;
  // Plain http only for a manifest that came that way (a dev server, setManifestUrl()).
  const bool http = out.url.rfind("http://", 0) == 0 && url.rfind("http://", 0) == 0;
  if (!parseVersion(out.version).ok || !(https || http) || !isSha256(out.sha256) || !out.size) {
    fail("MANIFEST INCOMPLETE");
    return false;
  }
  log_i("ota: latest release %s (%s, %u bytes)", out.version.c_str(), out.build.c_str(),
        static_cast<unsigned>(out.size));
  return true;
}

// --- The download ------------------------------------------------------------------------

std::string hex(const uint8_t *d, size_t n) {
  static const char kDigits[] = "0123456789abcdef";
  std::string out;
  for (size_t i = 0; i < n; ++i) {
    out += kDigits[d[i] >> 4];
    out += kDigits[d[i] & 15];
  }
  return out;
}

// Streams the image into the idle slot. Done if it's in and the boot slot switched; else
// fail() (or stopRequested), with the running firmware untouched.
void download(const Release &r) {
  const esp_partition_t *slot = esp_ota_get_next_update_partition(nullptr);
  if (!slot) return fail("NO IDLE SLOT");
  if (r.size > slot->size) return fail("TOO BIG FOR THE SLOT");

  net::Get get;
  get.setUserAgent(kUserAgent);
  std::string error;
  if (!get.open(r.url, error)) return fail(error);
  if (get.status() != 200) return fail("SERVER SAID " + std::to_string(get.status()));
  if (get.length() >= 0 && get.length() != static_cast<int64_t>(r.size)) {
    return fail("SIZE DOESN'T MATCH THE MANIFEST");
  }

  // Flash writes stall the cache, so playback would stutter; the update restarts the board
  // anyway.
  audio::stop();
  esp_ota_handle_t handle = 0;
  esp_err_t err = esp_ota_begin(slot, OTA_WITH_SEQUENTIAL_WRITES, &handle);
  if (err != ESP_OK) return fail("COULDN'T START: " + upper(esp_err_to_name(err)));
  log_i("ota: downloading %u bytes into %s", static_cast<unsigned>(r.size), slot->label);

  // Internal RAM: flash writes run with the cache off.
  auto *buf = static_cast<char *>(heap_caps_malloc(kChunk, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (!buf) {
    esp_ota_abort(handle);
    return fail("NO MEMORY FOR THE DOWNLOAD");
  }
  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, 0);
  uint32_t got = 0;
  std::string why;
  while (!stopRequested) {
    const int n = get.read(buf, kChunk);
    if (n < 0) {
      why = "CONNECTION LOST";
      break;
    }
    if (n == 0) break;
    if (got + n > r.size) {
      why = "BIGGER THAN THE MANIFEST SAYS";
      break;
    }
    mbedtls_sha256_update(&sha, reinterpret_cast<const unsigned char *>(buf), n);
    err = esp_ota_write(handle, buf, n);
    if (err != ESP_OK) {
      // The first write checks for an app image's magic byte.
      why = err == ESP_ERR_OTA_VALIDATE_FAILED ? "NOT A FIRMWARE IMAGE"
                                               : "FLASH WRITE: " + upper(esp_err_to_name(err));
      break;
    }
    got += n;
    bytes = got;
  }
  uint8_t digest[32];
  mbedtls_sha256_finish(&sha, digest);
  mbedtls_sha256_free(&sha);
  heap_caps_free(buf);
  get.close();

  if (why.empty() && stopRequested) why = "CANCELLED";
  if (why.empty() && got != r.size) why = "DOWNLOAD CUT SHORT";
  if (why.empty() && hex(digest, sizeof digest) != r.sha256) why = "CHECKSUM DOESN'T MATCH";
  if (!why.empty()) {
    esp_ota_abort(handle);
    return fail(why);
  }
  err = esp_ota_end(handle);  // checks the image (header, segments, its own SHA-256)
  if (err != ESP_OK) {
    return fail(err == ESP_ERR_OTA_VALIDATE_FAILED ? "BAD IMAGE" : upper(esp_err_to_name(err)));
  }
  err = esp_ota_set_boot_partition(slot);
  if (err != ESP_OK) return fail("COULDN'T SWITCH SLOTS: " + upper(esp_err_to_name(err)));
  log_i("ota: %s written and verified; boots next", slot->label);
  phase = Phase::Done;
}

void restart() {
  const uint32_t t0 = millis();
  while (!shown && millis() - t0 < kShownWaitMs) delay(50);
  audio::stop();  // amp off and DAC muted first, so the speaker doesn't pop
  delay(150);
  log_i("ota: restarting into the new image");
  esp_restart();
}

void run(void *) {
  std::string error;
  if (!wlan::connect(kConnectTimeoutMs, error)) {
    fail("WI-FI: " + error);
  } else if (!stopRequested) {
    if (job == Job::Check) {
      phase = Phase::Checking;
      Release found;
      if (fetchManifest(found)) {
        {
          std::lock_guard<std::mutex> lock(textMutex);
          latest = found;
          latestOrder = compareVersions(found.version, sysinfo::firmwareVersion());
        }
        phase = Phase::Checked;
      }
    } else {
      Release r;
      {
        std::lock_guard<std::mutex> lock(textMutex);
        r = latest;
      }
      phase = Phase::Downloading;
      WiFi.setSleep(false);  // modem sleep slows the download several times over
      download(r);
      WiFi.setSleep(true);
    }
  }
  wlan::disconnect();
  if (phase == Phase::Done) restart();  // even if stopped meanwhile: the new image is in

  // A cancel isn't a failure: back to what was known before.
  if (stopRequested) phase = job == Job::Install ? Phase::Checked : Phase::Idle;
  log_i("ota: %s finished", job == Job::Check ? "check" : "download");
  running = false;
  vTaskDelete(nullptr);
}

bool startTask(Job j) {
  shown = false;
  bytes = 0;
  job = j;
  {
    std::lock_guard<std::mutex> lock(textMutex);
    resultText.clear();
  }
  phase = Phase::Joining;
  // HTTPS: the TLS handshake runs on this task.
  if (xTaskCreate(run, "ota", 12288, nullptr, 1, nullptr) != pdPASS) {
    fail("NO MEMORY FOR THE UPDATE TASK");
    running = false;
    return false;
  }
  return true;
}

}  // namespace

bool configured() { return wlan::configured(); }

bool check() {
  if (!configured()) return false;
  stopRequested = false;  // withdraws a stop the task hasn't acted on yet
  if (running.exchange(true)) return true;
  return startTask(Job::Check);
}

bool install() {
  if (!configured()) return false;
  {
    std::lock_guard<std::mutex> lock(textMutex);
    if (latest.version.empty()) return false;
  }
  if (running.exchange(true)) return false;
  stopRequested = false;
  return startTask(Job::Install);
}

void stop() {
  if (running) stopRequested = true;
}

Status status() {
  Status s;
  s.phase = phase.load();
  s.bytes = bytes.load();
  std::lock_guard<std::mutex> lock(textMutex);
  s.release = latest;
  s.order = latestOrder;
  s.result = resultText;
  return s;
}

bool active() {
  const Phase p = phase.load();
  return p == Phase::Joining || p == Phase::Checking || p == Phase::Downloading ||
         p == Phase::Done;
}

void doneShown() { shown = true; }

void confirmBoot() {
  const esp_partition_t *run = esp_ota_get_running_partition();
  esp_ota_img_states_t state;
  if (!run || esp_ota_get_state_partition(run, &state) != ESP_OK) return;
  if (state != ESP_OTA_IMG_PENDING_VERIFY) return;
  if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
    log_i("ota: %s started cleanly after an update; keeping it", run->label);
  } else {
    log_e("ota: couldn't mark %s good; the next reset rolls back", run->label);
  }
}

std::string manifestUrl() {
  {
    std::lock_guard<std::mutex> lock(textMutex);
    if (!devManifestUrl.empty()) return devManifestUrl;
  }
  return std::string("https://github.com/") + kRepo + "/releases/latest/download/manifest.json";
}

void setManifestUrl(const std::string &url) {
  std::lock_guard<std::mutex> lock(textMutex);
  devManifestUrl = url;
}

int compareVersions(const std::string &a, const std::string &b) {
  const Version va = parseVersion(a);
  const Version vb = parseVersion(b);
  if (!va.ok || !vb.ok) return 0;
  for (int i = 0; i < 4; ++i) {
    if (va.part[i] != vb.part[i]) return va.part[i] > vb.part[i] ? 1 : -1;
  }
  return 0;
}

}  // namespace ota
