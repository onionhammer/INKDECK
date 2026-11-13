#include "trackinfo.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <atomic>
#include <deque>
#include <map>

namespace trackinfo {
namespace {

constexpr size_t kMaxCached = 48;

struct Entry {
  Info info;
  bool probed = false;
  bool scanned = false;
  bool wantWave = false;
  uint32_t order = 0;  // insertion order, to evict the oldest
};

SemaphoreHandle_t lock = nullptr;
SemaphoreHandle_t wake = nullptr;
std::map<std::string, Entry> cache;
std::deque<std::string> queue;
std::atomic<uint32_t> gen{0};
uint32_t counter = 0;

struct Locked {
  Locked() { xSemaphoreTake(lock, portMAX_DELAY); }
  ~Locked() { xSemaphoreGive(lock); }
};

void evict() {
  while (cache.size() > kMaxCached) {
    auto oldest = cache.begin();
    for (auto it = cache.begin(); it != cache.end(); ++it) {
      if (it->second.order < oldest->second.order) oldest = it;
    }
    cache.erase(oldest);
  }
}

void work(const std::string &path) {
  bool probe, scan;
  mp3info::Meta meta;
  {
    Locked l;
    auto it = cache.find(path);
    if (it == cache.end()) return;
    probe = !it->second.probed;
    scan = it->second.wantWave && !it->second.scanned;
    meta = it->second.info.meta;
  }
  if (probe) {
    const uint32_t t0 = millis();
    const bool ok = mp3info::probe(path, meta);
    log_i("trackinfo: probe %s %s in %lu ms", path.c_str(), ok ? "ok" : "failed",
          (unsigned long)(millis() - t0));
    Locked l;
    auto it = cache.find(path);
    if (it != cache.end()) {
      it->second.info.meta = meta;
      it->second.probed = true;
    }
    gen++;
  }
  if (scan) {
    const uint32_t t0 = millis();
    std::vector<uint8_t> wave;
    mp3info::waveform(path, meta, kWaveBins, wave);
    log_i("trackinfo: waveform of %s in %lu ms", path.c_str(), (unsigned long)(millis() - t0));
    Locked l;
    auto it = cache.find(path);
    if (it != cache.end()) {
      it->second.info.wave = std::move(wave);
      it->second.scanned = true;
    }
    gen++;
  }
}

void task(void *) {
  for (;;) {
    xSemaphoreTake(wake, portMAX_DELAY);
    for (;;) {
      std::string path;
      {
        Locked l;
        if (queue.empty()) break;
        path = queue.front();
        queue.pop_front();
      }
      work(path);
    }
  }
}

}  // namespace

void begin() {
  lock = xSemaphoreCreateMutex();
  wake = xSemaphoreCreateBinary();
  // Core 0 next to the audio task, below its priority: scans only use the time it leaves idle.
  xTaskCreatePinnedToCore(task, "trackinfo", 8192, nullptr, 2, nullptr, 0);
}

void request(const std::string &path, bool wave) {
  if (path.empty()) return;
  {
    Locked l;
    Entry &e = cache[path];
    if (e.order == 0) e.order = ++counter;
    const bool needed = !e.probed || (wave && !e.scanned);
    e.wantWave |= wave;
    if (!needed) return;
    for (const auto &queued : queue) {
      if (queued == path) return;
    }
    queue.push_back(path);
    evict();
  }
  xSemaphoreGive(wake);
}

bool get(const std::string &path, Info &out) {
  Locked l;
  auto it = cache.find(path);
  if (it == cache.end() || !it->second.probed) return false;
  out = it->second.info;
  return true;
}

uint32_t generation() { return gen.load(); }

}  // namespace trackinfo
