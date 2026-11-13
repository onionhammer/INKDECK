// Track details the Music screen shows beyond what the player knows: tags and length for queued
// tracks, and the waveform overview. A background task works them out (SD reads and a few MP3
// decodes), so the UI never waits: request(), then poll get() when generation() changes.
#pragma once
#include <stdint.h>

#include <string>
#include <vector>

#include "mp3info.h"

namespace trackinfo {

constexpr int kWaveBins = 72;

struct Info {
  mp3info::Meta meta;
  std::vector<uint8_t> wave;  // kWaveBins levels, 0-255; empty until scanned
};

void begin();

// Queues path to be probed, and scanned for the waveform if wave is set. Cheap to call again.
void request(const std::string &path, bool wave);

// False until the probe has finished (or failed: then meta has only the file name).
bool get(const std::string &path, Info &out);

// Changes whenever a probe or scan finishes.
uint32_t generation();

}  // namespace trackinfo
