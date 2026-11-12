#include "sdcard.h"

#include <Arduino.h>
#include <SD_MMC.h>

#include "board_pins.h"

namespace sdcard {
namespace {
bool isMounted = false;
}

bool mount() {
  SD_MMC.end();
  isMounted = false;
  if (!SD_MMC.setPins(board::SD_CLK, board::SD_CMD, board::SD_D0, board::SD_D1, board::SD_D2,
                      board::SD_D3)) {
    log_e("SD: setPins failed");
    return false;
  }
  // 4-bit at 20 MHz. 40 MHz (the vendor IDF app's setting) mounted fine but produced
  // intermittent read CRC errors (0x109) on our board/card; 20 MHz is still ~10x what an
  // MP3 or a chapter load needs.
  if (SD_MMC.begin(kMountPoint, false, false, SDMMC_FREQ_DEFAULT, 5)) {
    log_i("SD: mounted at %d kHz, %llu MB card", SDMMC_FREQ_DEFAULT, SD_MMC.cardSize() >> 20);
    isMounted = true;
    return true;
  }
  log_w("SD: mount failed (no card?)");
  return false;
}

bool mounted() { return isMounted; }

uint64_t sizeBytes() { return isMounted ? SD_MMC.cardSize() : 0; }

}  // namespace sdcard
