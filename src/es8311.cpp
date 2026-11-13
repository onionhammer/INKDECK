// Register values follow Espressif's es8311 driver (Apache-2.0; Waveshare's Arduino copy is
// summarised in .claude/skills/waveshare-epaper-reference/references/audio.md §8). The clock
// rows are the 256 x fs entries of its coefficient table, which are identical for 8-48 kHz.
#include "es8311.h"

#include <Arduino.h>
#include <Wire.h>

#include "board_pins.h"

namespace es8311 {
namespace {

bool write(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(board::I2C_ADDR_ES8311);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

struct RegValue {
  uint8_t reg;
  uint8_t value;
};

constexpr RegValue kInit[] = {
    {0x00, 0x00}, {0x00, 0x80},  // release reset; power on state machine, slave mode
    {0x01, 0x3F},                // MCLK from MCLK pin, all internal clocks on
    {0x02, 0x00},                // pre-divider 1, pre-multiplier 1x
    {0x03, 0x10},                // single speed, ADC OSR
    {0x04, 0x10},                // DAC OSR
    {0x05, 0x00},                // ADC/DAC clock dividers 1
    {0x06, 0x03},                // BCLK not inverted (divider only matters in master mode)
    {0x07, 0x00}, {0x08, 0xFF},  // LRCK divider (master mode only)
    {0x09, 0x0C},                // serial in (DAC): I2S, 16-bit, left slot -> DAC
    {0x0A, 0x0C},                // serial out (ADC): I2S, 16-bit
    {0x0D, 0x01},                // power up analog circuitry
    {0x0E, 0x02},                // enable analog PGA and ADC modulator
    {0x12, 0x00},                // power up DAC
    {0x13, 0x10},                // enable output to the headphone/amp driver
    {0x1C, 0x6A},                // ADC EQ bypass, cancel DC offset
    {0x37, 0x08},                // bypass DAC EQ
    {0x32, 0x00},                // DAC volume minimum
    {0x31, 0x60},                // DAC muted
};

}  // namespace

bool begin() {
  // esp_codec_dev writes this twice: the first transaction after power-up occasionally fails.
  write(0x44, 0x08);
  write(0x44, 0x08);
  if (!write(0x00, 0x1F)) {  // reset
    log_e("ES8311: no ACK at 0x%02X", board::I2C_ADDR_ES8311);
    return false;
  }
  delay(20);
  bool ok = true;
  for (const auto &rv : kInit) ok &= write(rv.reg, rv.value);
  if (!ok) log_e("ES8311: init write failed");
  return ok;
}

void setVolume(int volume) {
  volume = constrain(volume, 0, 100);
  // REG32: 0xBF = 0 dB, 0.5 dB per step.
  write(0x32, volume == 0 ? 0x00 : static_cast<uint8_t>(0xBF - (100 - volume)));
}

void mute(bool muted) { write(0x31, muted ? 0x60 : 0x00); }

void setMicGain(int digitalGainDb) {
  // REG16 bits 2:0: ADC scale-up 0-42 dB in 6 dB steps (datasheet; audio.md §4).
  write(0x16, static_cast<uint8_t>(constrain(digitalGainDb, 0, kMicDigitalMaxDb) / 6));
}

void micBegin(int digitalGainDb) {
  // REG14: bit 4 selects Mic1p-Mic1n, bits 3:0 the PGA in 3 dB steps (0xA = 30 dB). REG17: ADC
  // volume, 0xBF = 0 dB. The same values esp_codec_dev and Waveshare's driver use (audio.md).
  write(0x14, 0x10 | (kMicPgaDb / 3));
  write(0x17, 0xBF);
  setMicGain(digitalGainDb);
}

}  // namespace es8311
