// microSD card over 4-bit SDMMC, mounted at /sdcard.
#pragma once
#include <stdint.h>

namespace sdcard {

constexpr const char *kMountPoint = "/sdcard";

bool mount();  // (re)mounts; true on success
bool mounted();
uint64_t sizeBytes();  // 0 when not mounted
// Free space on the file system; 0 when not mounted. Can take a while on a big card the first
// time (FatFs counts free clusters), so call it from a background task.
uint64_t freeBytes();

}  // namespace sdcard
