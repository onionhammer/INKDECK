// IMA ADPCM (the DVI/IMA step tables), for comms voice (comms.h, docs/comms-protocol.md): 16-bit
// PCM in, 4 bits a sample out, so 16 kHz mono is 64 kbps. Little code and almost no CPU, which
// leaves the radio's time for relaying.
//
// Each comms packet carries the state it starts from (predictor and step index), so it decodes
// on its own: a lost packet is a 20 ms gap, not noise in everything after it. Two samples a
// byte, the first in the low nibble, as in IMA ADPCM WAV files.
//
// Plain C++ with no Arduino or ESP-IDF calls: the self-test runs it on the board, and it builds
// on a computer as well.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace adpcm {

struct State {
  int16_t predictor = 0;  // the last sample, as the decoder will have it
  uint8_t index = 0;      // into the step table, 0 to 88
};

// Encodes `samples` samples (an even number) into samples / 2 bytes at `out`, and moves the
// state on to where the next block starts.
void encode(State &state, const int16_t *pcm, size_t samples, uint8_t *out);

// Decodes `bytes` bytes into bytes * 2 samples at `pcm`, from `state`, which moves on. A state
// out of range (a corrupt packet) is clamped, never read past the table.
void decode(State &state, const uint8_t *in, size_t bytes, int16_t *pcm);

}  // namespace adpcm
