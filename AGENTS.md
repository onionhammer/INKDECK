# AGENTS.md

Firmware for **one device: the Waveshare ESP32-S3-ePaper-3.97**, an ESP32-S3 board with a 3.97"
800×480 B/W e-paper display, PMIC + Li-ion charging, audio, RTC and sensors. Base every pin, driver
and power decision on this file, `include/board_pins.h`, or the primary sources in
[`docs/`](docs/README.md). If they disagree with the schematic, the schematic wins; fix them.

## Layout & commands

| Path | Purpose |
| --- | --- |
| `platformio.ini` | Single env `epaper397`: official `espressif32 @ ~8.0.0` (Arduino core 3.3.x / IDF 5.5), partition table (2×3 MB OTA + 9.9 MB FAT), `-Os` (gotcha 9), log level, display units (`INKDECK_FAHRENHEIT`, see `src/units.h`), standby forecast interval (`INKDECK_STANDBY_WEATHER_HOURS`, see `src/forecast.h`), where a set PIN is asked (`INKDECK_PIN_ON`: `power_on`, `settings`, `files`; see `src/pin_places.h`) |
| `boards/waveshare_esp32_s3_epaper_397.json` | Custom board def: 16 MB QIO flash, **octal PSRAM (`qio_opi`)**, native USB CDC |
| `include/board_pins.h` | **All GPIOs and I2C addresses**, from the schematic. Use these constants; don't hard-code pins |
| `src/` | Firmware. `ui/` is the navigator (screen stack and key language, `ui/nav.h`), components, text styles and generated fonts. `apps/` holds the screens. Drivers sit flat in `src/` |
| `README.md` | The features in place, module by module, and how to build and flash. Update it when a feature lands |
| `tools/` | `gen_fonts.py` (JetBrains Mono → GFX bitmap fonts) and `screenshot.py` (drive the UI over USB, save the framebuffer as PNG). Run both with `uv run` |
| `lib/libsmb2/` | Vendored [libsmb2](https://github.com/sahlberg/libsmb2) (LGPL-2.1) for SMB shares in Files: upstream's client sources unchanged, built by `library.json`, with `port/config.h` for what upstream's CMake would define. Its `README.md` says how to update it |
| `compat/` | Stand-in headers for library builds: `dsps_fft2r_platform.h` lets PNGdec's ESP32-S3 SIMD file assemble (on the include path via `platformio.ini`) |
| `scripts/gen_secrets.py` | PlatformIO pre-build hook. Reads the gitignored `.env` (template: `.env.example`) and writes `inkdeck_secrets.h` (`WIFI_CONFIGURED`, `WIFI_SSID`, `WIFI_PASSWORD`, `TIMEZONE` as a POSIX TZ string, `LOCATION_CONFIGURED`, `LATITUDE`, `LONGITUDE`, and `OTA_CONFIGURED`, `OTA_PASSWORD_SHA256`: only the hash of `OTA_PASSWORD` goes into the image) into the build directory. For an upload to a network port (IP or `*.local`, which PlatformIO sends with espota) it also adds `--auth=<OTA_PASSWORD>` to the upload flags. Include that header only where it's needed. Wi-Fi is optional, but an invalid value stops the build. The values are stored as plain text in the firmware image. Code reads Wi-Fi, the time zone and the location through `src/settings.h` (`wlan.h` for Wi-Fi), never the macros: `SD:/inkdeck.ini` on the card overrides them (example: `inkdeck.ini.example`, which should match the template `src/settings.cpp` writes). Passwords from the card go to NVS and off the card |
| `scripts/gen_build_info.py` | PlatformIO pre-build hook. Writes `inkdeck_build.h` (`INKDECK_BUILD_ID`: the short git commit, `+` if the tree is dirty) into the build directory, for the System screen (`sysinfo::buildId()`) |
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
- **On/off:** the PWR key drives the PMIC's `PWRON` pin (Waveshare config: 1 s press = on, 4 s = forced off). The chip's own defaults are 128 ms and 6 s (verified on hardware). Our `pmic::configure()` sets 1 s / 4 s at boot; `references/power.md` §2 has the full comparison. `axp2101.shutdown()` cuts power to everything, including the ESP32.
- **Wake from full power-off:** the RTC's INT output is diode-OR'd onto `PWRON`, so **an RTC alarm boots the board.** The intended low-power loop: update display → panel deep sleep → arm RTC alarm → PMIC shutdown. The e-paper holds its image with no power. The RTC runs from the PMIC's always-on LDO. On boot, `RTC_INT` and `PWR_KEY_SENSE` tell you why the board woke. **Our RTC holds UTC** (unlike the vendor's local time), so alarm registers are UTC; `TIMEZONE` in `.env` gives local time (`src/rtc.h`). `src/power.h` implements the loop: Sleep (wakes for alarms) and standby (the Clock face, redrawn by an RTC wake every minute). On USB, PWR doesn't power off: it shows the desk face (the Clock with its weather strip) and the board stays on to keep it current (`apps::powerKey()`). The RTC alarm is armed only on the way to a power-off and disarmed first thing at boot: while it's fired, INT holds PWRON low, and 4 s of that forces a running board off.
- ESP32 deep sleep also works, but the rails stay up, so it draws more power. Use it only when you need fast wake or wake on a GPIO.
- **Battery:** a 1500 mAh Li-ion cell. Our firmware charges at 500 mA to 4.2 V: 50 mA precharge, stop at 25 mA. Waveshare charges at 200 mA. The input limit is 1500 mA and the power-down threshold 2.6 V (chip defaults, same as Waveshare). A 10 k NTC is fitted on TS. The chip's default (`0x50`) keeps TS out of charge control, and XPowersLib also disables TS measurement. At its default, CHGLED drives charge status onto GPIO2 (`PMIC_CHG`). Waveshare turns it off; we leave it on.

## Driver notes

- **E-paper:** the framebuffer is 48,000 bytes at 1 bpp (96,000 for 4-grey); put it in PSRAM. Wait on BUSY rather than using fixed delays. Always deep-sleep the panel after an update. Partial refreshes accumulate ghosting, so do a full refresh periodically. Waveshare's `EPD_Sleep()` also cuts ALDO3, which wipes panel RAM. After that, a partial refresh needs a re-init plus a re-send of the previous frame as the base.
- **Audio:** the vendor lets `esp_codec_dev` own `AMP_EN`. It stays HIGH from codec open until close, and silence is a DAC mute. Switching the amp per clip would save power but may pop, so test before relying on it. The codec is configured over I2C; audio flows over I2S.
- **I2C:** all 5 devices share one bus. Create one bus instance and pass it around; don't let each driver initialise its own.
- **Memory:** C++ `new` (so `std::string`, `std::vector`, `std::map`) prefers PSRAM (`src/psram_new.cpp`), because IDF's `malloc()` keeps everything under 4 KB in the ~230 KB of internal RAM, and Wi-Fi needs internal RAM to start. Buffers for DMA, task stacks and anything an IRAM interrupt touches must come from `heap_caps_malloc(..., MALLOC_CAP_INTERNAL)` (or `MALLOC_CAP_DMA`). TLS buffers go to PSRAM through `net::begin()`. Dev key `h` prints the heap.

## Flashing & recovery

- If the USB port disappears (crashed firmware, board powered off): **hold BOOT, power-cycle (replug USB, or PWR key on battery), release BOOT** to enter ROM download mode, then upload.
- On battery, hung firmware keeps the board on. Hold PWR for 4 s or more to force it off.
- Stock firmware: 16 MB full-flash images in the upstream repo under `Firmware/Factory procedure/*.bin`. Write them at `0x0` with esptool.
- **Over Wi-Fi** (`src/ota.h`): System → UPDATE OVER WI-FI listens for 10 minutes, then `pio run -t upload --upload-port inkdeck.local` (or the IP shown) pushes the build with espota. It needs `OTA_PASSWORD` in `.env`, the same one the board was built with. The image goes to the idle slot, and only a verified image switches `otadata`, so an interrupted transfer is harmless. Rollback is on: the core's `verifyRollbackLater()` is overridden, and `ota::confirmBoot()` at the end of `setup()` keeps a new image. A crash or reset before that point boots the previous slot. Keep `confirmBoot()` after anything a bad build could break. The boot after an update is a full one (software reset, never an RTC wake), so `apps::standbyWake()`'s power-off before it never meets a pending image. A USB upload rewrites `otadata` (`boot_app0.bin`) and boots `app0`. Images must fit the 3 MB slot (`firmware.bin` is about 1.9 MB now; PlatformIO's "Flash used" leaves out the 88 KB `.eh_frame`, so check the `.bin`). The partition table can't change over the air.
- **Opening the serial port can reset the board.** Verified on hardware: a pyserial open with default settings reset it, while setting `dtr = rts = False` before `open()` did not (`tools/screenshot.py` does that). The likely cause is the USB-Serial-JTAG auto-reset (RTS high with DTR low, the same signal esptool uses to reset). **On macOS the open resets it anyway** (verified 2026-11-14, `rst:0x15 (USB_UART_CHIP_RESET)`): keep one session open and wait for the boot log to go quiet before sending keys, as `tools/screenshot.py` now does. There the port is `/dev/cu.usbmodem*` and PlatformIO is `~/.platformio/penv/bin/pio`; pass `--upload-port`, or it may pick the Bluetooth port.

## Gotchas

1. The wiki tutorials say they use the "ESP32-S3-Zero". That's boilerplate; their pins are for this board.
2. Vendor code defines the IMU interrupt (INT1) as GPIO39: IDF `DOF_INT1`, Arduino `SENSOR_IRQ`. GPIO39 is really the amp enable, and INT1 isn't connected to it (R9 not fitted). Use `IMU_INT2` (GPIO40).
3. The vendor IMU driver has scaling and axis bugs (gyro reads 2× high, gyro X/Y swapped). Check `references/sensors.md` in the skill before reusing it.
4. An RTC alarm set in the past fires only when that date-time comes round next month. Always compute the next wake time from the current RTC time.
5. If PSRAM reports 0 KB, the board's `memory_type` isn't `qio_opi`.
6. On the S3, Arduino's `Serial1` defaults to GPIO15/16, which are SD D0/CLK. Always pass explicit pins.
7. **Never `fwrite()` a sector or more straight from flash to the SD card.** The SDMMC driver DMAs whole 512-byte sectors directly from the caller's buffer, and from a flash address (string constants, `PROGMEM`) the card gets zeros. Verified with dev command `!sdtest`: 1 KB from flash in one call came back all wrong, while internal RAM and PSRAM were exact. Smaller writes go through stdio's buffer and are fine. Copy into a `std::string` first.
8. A quoted `#include "x.h"` looks in the including file's folder first, so a `src/x.h` is shadowed by `src/apps/x.h` for files in `apps/`. Hence `forecast.h` (not `weather.h`) and `podcast_library.h` (not `podcasts.h`) next to `apps/weather.h` and `apps/podcasts.h`.
9. **Keep `-Os` in `build_flags`.** The framework's PlatformIO build script (`tools/platformio-build.py`) loads Espressif's flags files, which leave the optimization level to the Arduino IDE's `platform.txt`, and adds none itself. Without `-Os`, GCC compiles our code, the Arduino core and every library at `-O0`. Only the prebuilt ESP-IDF libraries were optimized. Measured 2026-11-14: the image went from 2.81 MB to 2.08 MB, and standard-library templates instantiated in our code from 279 KB to 34 KB.
10. **The firmware builds with `-fno-exceptions`** (another 110 KB). `throw`, `try` and `catch` don't compile. A library call that throws (`std::stoi` on bad input, `vector::at` out of range, a failed `new`) ends in `abort()`, as it did before when nothing caught it. Check inputs instead. The other size flags in `platformio.ini` (`ESP32_ARDUINO_NO_RGB_BUILTIN`, PNGdec's `NO_GZIP`/`NOBYFOUR`) are explained there. In code, prefer `siscanf` over `sscanf` when no float is parsed, and float math (`sinf`…) over double where precision allows: the float-capable scanner and the double libm cost about 10 KB each.
