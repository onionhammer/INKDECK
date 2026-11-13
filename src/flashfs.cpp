#include "flashfs.h"

#include <Arduino.h>
#include <FFat.h>
#include <esp_partition.h>

namespace flashfs {
namespace {
bool isMounted = false;
}  // namespace

static const esp_partition_t *partition() {
  return esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT,
                                  "ffat");
}

bool available() { return partition() != nullptr; }

uint64_t partitionBytes() {
  const esp_partition_t *p = partition();
  return p ? p->size : 0;
}

bool mount() {
  if (isMounted) return true;
  if (!available()) return false;
  const uint32_t t0 = millis();
  isMounted = FFat.begin(true, kMountPoint, 5, "ffat");
  if (isMounted) {
    log_i("flash FAT: mounted in %lu ms, %u KB, %u KB free", millis() - t0,
          static_cast<unsigned>(FFat.totalBytes() >> 10),
          static_cast<unsigned>(FFat.freeBytes() >> 10));
  } else {
    log_e("flash FAT: mount failed");
  }
  return isMounted;
}

bool mounted() { return isMounted; }

uint64_t sizeBytes() { return isMounted ? FFat.totalBytes() : 0; }

uint64_t freeBytes() { return isMounted ? FFat.freeBytes() : 0; }

}  // namespace flashfs
