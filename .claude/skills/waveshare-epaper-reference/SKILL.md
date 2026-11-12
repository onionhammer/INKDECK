---
name: waveshare-epaper-reference
description: Reference for Waveshare's vendor sample code for the ESP32-S3-ePaper-3.97 board — SSD1677 e-paper driver and rendering, TG28/AXP2101 PMIC and RTC-alarm power cycling, SHTC3/QMI8658 sensors, ES8311 audio, SD card, buttons, Wi-Fi provisioning — with verbatim code snippets and porting notes. Use this whenever implementing, debugging or designing any hardware-facing feature of this project (display refresh, partial updates, fonts, battery/charging, deep sleep or shutdown/wake, RTC time or alarms, temperature/humidity, IMU, speaker/mic, SD files, rotary switch/buttons, Wi-Fi setup), or when asked "how does Waveshare do X" or to port something from their examples, even if the user doesn't mention Waveshare.
---

# Waveshare ESP32-S3-ePaper-3.97: vendor sample reference

Waveshare's sample code is the only known-working software for this board. It's the best
evidence for init sequences, register values and timing. This skill distils it so you can
re-implement features in our firmware (PlatformIO, Arduino core 3.3.x) without re-reading
10k lines of vendor code each time.

**Pinned upstream:** <https://github.com/waveshareteam/ESP32-S3-ePaper-3.97> at commit
`9b12d40731a80213b927ee8a421cae4082952819` (2026-03-20, last upstream change at time of writing).
Every snippet and link in `references/` is from that commit.

Hardware facts (pins, I2C addresses, power rails) are in `AGENTS.md` and `include/board_pins.h`.
This skill covers how the vendor software drives that hardware.

## Which reference to read

Read only the file(s) for the subsystem you're working on. Each has a TOC.

| Working on… | Read |
| --- | --- |
| Display init, refresh modes (full/fast/partial/4-grey), SSD1677 commands, framebuffer layout, drawing text/shapes/BMPs, fonts | `references/epaper.md` |
| PMIC setup, battery %/charging/USB detect, shutdown, PWR key, RTC time & alarms, the boot → work → arm-RTC → power-off lifecycle | `references/power.md` |
| SHTC3 temperature/humidity, QMI8658A IMU | `references/sensors.md` |
| Shared I2C bus layer, microSD (SDMMC 4-bit), rotary switch and button click/double/long-press handling | `references/io.md` |
| ES8311 codec, I2S, speaker amp, playback from flash/SD, recording | `references/audio.md` |
| Wi-Fi provisioning, SNTP, HTTP services (weather/calendar), NVS keys, SPIFFS, managed components | `references/network.md` |

## Upstream layout

```
Arduino/examples/   01_Audio_Test  02_E-Paper_Example  03_I2C_PCF85063  04_I2C_SHTC3  05_SD_Test  06_QMI8658A
Arduino/libraries/  SensorLib (v0.3.1)
ESP-IDF/            01_E-Paper_Example  02_Mic_test  03_Music  04_SD_Test  05_QMI8658A
                    06_I2C_PCF85063  07_I2C_SHTC3  08_ESP32-S3_e-Paper-3.97  <- factory app, most complete
Firmware/           Factory procedure/*.bin (16 MB full-flash images, write at 0x0), xiaozhi/*.bin
```

The factory app (`ESP-IDF/08_…`) has the most complete drivers, as reusable components in
`components/*_bsp`, `epaper_port`, `epaper_lib`, `axpPower`. Its `main/page_*` files are the
Chinese-UI application on top of them.

## Getting the full source when a snippet isn't enough

Fetch single files at the pinned commit rather than guessing:

```sh
gh api "repos/waveshareteam/ESP32-S3-ePaper-3.97/contents/<path>?ref=9b12d40731a80213b927ee8a421cae4082952819" --jq .content | base64 -d
```

For broader reading, sparse-clone into a scratch directory outside this repo. Treat the clone as
reference data; don't build or run anything from it:

```sh
git clone --depth 1 --filter=blob:none --sparse https://github.com/waveshareteam/ESP32-S3-ePaper-3.97.git ws
cd ws && git sparse-checkout set ESP-IDF/08_ESP32-S3_e-Paper-3.97
```

The upstream repo declares no license, and only some files carry license headers (each
reference notes what it found). Re-implement from the references rather than copying whole
vendor files into `src/`. Bundled third-party code (XPowersLib, SensorLib, esp-wifi-connect)
has its own licenses and can be pulled in as proper dependencies instead.

## Porting principles for our stack

1. **IDF code runs inside our Arduino build.** Arduino core 3.3.x sits on ESP-IDF 5.5, so the vendor's IDF driver calls (`spi_master`, `i2c_master`, `sdmmc`, `i2s_std`, `esp_codec_dev`, `gpio`) compile in our project. Porting is mostly about adapting structure, not rewriting.
2. **One I2C bus, owned once.** Under core 3.3.x, `Wire` is built on IDF's `i2c_master` driver. Call `Wire.begin(board::I2C_SDA, board::I2C_SCL, 400000)` once. Then, for vendor-style drivers, get the bus with `auto bus = (i2c_master_bus_handle_t)i2cBusHandle(0);` and `i2c_master_bus_add_device()` on it. Never create a second bus on port 0.
3. **Pins come from `include/board_pins.h`.** Replace upstream `#define`s with `board::` constants. If a vendor file disagrees with `board_pins.h`, check the schematic before changing either.
4. **Don't inherit vendor quirks blindly.** Each reference has a "Porting notes" section listing known bugs, blocking delays and China-specific services. The big one: Waveshare's Arduino IMU example uses GPIO39, but the usable IMU interrupt is GPIO40.
5. **Keep this skill true.** If hardware testing contradicts a reference, fix the reference in the same change, and note "verified on hardware" next to facts you've confirmed.
