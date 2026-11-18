#include "file_copy.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <mutex>

#include "sdcard.h"

namespace filecopy {
namespace {

// A share's largest read (SMB's) goes straight into a buffer this big, and the card takes it in
// whole sectors. PSRAM, 64-byte aligned for the SDMMC driver's DMA (as webdav.cpp's).
constexpr size_t kChunk = 32 * 1024;
constexpr int64_t kSpareBytes = 8ll << 20;  // left free on the card after a copy

std::mutex mu;
Status current;  // mu; done is kept in doneBytes while the copy runs
std::atomic<bool> running{false};
std::atomic<bool> stopNow{false};
std::atomic<int64_t> doneBytes{0};
std::atomic<uint32_t> startedMs{0};

// Makes the folders above path that are missing, below the card's mount point.
void makeParents(const std::string &path) {
  const size_t root = strlen(sdcard::kMountPoint);
  for (size_t slash = path.find('/', root + 1); slash != std::string::npos;
       slash = path.find('/', slash + 1)) {
    mkdir(path.substr(0, slash).c_str(), 0777);  // EEXIST for those already there
  }
}

void finish(State state, const std::string &error) {
  std::lock_guard<std::mutex> lock(mu);
  current.state = state;
  current.error = error;
  current.done = doneBytes.load();
  current.elapsedMs = millis() - startedMs.load();
  running = false;
}

void run(void *) {
  std::string src, dst;
  int64_t total;
  {
    std::lock_guard<std::mutex> lock(mu);
    src = current.src;
    dst = current.dst;
    total = current.total;
  }
  const std::string part = dst + ".part";
  std::string error;
  int64_t got = 0;
  uint32_t readMs = 0, writeMs = 0;  // for the log: which side is slow
  bool complete = false;

  const int64_t free = static_cast<int64_t>(sdcard::freeBytes());
  uint8_t *buf = nullptr;
  int in = -1;
  int out = -1;
  if (free > 0 && total > 0 && free < total + kSpareBytes) {
    error = "CARD FULL";
  } else if (!(buf = static_cast<uint8_t *>(
                   heap_caps_aligned_alloc(64, kChunk, MALLOC_CAP_SPIRAM)))) {
    error = "OUT OF MEMORY";
  } else if ((in = open(src.c_str(), O_RDONLY)) < 0) {
    log_w("filecopy: can't open %s (errno %d)", src.c_str(), errno);
    error = "CAN'T OPEN IT ON THE SERVER";
  } else {
    makeParents(dst);
    out = open(part.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (out < 0) {
      log_w("filecopy: can't create %s (errno %d)", part.c_str(), errno);
      error = "CAN'T WRITE TO THE CARD";
    }
  }
  if (out >= 0) {
    while (!stopNow.load()) {
      const uint32_t r0 = millis();
      const ssize_t n = read(in, buf, kChunk);
      const uint32_t w0 = millis();
      readMs += w0 - r0;
      if (n < 0) {
        error = "CONNECTION LOST";
        break;
      }
      if (n == 0) {
        complete = true;
        break;
      }
      const bool wrote = write(out, buf, n) == n;
      writeMs += millis() - w0;
      if (!wrote) {
        error = "SD WRITE FAILED";
        break;
      }
      got += n;
      doneBytes = got;
    }
    if (close(out) != 0 && complete) {
      complete = false;
      error = "SD WRITE FAILED";
    }
  }
  if (in >= 0) close(in);  // the share's last user may be this file: its idle timer starts
  heap_caps_free(buf);

  if (complete && rename(part.c_str(), dst.c_str()) != 0) {
    log_w("filecopy: can't rename %s (errno %d)", part.c_str(), errno);
    complete = false;
    error = "CAN'T RENAME THE COPY";
  }
  if (!complete && out >= 0) remove(part.c_str());
  const uint32_t ms = millis() - startedMs.load();
  log_i("filecopy: %s -> %s: %lld KB in %lu ms (%lu KB/s; reading %lu ms, writing %lu ms)%s%s",
        src.c_str(), dst.c_str(), static_cast<long long>(got >> 10), static_cast<unsigned long>(ms),
        static_cast<unsigned long>(ms ? (got >> 10) * 1000 / ms : 0),
        static_cast<unsigned long>(readMs), static_cast<unsigned long>(writeMs),
        complete ? "" : ", ", complete ? "" : (stopNow.load() ? "stopped" : error.c_str()));
  finish(complete ? State::Done : stopNow.load() ? State::Stopped : State::Failed, error);
  vTaskDelete(nullptr);
}

}  // namespace

bool start(const std::string &src, const std::string &dst, int64_t size, std::string &error) {
  if (running.exchange(true)) {
    error = "A COPY IS UNDER WAY";
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(mu);
    current = Status{};
    current.state = State::Copying;
    current.src = src;
    current.dst = dst;
    current.total = size;
  }
  doneBytes = 0;
  stopNow = false;
  startedMs = millis();
  // The share's reads run on this task (libsmb2, the sockets), and the card's writes; the audio
  // task reads shares with 8 KB.
  if (xTaskCreate(run, "filecopy", 8192, nullptr, 1, nullptr) != pdPASS) {
    error = "OUT OF MEMORY";
    finish(State::Failed, error);
    return false;
  }
  return true;
}

Status status() {
  std::lock_guard<std::mutex> lock(mu);
  Status s = current;
  if (s.state == State::Copying) {
    s.done = doneBytes.load();
    s.elapsedMs = millis() - startedMs.load();
  }
  return s;
}

bool busy() { return running.load(); }

void cancel() {
  if (running.load()) stopNow = true;
}

}  // namespace filecopy
