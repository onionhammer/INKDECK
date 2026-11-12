// MP3 player: streams from SD, decodes with the core's libhelix, plays through the ES8311 and
// the NS4150B speaker amp. Runs in its own task on core 0, so playback continues while the UI
// (core 1) is busy refreshing the panel or loading a book.
//
// The amp (AMP_EN) and I2S/MCLK are only on during a playback session; pause just mutes the DAC.
#pragma once
#include <stdint.h>

#include <string>
#include <vector>

namespace audio {

enum class State : uint8_t { Stopped, Playing, Paused };

struct Status {
  State state = State::Stopped;
  std::string path;
  std::string title;   // ID3 title, else file name
  std::string artist;  // ID3 artist, may be empty
  int track = 0;       // index into the playlist
  int trackCount = 0;
  uint32_t elapsedMs = 0;
  uint32_t durationMs = 0;  // estimate from the first frame's bitrate; 0 = unknown
  int volume = 0;
  uint32_t generation = 0;  // changes on every track or state change (not on time/volume)
};

void begin();

// Plays playlist[index], then the following entries, then stops.
void play(std::vector<std::string> playlist, int index);
void togglePause();
void next();
void stop();
void changeVolume(int delta);

Status status();
bool active();  // playing or paused

}  // namespace audio
