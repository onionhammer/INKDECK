// Waveshare ESP32-S3-ePaper-3.97 — pin and bus map.
// Source of truth: docs/board/ESP32-S3-ePaper-3.97-schematic.pdf, page 1 GPIO table.
// Cross-checked against Waveshare's example code. Strapping pins in use: 0, 3, 45, 46.
// GPIO35-37 are used by the octal PSRAM — never touch them.
#pragma once
#include <stdint.h>

namespace board {

// --- E-paper (SSD1677, 800x480, 4-wire SPI, write-only; Waveshare uses SPI3 @ 20 MHz mode 0)
constexpr int EPD_SCK  = 11;
constexpr int EPD_MOSI = 12;
constexpr int EPD_CS   = 10;
constexpr int EPD_DC   = 9;
constexpr int EPD_RST  = 46;  // strapping pin
constexpr int EPD_BUSY = 3;   // input, strapping pin
constexpr int EPD_WIDTH  = 800;
constexpr int EPD_HEIGHT = 480;

// --- Shared I2C bus (Waveshare: port 0 @ 400 kHz)
constexpr int I2C_SDA = 41;
constexpr int I2C_SCL = 42;
constexpr uint8_t I2C_ADDR_ES8311   = 0x18;  // audio codec
constexpr uint8_t I2C_ADDR_PMIC     = 0x34;  // TG28, AXP2101 register-compatible
constexpr uint8_t I2C_ADDR_PCF85063 = 0x51;  // RTC
constexpr uint8_t I2C_ADDR_QMI8658  = 0x6A;  // IMU (SA0 = GND)
constexpr uint8_t I2C_ADDR_SHTC3    = 0x70;  // temp/humidity

// --- Audio: ES8311 codec + NS4150B amp
constexpr int I2S_MCLK = 13;
constexpr int I2S_BCLK = 14;
constexpr int I2S_WS   = 47;
constexpr int I2S_DOUT = 48;  // ESP -> codec (speaker)
constexpr int I2S_DIN  = 21;  // codec -> ESP (microphone)
constexpr int AMP_EN   = 39;  // HIGH = speaker amp on

// --- microSD, 4-bit SDMMC
constexpr int SD_CLK = 16;
constexpr int SD_CMD = 17;
constexpr int SD_D0  = 15;
constexpr int SD_D1  = 7;
constexpr int SD_D2  = 8;
constexpr int SD_D3  = 18;

// --- Buttons (active low; rotary switch has external 10k pull-ups)
constexpr int BTN_BOOT  = 0;  // strapping pin: held at power-on -> download mode
constexpr int BTN_UP    = 4;
constexpr int BTN_PRESS = 5;  // rotary push ("Function")
constexpr int BTN_DOWN  = 6;

// --- Interrupts / power sense (enable internal pull-ups on the inputs)
constexpr int PWR_KEY_SENSE = 1;   // HIGH while PMIC PWRON is pulled low (PWR key held or RTC INT asserted)
constexpr int PMIC_CHG      = 2;   // TG28 CHGLED output (Hi-Z unless CHGLED mode is enabled on the PMIC)
constexpr int PMIC_IRQ      = 38;  // active low
constexpr int IMU_INT2      = 40;  // use this; INT1 -> GPIO39 only if R9 fitted (NC by default)
constexpr int RTC_INT       = 45;  // LOW when PCF85063 INT asserted; strapping pin

// --- UART0 (RX/TX test pads; verify direction, schematic labels are ambiguous)
constexpr int UART0_TX = 43;
constexpr int UART0_RX = 44;

}  // namespace board
