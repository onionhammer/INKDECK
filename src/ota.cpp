#include "ota.h"

#include <Arduino.h>
#include <ArduinoOTA.h>
#include <WiFi.h>
#include <esp_ota_ops.h>
#include <esp_system.h>

#include <atomic>
#include <mutex>

#include "audio.h"
#include "inkdeck_secrets.h"  // OTA_PASSWORD_SHA256, generated from .env by scripts/gen_secrets.py
#include "wlan.h"

// The core marks a new image good in initArduino(), before setup(), unless this says the app
// will do it (esp32-hal-misc.c). confirmBoot() does, once setup() has run.
extern "C" bool verifyRollbackLater() { return true; }

namespace ota {
namespace {

constexpr uint32_t kConnectTimeoutMs = 20000;
constexpr uint32_t kListenMs = kListenMinutes * 60 * 1000;
constexpr uint32_t kShownWaitMs = 6000;  // for the DONE frame; a full refresh takes about 3 s

std::atomic<Phase> phase{Phase::Idle};
std::atomic<bool> running{false};  // the task exists
std::atomic<bool> stopRequested{false};
std::atomic<bool> shown{false};
std::atomic<bool> attemptFailed{false};  // the current attempt has reported its error
std::atomic<uint32_t> bytes{0};
std::atomic<uint32_t> total{0};
std::atomic<uint32_t> deadlineMs{0};
std::mutex textMutex;
std::string resultText;
std::string ipText;

void setResult(std::string text) {
  std::lock_guard<std::mutex> lock(textMutex);
  resultText = std::move(text);
}

std::string upper(const char *s) {
  std::string out = s ? s : "";
  for (char &c : out) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
  return out;
}

// What went wrong with an attempt; it goes back to listening afterwards. A failed connect is
// followed by an end error for the same attempt, so the first report of an attempt stands
// (a wrong password always comes first in its attempt).
void onError(ota_error_t e) {
  if (e != OTA_AUTH_ERROR && attemptFailed.exchange(true)) return;
  switch (e) {
    case OTA_AUTH_ERROR: setResult("WRONG PASSWORD"); break;
    case OTA_BEGIN_ERROR: setResult("COULDN'T START: " + upper(Update.errorString())); break;
    // espota has the board connect back to the computer for the image.
    case OTA_CONNECT_ERROR: setResult("COULDN'T CONNECT BACK (FIREWALL?)"); break;
    case OTA_RECEIVE_ERROR: setResult("TRANSFER STALLED"); break;
    case OTA_END_ERROR: setResult("BAD IMAGE: " + upper(Update.errorString())); break;
  }
  log_w("ota: attempt failed (%d): %s", static_cast<int>(e), Update.errorString());
  phase = Phase::Listening;
  deadlineMs = millis() + kListenMs;
}

void configure() {
  ArduinoOTA.setHostname(kHostname)
      .setPort(kPort)
      .setPasswordHash(OTA_PASSWORD_SHA256)
      .setRebootOnSuccess(false)  // after the DONE frame is on the panel
      .setMdnsEnabled(true)
      .onStart([] {
        attemptFailed = false;
        bytes = 0;
        total = 0;
        setResult("");
        phase = Phase::Receiving;
        // Flash writes stall the cache, so playback would stutter; the update restarts the
        // board anyway.
        audio::stop();
        log_i("ota: receiving an image");
      })
      .onProgress([](unsigned int done, unsigned int size) {
        bytes = done;
        total = size;
      })
      .onEnd([] {
        log_i("ota: image written and verified (%u bytes)", static_cast<unsigned>(total.load()));
        phase = Phase::Done;
      })
      .onError(onError);
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
    setResult("WI-FI: " + error);
    phase = Phase::Failed;
    running = false;
    vTaskDelete(nullptr);
    return;
  }
  WiFi.setSleep(false);  // modem sleep slows the transfer several times over
  {
    std::lock_guard<std::mutex> lock(textMutex);
    ipText = WiFi.localIP().toString().c_str();
  }

  configure();
  ArduinoOTA.begin();
  deadlineMs = millis() + kListenMs;
  phase = Phase::Listening;
  log_i("ota: listening at %s.local:%u for %lu min", kHostname, kPort,
        static_cast<unsigned long>(kListenMinutes));

  bool timedOut = false;
  while (phase != Phase::Done && !stopRequested) {
    ArduinoOTA.handle();  // a whole transfer happens inside one call
    if (phase == Phase::Listening && static_cast<int32_t>(millis() - deadlineMs) >= 0) {
      timedOut = true;
      break;
    }
    delay(20);
  }

  ArduinoOTA.end();
  WiFi.setSleep(true);
  wlan::disconnect();
  if (phase == Phase::Done) restart();  // even if stopped meanwhile: the new image is in

  if (timedOut) {
    setResult("NOTHING ARRIVED IN " + std::to_string(kListenMinutes) + " MIN");
    phase = Phase::Failed;
  } else {
    phase = Phase::Idle;
  }
  log_i("ota: stopped listening%s", timedOut ? " (timed out)" : "");
  running = false;
  vTaskDelete(nullptr);
}

}  // namespace

bool configured() { return wlan::configured() && OTA_CONFIGURED; }

bool start() {
  if (!configured()) return false;
  stopRequested = false;  // withdraws a stop the task hasn't acted on yet
  if (running.exchange(true)) return true;
  shown = false;
  bytes = 0;
  total = 0;
  setResult("");
  phase = Phase::Joining;
  // Wi-Fi, mDNS and the PBKDF2 password check need more than the default 2 KB.
  if (xTaskCreate(run, "ota", 8192, nullptr, 1, nullptr) != pdPASS) {
    setResult("NO MEMORY FOR THE UPDATE TASK");
    phase = Phase::Failed;
    running = false;
    return false;
  }
  return true;
}

void stop() {
  if (running) stopRequested = true;
}

Status status() {
  Status s;
  s.phase = phase.load();
  s.bytes = bytes.load();
  s.total = total.load();
  if (s.phase == Phase::Listening) {
    const int32_t left = static_cast<int32_t>(deadlineMs.load() - millis());
    s.secondsLeft = left > 0 ? (static_cast<uint32_t>(left) + 999) / 1000 : 0;
  }
  std::lock_guard<std::mutex> lock(textMutex);
  s.ip = ipText;
  s.result = resultText;
  return s;
}

bool active() {
  const Phase p = phase.load();
  return p == Phase::Joining || p == Phase::Listening || p == Phase::Receiving;
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

}  // namespace ota
