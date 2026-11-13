// Minimal ES8311 codec control for playback, over the shared Wire bus (begin Wire first).
//
// Assumes the ESP32 is I2S master and drives MCLK at 256 x fs. For every rate from 8 to 48 kHz
// that gives the same clock-divider settings, so begin() needs no sample rate and a rate change
// only needs the I2S side reclocked.
#pragma once

namespace es8311 {

// Resets and configures the codec for 16-bit I2S playback, DAC muted. Start MCLK first.
bool begin();

// 0..100, mapped linearly in dB to -50..0 dB (0.5 dB steps). 0 = silence. Never adds gain.
void setVolume(int volume);

void mute(bool muted);

}  // namespace es8311
