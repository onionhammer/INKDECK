#include "adpcm.h"

#include "features.h"

#if INKDECK_COMMS  // the whole file (features.h)

namespace adpcm {
namespace {

constexpr int16_t kSteps[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,
    25,    28,    31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,
    88,    97,    107,   118,   130,   143,   157,   173,   190,   209,   230,   253,   279,
    307,   337,   371,   408,   449,   494,   544,   598,   658,   724,   796,   876,   963,
    1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,  2272,  2499,  2749,  3024,  3327,
    3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,  9493,  10442, 11487,
    12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};

constexpr int8_t kIndexMoves[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

int clampIndex(int i) { return i < 0 ? 0 : i > 88 ? 88 : i; }
int clampSample(int v) { return v < -32768 ? -32768 : v > 32767 ? 32767 : v; }

// The step a code adds, exactly as the decoder works it out, so encoder and decoder agree.
int delta(int step, uint8_t code) {
  int d = step >> 3;
  if (code & 4) d += step;
  if (code & 2) d += step >> 1;
  if (code & 1) d += step >> 2;
  return d;
}

uint8_t encodeOne(int &predictor, int &index, int sample) {
  const int step = kSteps[index];
  int diff = sample - predictor;
  uint8_t code = 0;
  if (diff < 0) {
    code = 8;
    diff = -diff;
  }
  if (diff >= step) {
    code |= 4;
    diff -= step;
  }
  if (diff >= step >> 1) {
    code |= 2;
    diff -= step >> 1;
  }
  if (diff >= step >> 2) code |= 1;
  const int d = delta(step, code);
  predictor = clampSample(code & 8 ? predictor - d : predictor + d);
  index = clampIndex(index + kIndexMoves[code]);
  return code;
}

int16_t decodeOne(int &predictor, int &index, uint8_t code) {
  const int d = delta(kSteps[index], code);
  predictor = clampSample(code & 8 ? predictor - d : predictor + d);
  index = clampIndex(index + kIndexMoves[code]);
  return static_cast<int16_t>(predictor);
}

}  // namespace

void encode(State &state, const int16_t *pcm, size_t samples, uint8_t *out) {
  int predictor = state.predictor;
  int index = clampIndex(state.index);
  for (size_t i = 0; i + 1 < samples; i += 2) {
    const uint8_t lo = encodeOne(predictor, index, pcm[i]);
    const uint8_t hi = encodeOne(predictor, index, pcm[i + 1]);
    out[i / 2] = static_cast<uint8_t>(lo | hi << 4);
  }
  state.predictor = static_cast<int16_t>(predictor);
  state.index = static_cast<uint8_t>(index);
}

void decode(State &state, const uint8_t *in, size_t bytes, int16_t *pcm) {
  int predictor = state.predictor;
  int index = clampIndex(state.index);
  for (size_t i = 0; i < bytes; ++i) {
    pcm[2 * i] = decodeOne(predictor, index, in[i] & 0x0F);
    pcm[2 * i + 1] = decodeOne(predictor, index, in[i] >> 4);
  }
  state.predictor = static_cast<int16_t>(predictor);
  state.index = static_cast<uint8_t>(index);
}

}  // namespace adpcm

#endif  // INKDECK_COMMS
