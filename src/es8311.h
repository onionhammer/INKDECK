// Minimal ES8311 codec control for playback and the microphone, over the shared Wire bus
// (begin Wire first).
//
// Assumes the ESP32 is I2S master and drives MCLK at 256 x fs. For every rate from 8 to 48 kHz
// that gives the same clock-divider settings, so begin() needs no sample rate and a rate change
// only needs the I2S side reclocked.
#pragma once

namespace es8311 {

// Resets and configures the codec for 16-bit I2S playback, DAC muted. Start MCLK first, with
// ALDO2 (its AVDD) on.
bool begin();

// Power-down mode (datasheet: 0 uA): every block back at its power-on default, clocks off.
// begin() wakes it. Its analog supply, ALDO2, can go off after this.
void powerDown();

// 0..100, mapped linearly in dB to -50..0 dB (0.5 dB steps). 0 = silence. Never adds gain.
void setVolume(int volume);

void mute(bool muted);

// Microphone: input Mic1p-Mic1n through the analog PGA at 30 dB and the ADC at 0 dB, then a
// digital gain of 0-42 dB in 6 dB steps (REG16; values round down to a step). Call after
// begin(), with MCLK running.
constexpr int kMicPgaDb = 30;
constexpr int kMicDigitalMaxDb = 42;
void micBegin(int digitalGainDb);
void setMicGain(int digitalGainDb);

}  // namespace es8311
