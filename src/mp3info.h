// MP3 file inspection without playing it: ID3v2 tags, format and length (including VBR files
// via their Xing/Info/VBRI header), and a coarse loudness overview for the Music waveform.
#pragma once
#include <stdint.h>
#include <stdio.h>

#include <string>
#include <vector>

namespace mp3info {

struct Meta {
  std::string title;   // UTF-8; the file name (without extension) if there's no tag
  std::string artist;  // UTF-8; may be empty
  long audioStart = 0;  // first byte after the ID3 tag
  long audioBytes = 0;
  uint32_t durationMs = 0;  // 0 = unknown
  uint32_t sampleRate = 0;
  uint16_t bitrateKbps = 0;  // the first frame's, or the average for VBR
  uint8_t channels = 0;
  bool vbr = false;
};

// Reads ID3v2.3/2.4 TIT2/TPE1 (UTF-8 out) and returns the offset of the first audio byte.
long readTags(FILE *f, std::string &title, std::string &artist);

// Tags plus format and length. False if the file can't be read or has no MPEG audio frame.
bool probe(const std::string &path, Meta &out);

// Loudness at `bins` evenly spaced points: decodes a few frames at each one. Levels are 0-255
// on a 60 dB scale. Takes roughly 15 ms per bin (SD seek + three frame decodes).
bool waveform(const std::string &path, const Meta &meta, int bins, std::vector<uint8_t> &out);

}  // namespace mp3info
