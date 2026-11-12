# AGENTS.md

Firmware for **one device: the Waveshare ESP32-S3-ePaper-3.97**, an ESP32-S3 board with a 3.97"
800×480 B/W e-paper display, PMIC + Li-ion charging, audio, RTC and sensors. Base every pin, driver
and power decision on this file, `include/board_pins.h`, or the primary sources in
[`docs/`](docs/README.md). If they disagree with the schematic, the schematic wins; fix them.

## Layout & commands

| Path | Purpose |
| --- | --- |
| `platformio.ini` | Single env `epaper397`: official `espressif32 @ ~8.0.0` (Arduino core 3.3.x / IDF 5.5), partition table (2×3 MB OTA + 9.9 MB FAT), log level |
| `boards/waveshare_esp32_s3_epaper_397.json` | Custom board def: 16 MB QIO flash, **octal PSRAM (`qio_opi`)**, native USB CDC |
| `include/board_pins.h` | **All GPIOs and I2C addresses**, from the schematic. Use these constants; don't hard-code pins |
| `src/` | Firmware. `ui/` is the navigator (screen stack and key language, `ui/nav.h`), components, text styles and generated fonts. `apps/` holds the screens. Drivers sit flat in `src/` |
| `PROGRESS.md` | **UI build plan and status**: phases, decisions, known issues, how to add a screen. Update it when a phase moves |
| `tools/` | `gen_fonts.py` (JetBrains Mono → GFX bitmap fonts) and `screenshot.py` (drive the UI over USB, save the framebuffer as PNG). Run both with `uv run` |
| `docs/` | Schematic, datasheets, mechanical files, Waveshare wiki as markdown |
| `docs/ui-design.md` | **UI design spec** (INKDECK/OS): orientation, 1-bit visual rules, shared chrome, button mapping, one entry per screen. Links the [design canvas](https://claude.ai/artifact/CkQZ5d3cCFz6dFwQNNuwof). **Read before writing any screen or UI code** |
| `.claude/skills/waveshare-epaper-reference/` | Distilled Waveshare sample code: driver snippets, init sequences, power lifecycle, porting notes, one `references/*.md` per subsystem. **Read the relevant one before writing hardware code** |

The PlatformIO CLI isn't on PATH. Use `~/.platformio/penv/Scripts/pio.exe`, or the VS Code PlatformIO extension.

```sh
pio run                      # build
pio run -t upload            # flash over native USB
pio device monitor           # serial console (115200, exception decoder on)
```

Vendor sample code: <https://github.com/waveshareteam/ESP32-S3-ePaper-3.97>. The skill above pins
the commit and explains how to fetch full files when a snippet isn't enough.

## Hardware

- **MCU:** ESP32-S3-WROOM-1-N16R8 (240 MHz dual-core, 16 MB flash, 8 MB octal PSRAM, Wi-Fi + BLE 5). USB-C goes directly to the S3's native USB. No UART bridge, no reset button.
- **Display:** SSD1677 controller, 4-wire SPI, write-only. Refresh: full ≈3 s, fast ≈1.5 s, partial ≈0.3 s, 4-grey ≈3 s. Operating range 0–50 °C.
- **PMIC:** TG28, **AXP2101 register-compatible**; Waveshare uses XPowersLib's `XPowersAXP2101`.
- **Peripherals:** ES8311 codec + NS4150B speaker amp + mic, QMI8658A IMU, SHTC3, PCF85063A RTC, microSD (4-bit SDMMC), 3-way rotary switch, BOOT and PWR buttons.
- **Connectors:** MX1.25 for the 3.7 V Li-ion battery, the speaker and an RTC backup cell (marked "reserved"). Test pads on the bottom edge: 5V, GND, 3V3, RX, TX, SDA, SCL, GP0, GP1.

## Power architecture (read before writing sleep/power code)

- **Rails:** DC1 → 3V3 system rail. ALDO1 → `AudioCTR_VCC`. ALDO2 → mic supply. **ALDO3 → the e-paper panel's power switch; if ALDO3 is off, the panel is dead.** These rails are on by default at power-up.
- **On/off:** the PWR key drives the PMIC's `PWRON` pin (Waveshare config: 1 s press = on, 4 s = forced off). `axp2101.shutdown()` cuts power to everything, including the ESP32.
- **Wake from full power-off:** the RTC's INT output is diode-OR'd onto `PWRON`, so **an RTC alarm boots the board.** The intended low-power loop: update display → panel deep sleep → arm RTC alarm → PMIC shutdown. The e-paper holds its image with no power. The RTC runs from the PMIC's always-on LDO. On boot, `RTC_INT` and `PWR_KEY_SENSE` tell you why the board woke.
- ESP32 deep sleep also works, but the rails stay up, so it draws more power. Use it only when you need fast wake or wake on a GPIO.
- **Battery** (Waveshare's settings): 200 mA charge to 4.2 V, 1500 mA VBUS limit, 2.6 V power-down. A 10 k NTC is fitted on TS, but XPowersLib disables TS measurement at init. The vendor also disables the CHGLED output, so GPIO2 (`PMIC_CHG`) shows nothing unless you re-enable it.

## Driver notes

- **E-paper:** the framebuffer is 48,000 bytes at 1 bpp (96,000 for 4-grey); put it in PSRAM. Wait on BUSY rather than using fixed delays. Always deep-sleep the panel after an update. Partial refreshes accumulate ghosting, so do a full refresh periodically. Waveshare's `EPD_Sleep()` also cuts ALDO3, which wipes panel RAM. After that, a partial refresh needs a re-init plus a re-send of the previous frame as the base.
- **Audio:** the vendor lets `esp_codec_dev` own `AMP_EN`. It stays HIGH from codec open until close, and silence is a DAC mute. Switching the amp per clip would save power but may pop, so test before relying on it. The codec is configured over I2C; audio flows over I2S.
- **I2C:** all 5 devices share one bus. Create one bus instance and pass it around; don't let each driver initialise its own.

## Flashing & recovery

- If the USB port disappears (crashed firmware, board powered off): **hold BOOT, power-cycle (replug USB, or PWR key on battery), release BOOT** to enter ROM download mode, then upload.
- On battery, hung firmware keeps the board on. Hold PWR for 4 s or more to force it off.
- Stock firmware: 16 MB full-flash images in the upstream repo under `Firmware/Factory procedure/*.bin`. Write them at `0x0` with esptool.
- **Opening the serial port can reset the board.** Verified on hardware: a pyserial open with default settings reset it, while setting `dtr = rts = False` before `open()` did not (`tools/screenshot.py` does that). The likely cause is the USB-Serial-JTAG auto-reset (RTS high with DTR low, the same signal esptool uses to reset).

## Gotchas

1. The wiki tutorials say they use the "ESP32-S3-Zero". That's boilerplate; their pins are for this board.
2. Vendor code defines the IMU interrupt (INT1) as GPIO39: IDF `DOF_INT1`, Arduino `SENSOR_IRQ`. GPIO39 is really the amp enable, and INT1 isn't connected to it (R9 not fitted). Use `IMU_INT2` (GPIO40).
3. The vendor IMU driver has scaling and axis bugs (gyro reads 2× high, gyro X/Y swapped). Check `references/sensors.md` in the skill before reusing it.
4. An RTC alarm set in the past fires only when that date-time comes round next month. Always compute the next wake time from the current RTC time.
5. If PSRAM reports 0 KB, the board's `memory_type` isn't `qio_opi`.
6. On the S3, Arduino's `Serial1` defaults to GPIO15/16, which are SD D0/CLK. Always pass explicit pins.
