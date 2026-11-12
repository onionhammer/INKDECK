// microSD card over 4-bit SDMMC, mounted at /sdcard.
#pragma once
#include <stdint.h>

namespace sdcard {

constexpr const char *kMountPoint = "/sdcard";

bool mount();  // (re)mounts; true on success
bool mounted();
uint64_t sizeBytes();  // 0 when not mounted

}  // namespace sdcard
