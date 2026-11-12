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
  std::string title;   // ID3 title (UTF-8), else the file name
  std::string artist;  // ID3 artist (UTF-8), may be empty
  int track = 0;       // index into the playlist
  int trackCount = 0;
  uint32_t elapsedMs = 0;
  uint32_t durationMs = 0;  // estimate from the first frame's bitrate; 0 = unknown
  uint32_t sampleRate = 0;  // from the first decoded frame; 0 until then
  int bitrateKbps = 0;
  int channels = 0;
  int volume = 0;
  uint32_t generation = 0;  // changes on every track, state or format change (not time/volume)
};

void begin();

// Plays playlist[index], then the following entries, then stops.
void play(std::vector<std::string> playlist, int index);
// Pause/resume; when stopped at the end of the queue, plays the last track again.
void togglePause();
void next();
void stop();
void changeVolume(int delta);

Status status();
bool active();  // playing or paused (the amp and I2S are on)

// Paths of playlist entries [from, from + count), clipped to the playlist.
std::vector<std::string> queue(int from, int count);

}  // namespace audio
