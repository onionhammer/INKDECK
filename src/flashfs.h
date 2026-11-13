// The onboard FAT partition ("ffat", 9.9 MB of the 16 MB flash; platformio.ini's partition
// table), mounted at /ffat. Nothing in the firmware writes it: files get there with the dev
// command !cp (src/devtools.h). Mounted on first use rather than at boot, because the very
// first mount formats the partition, which takes a few seconds.
#pragma once
#include <stdint.h>

namespace flashfs {

constexpr const char *kMountPoint = "/ffat";

bool available();  // the partition table has the partition
uint64_t partitionBytes();  // its size in the partition table; 0 without one
bool mount();      // mounts it (formatting it if it has never been); true if mounted
bool mounted();
uint64_t sizeBytes();  // the file system's size; 0 when not mounted
uint64_t freeBytes();  // 0 when not mounted

}  // namespace flashfs
