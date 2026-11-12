# IO: shared I2C bus, microSD, buttons

Vendor code at commit `9b12d40`. Paths are relative to `ESP-IDF/08_ESP32-S3_e-Paper-3.97/` unless
they start with `Arduino/`. Device-level I2C protocols (SHTC3, QMI8658A) are in `sensors.md`; the PMIC
and RTC are in `power.md`.

## Contents

1. [Upstream file map](#1-upstream-file-map)
2. [Shared I2C bus layer](#2-shared-i2c-bus-layer)
3. [microSD (SDMMC 4-bit)](#3-microsd-sdmmc-4-bit)
4. [Buttons and rotary switch](#4-buttons-and-rotary-switch)
5. [Porting notes and pitfalls](#5-porting-notes-and-pitfalls)
6. [Licensing](#6-licensing)

## 1. Upstream file map

| What | Link |
| --- | --- |
| I2C addresses, pins, helper prototypes | [i2c_bsp.h#L19-L65](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/components/i2c_bsp/i2c_bsp.h#L19-L65) |
| Bus init + add device | [i2c_bsp.c#L19-L50](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/components/i2c_bsp/i2c_bsp.c#L19-L50) |
| Device registry + handle lookup | [i2c_bsp.c#L53-L93](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/components/i2c_bsp/i2c_bsp.c#L53-L93) |
| Read/write helpers | [i2c_bsp.c#L96-L202](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/components/i2c_bsp/i2c_bsp.c#L96-L202) |
| Boot order (SD, I2C) | [main.cc#L443-L448](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/main/main.cc#L443-L448) |
| Arduino example's own I2C bus (300 kHz) | [Arduino/examples/04_I2C_SHTC3/i2c_bsp.c#L13-L34](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/Arduino/examples/04_I2C_SHTC3/i2c_bsp.c#L13-L34) |
| SD pins + mount | [sdcard_bsp.c#L14-L85](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/components/sdcard_bsp/sdcard_bsp.c#L14-L85) |
| SD directory listing helpers | [sdcard_bsp.c#L94-L147](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/components/sdcard_bsp/sdcard_bsp.c#L94-L147), [#L248-L290](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/components/sdcard_bsp/sdcard_bsp.c#L248-L290) |
| SD file read/write helpers | [sdcard_bsp.c#L150-L245](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/components/sdcard_bsp/sdcard_bsp.c#L150-L245), [#L293-L328](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/components/sdcard_bsp/sdcard_bsp.c#L293-L328) |
| SD header (limits, cache path) | [sdcard_bsp.h#L10-L33](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/components/sdcard_bsp/sdcard_bsp.h#L10-L33) |
| Arduino SD example (SD_MMC, 1-bit) | [Arduino/examples/05_SD_Test/05_SD_Test.ino#L14-L32](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/Arduino/examples/05_SD_Test/05_SD_Test.ino#L14-L32) |
| Button GPIO/ID map, GPIO init | [button_bsp.c#L10-L67](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/components/button_bsp/button_bsp.c#L10-L67) |
| `button_Init` (attach + 5 ms timer) | [button_bsp.c#L70-L123](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/components/button_bsp/button_bsp.c#L70-L123) |
| Event → event-group bit mapping | [button_bsp.c#L126-L330](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/components/button_bsp/button_bsp.c#L126-L330) |
| `wait_key_event_and_return_code` | [button_bsp.c#L341-L379](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/components/button_bsp/button_bsp.c#L341-L379) |
| MultiButton timing constants + events | [multi_button.h#L13-L31](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/components/button_bsp/multi_button.h#L13-L31) |
| MultiButton state machine | [multi_button.c#L61-L158](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/components/button_bsp/multi_button.c#L61-L158) |
| Home menu key handling | [main.cc#L307-L405](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/main/main.cc#L307-L405) |
| Long-press-hold auto-repeat (alarm editor) | [page_alarm.cc#L268-L285](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/main/page_alarm/page_alarm.cc#L268-L285) |

## 2. Shared I2C bus layer

One `i2c_master` bus (IDF ≥ 5.2 "new" driver) on **port 0, SCL GPIO42, SDA GPIO41**, internal pull-ups
on, glitch filter 7. Every device handle is added at **400 kHz**.

```c
#define AXP2101Addr    0x34
#define SHTC3Addr      0x70
#define PCF85063Addr   0x51
#define ES8311_I2C_ADDR 0x18
#define QMI8658_SLAVE_ADDR_L 0x6a      // SA0 is GND
#define QMI8658_SLAVE_ADDR_H 0x6b      // SA0 is VCC
#define ESP32_SCL_NUM    42
#define ESP32_SDA_NUM    41
#define I2C_MASTER_NUM   0
#define I2C_MASTER_FREQ_HZ 400000
// ...
    i2c_master_bus_config_t bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_MASTER_NUM,
        .scl_io_num = ESP32_SCL_NUM,
        .sda_io_num = ESP32_SDA_NUM,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &i2c_bus_handle));
```

**Device registry.** `i2c_devices_init()` adds persistent handles for RTC 0x51, SHTC3 0x70, AXP2101
0x34 and QMI8658 0x6A. Adding a handle doesn't probe the device; failures are only logged, and the
function always returns `ESP_OK`. `get_dev_handle(addr)` maps an address to its handle. **Any other
address (ES8311 0x18, QMI8658 0x6B) gets a temporary handle that's added and removed on every call**:

```c
esp_err_t i2c_write_reg(uint8_t dev_addr, uint8_t reg, uint8_t *data, size_t len)
{
    i2c_master_dev_handle_t dev_handle = get_dev_handle(dev_addr);
    // If there is no pre-created handle, create a temporary one
    if (!dev_handle) {
        esp_err_t ret = i2c_bus_add_device(dev_addr, I2C_MASTER_FREQ_HZ, &dev_handle);
        if (ret != ESP_OK) return ret;
    }
    uint8_t buf[len + 1];
    buf[0] = reg;
    memcpy(&buf[1], data, len);
    esp_err_t ret = i2c_master_transmit(dev_handle, buf, len + 1, pdMS_TO_TICKS(1000));
    // If the handle is created temporarily, it needs to be deleted
    if (get_dev_handle(dev_addr) == NULL) {
        i2c_master_bus_rm_device(dev_handle);
    }
    return ret;
}
```

| Helper | Wire transaction | Used by |
| --- | --- | --- |
| `i2c_write_reg(addr, reg, data, len)` | START addr+W, reg, data…, STOP (one `i2c_master_transmit`) | `axp_prot.cpp` (XPowersLib callback), `pcf85063_bsp.c`, QMI8658 |
| `i2c_read_reg(addr, reg, data, len)` | addr+W, reg, repeated START, addr+R, data… (`i2c_master_transmit_receive`) | same |
| `i2c_write_bytes(addr, data, len)` | raw write, no register byte | SHTC3 commands |
| `i2c_read_bytes(addr, data, len)` | raw read | SHTC3 results |
| `i2c_master_write_read_device_compat(addr, w, wl, r, rl)` | write `wl` bytes, repeated START, read | SHTC3 Read-ID |

The ES8311 codec doesn't use these helpers. `es8311_bsp.c:26` hands the raw `i2c_bus_handle` to
`esp_codec_dev` (`.bus_handle = (void *)i2c_bus_handle`).

**Boot order** (`app_main`): `_sdcard_init()` → `i2c_master_init()` → 50 ms → `i2c_devices_init()`
→ 50 ms → SPIFFS → `PCF85063_init()` → `axp_init()` → … → `button_Init()` → `i2c_shtc3_init()` →
`page_audio_int()` (ES8311) → e-paper → `QMI8658_init()` (interactive mode only). There's no
application-level I2C mutex; the IDF driver serialises transactions on the bus (inferred from IDF
design). The app adds `rtc_mutex` / `qmi8658_mutex` around higher-level calls.

## 3. microSD (SDMMC 4-bit)

| Signal | GPIO | | Signal | GPIO |
| --- | --- | --- | --- | --- |
| CLK | 16 | | D0 | 15 |
| CMD | 17 | | D1 | 7 |
| D3 | 18 | | D2 | 8 |

No card-detect or write-protect pin. Mount point **`/sdcard`**.

```c
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 32 * 1024,  // Set it to 32KB to support larger capacity
    };
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 4;
    slot_config.clk = SDMMC_CLK_PIN;
    // ... cmd, d0..d3 from the #defines above
    esp_err_t ret = esp_vfs_fat_sdmmc_mount(SDlist, &host, &slot_config, &mount_config, &card_host);
```

- `SDMMC_HOST_DEFAULT()` (IDF 5.5) selects **slot 1** with all bus-width flags. `max_freq_khz` is
  raised from `SDMMC_FREQ_DEFAULT` (20 MHz) to **`SDMMC_FREQ_HIGHSPEED` = 40 MHz**.
  `SDMMC_SLOT_CONFIG_DEFAULT()` leaves `.flags = 0`, so no `SDMMC_SLOT_FLAG_INTERNAL_PULLUP` (external
  pull-ups on the board are assumed; inferred, check the schematic).
- `allocation_unit_size` only matters when formatting, and formatting is disabled.
- On failure it logs the decoded `esp_err_t` and returns. `card_host` stays `NULL`. There's no
  retry, remount or hot-plug handling. On success it calls `sdmmc_card_print_info(stdout, card_host)`.
- FATFS config (upstream `sdkconfig`): `CONFIG_FATFS_LFN_HEAP`, `MAX_LFN=255`, UTF-8 API,
  **codepage 936** (GBK). The Arduino core's prebuilt libs use LFN on stack, 255, UTF-8, **codepage
  850**. That only affects non-ASCII 8.3 short names.

**Helpers** (`sdcard_bsp.h`): `MAX_ENTRY_NUM 100`, `MAX_NAME_LEN 255`,
`file_entry_t {char name[255]; bool is_dir;}`.
- `list_dir_once(path, entries, max)`: one pass of `readdir`, skips `.`/`..`, then sorts
  directories first and names with `strcasecmp`.
- `list_dir_page(path, entries, start, n)`: skips `start` raw `readdir` entries and takes `n`, then
  sorts **only that page**. Across pages the order is directory order, not sorted.
- `get_dir_file_count(path)`, and `scan_files(path)`, which recurses and `xQueueSend`s full paths to
  `sdcard_queuehandle`. Nothing creates that queue, so `scan_files` only logs.
- `s_example_read_from_offset(path, buf, len, offset)`: `fopen("rb")` + `fseek` + `fread`. Returns 0
  if `card_host == NULL` or `sdmmc_get_status()` fails.
- `s_example_wriet_from_offset(path, buf, len, mode)` (sic): `mode == 0` truncates the file
  (`fopen("w")`) and returns 0; any other mode appends with `fopen("ab")`. There's no offset parameter.
- `sd_read_file_to_buffer(path, buf, size)`: reads up to `size` bytes, pads the rest with `0xFF`, and
  returns `true` even on a short read. `sd_write_buffer_to_file(path, buf, size)`: `"wb"`, returns
  `w == size`.
- `s_example_write_file` / `s_example_read_file` are non-static but missing from the header.

**What the app stores on SD:** `/sdcard/GUI/*.bmp` (UI icons when `CONFIG_IMG_SOURCE_TFCARD`),
`/sdcard/music` (WAV play/record), `/sdcard/fiction` (+ `/sdcard/bookmarks/*.bookmarks|.progress`), and
`/sdcard/lunar/YYYY-MM.json` (falls back to SPIFFS when `stat("/sdcard")` fails). Also
**`CLOCK_PARTIAL_PATH` = `/sdcard/GUI/clock_partial.bin`** (header comment: "The image cache address
before power failure"): the clock and weather pages write the 48,000-byte (`EPD_SIZE_MONO`) 1-bpp
framebuffer there and read it back. Presumably that's so a timed mode can rebuild the previous screen
after an RTC-alarm power cycle and partial-refresh on top of it (inferred). The settings page shows capacity from `card_host->csd.capacity * 512` and
`f_getfree("0:")`.

## 4. Buttons and rotary switch

| Logical button | GPIO | MultiButton id | Active level | Notes |
| --- | --- | --- | --- | --- |
| `Button_Up` | 4 | 1 | 0 (low) | rotary switch, one direction |
| `Button_Function` | 5 | 2 | 0 | rotary push ("confirm") |
| `Button_Down` | 6 | 3 | 0 | rotary switch, other direction |
| `Boot` | 0 | 4 | 0 | BOOT key (strapping pin) |

`gpio_init()` configures all four as inputs with **internal pull-ups on**, interrupts disabled. The
buttons are purely polled. The PWR key isn't handled here (it's a PMIC input; see `power.md`).

**Timing.** An `esp_timer` fires every **5 ms** (`esp_timer_start_periodic(..., 1000 * 5)`) and
calls `button_ticks()`, which runs the MultiButton state machine for each button:

```c
#define TICKS_INTERVAL    5	//ms
#define DEBOUNCE_TICKS    3	//MAX 7 (0 ~ 7)
#define SHORT_TICKS       (300 /TICKS_INTERVAL)
#define LONG_TICKS        (1000 /TICKS_INTERVAL)
```

| Event | Exact condition (derived from `button_handler`) |
| --- | --- |
| debounce | A level change is accepted after 3 consecutive differing samples (≈ 15 ms) |
| `PRESS_DOWN` | Debounced active level from idle (state 0 → 1, `ticks = 0`, `repeat = 1`) |
| `PRESS_UP` | Debounced release in any pressed state |
| `SINGLE_CLICK` | After a release, no new press for **> 60 ticks (> 300 ms)** and `repeat == 1`. Fires ~300 ms *after release* |
| `DOUBLE_CLICK` | Second press within 300 ms of the first release, released, then > 300 ms idle with `repeat == 2`. A 3rd+ press within the window gives **no click event at all** |
| `LONG_PRESS_START` | Held **> 200 ticks (> 1000 ms)** since `PRESS_DOWN`. No click follows the release |
| `LONG_PRESS_HOLD` | **Every 5 ms tick** while still held after `LONG_PRESS_START` |
| `PRESS_REPEAT` | Callback fires on the 2nd+ press, but see the bug below |

**Event-group bits** (`key_groups`, bit = return code of `wait_key_event_and_return_code`):

| Event | Up (GPIO4) | Function (GPIO5) | Down (GPIO6) | Boot (GPIO0) |
| --- | --- | --- | --- | --- |
| SINGLE_CLICK | 0 | 7 | 14 | 21 |
| DOUBLE_CLICK | 1 | 8 | 15 | 22 |
| PRESS_DOWN | 2 | 9 | 16 | (commented out) |
| PRESS_UP | 3 | 10 | 17 | (commented out) |
| PRESS_REPEAT | 4 (never set) | 11 (never set) | 18 (never set) | (commented out) |
| LONG_PRESS_START | 5 | 12 | 19 | **23** |
| LONG_PRESS_HOLD | 6 | 13 | 20 | **23** (same bit) |

```c
int wait_key_event_and_return_code(TickType_t timeout)
{
    // Wait for all key press event bits
    EventBits_t even = xEventGroupWaitBits(key_groups, set_bit_all, pdTRUE, pdFALSE, timeout);
    if(get_bit_button(even, 0))     return 0; // Button_Up Click
    // ... one test per bit, in ascending order, up to bit 22
    if(get_bit_button(even, 23))     return 23; // Boot Long press to keep triggering
    return -1; // No event was detected
}
```

It waits for *any* of bits 0–23 (`set_bit_all 0x00ffffff`), **clears all of them on exit**, and returns
the **lowest** set bit. It returns −1 on timeout. If several events are pending, only the lowest is
reported and the rest are lost.

**Why `PRESS_REPEAT` never arrives:** `button_press_event()` switches on
`get_button_event(btn)` (`handle->event`), but in state 2 MultiButton sets `handle->event = PRESS_DOWN`
and then calls both the `PRESS_DOWN` and `PRESS_REPEAT` callbacks. The repeat callback therefore sets
the PRESS_DOWN bit again.

**Codes the app acts on** (all pages poll `wait_key_event_and_return_code(pdMS_TO_TICKS(1000))` in a
loop and count −1 timeouts toward panel sleep, then power-off):

| Code | Meaning | Used for |
| --- | --- | --- |
| 0 | Up click | Previous item / decrement |
| 14 | Down click | Next item / increment |
| 7 | Function click | Enter / confirm |
| 8 | Function double-click | Back (most pages) |
| 12 | Function long press | Forced full refresh; wake from panel sleep with full refresh |
| 22 | Boot double-click | Back in pages; **power off** (`axp_pwr_off()`) on the home menu |
| 23 | Boot long press (start/hold) | Home menu → settings page; network page → disable Wi-Fi + restart |
| 21 | Boot click | Audio page only: cancel the delete-file prompt (with 8/22) |
| 6 / 20 | Up / Down long-press hold | Alarm editor: auto-repeat decrement / increment, at the page loop's rate |
| 1 / 15 | Up / Down double-click | Only as "any key wakes" in the fiction page's sleep loop |

Codes 2, 3, 5, 9, 10, 13, 16, 17 and 19 are generated but never used.

## 5. Porting notes and pitfalls

**I2C in our Arduino build.**
- Arduino core 3.3.x on IDF ≥ 5.4 builds `Wire` on the same `i2c_master` driver (`esp32-hal-i2c-ng.c`;
  the legacy `esp32-hal-i2c.c` is compiled out). Own the bus once: `Wire.begin(board::I2C_SDA,
  board::I2C_SCL, 400000)`. Then `auto bus = (i2c_master_bus_handle_t)i2cBusHandle(0);` and
  `i2c_master_bus_add_device(bus, …)` for vendor-style drivers, or pass `bus` to `esp_codec_dev` like
  `es8311_bsp.c` does. `Wire` itself lazily adds one device handle per address at its own frequency
  (`i2cAddDeviceIfNeeded`).
- **Never call `i2c_new_master_bus()` on port 0 as well as `Wire.begin()`.** Whichever runs second
  can't acquire the port. Upstream wraps theirs in `ESP_ERROR_CHECK`, which would abort.
- Don't mix in libraries that use the legacy `driver/i2c.h` API (`i2c_driver_install`,
  `i2c_master_cmd_begin`); IDF 5.x refuses to run both drivers together (inferred from IDF behaviour,
  not tested here).
- Replace temporary per-call handles with persistent ones; adding and removing a device on every
  transaction is wasteful.
- The timeouts are `pdMS_TO_TICKS(1000)` passed as a **millisecond** argument. With upstream's
  `CONFIG_FREERTOS_HZ=100` that's a 100 ms timeout; under Arduino (1000 Hz) it's 1000 ms. Pass plain ms.
- The legacy macros `i2c_writr_buff(addr, reg, buf, len)` and `i2c_read_buff(...)` **silently drop
  `reg`**. Nothing uses them; don't port them.
- The Arduino SHTC3 example creates its own `i2c_master` bus at 300 kHz with
  `i2c_master_bus_wait_all_done` before each transfer. That's fine standalone, but it can't coexist
  with `Wire` on port 0.

**SD card.**
- Arduino: `SD_MMC.setPins(board::SD_CLK, board::SD_CMD, board::SD_D0, board::SD_D1, board::SD_D2,
  board::SD_D3);` then `SD_MMC.begin("/sdcard", /*mode1bit=*/false, /*format=*/false,
  SDMMC_FREQ_HIGHSPEED, 5)`. On ESP32-S3, `SD_MMC` also uses slot 1, and 40 MHz is its default
  (`BOARD_MAX_SDMMC_FREQ`). **Waveshare's Arduino example passes `mode1bit = true`**, so it runs 1-bit
  even though it sets all six pins. Use 4-bit to match the ESP-IDF app. Drop to `SDMMC_FREQ_DEFAULT`
  (20 MHz) if you see CRC or timeouts.
- **Pin conflicts:** Arduino core 3.3's default `Serial1` pins on ESP32-S3 are **RX1 = GPIO15
  (SD_D0), TX1 = GPIO16 (SD_CLK)**. Never call `Serial1.begin(baud)` without explicit pins. GPIO15/16
  are also `XTAL_32K_P/N`, so don't enable an external 32 kHz crystal. The Arduino SD example mounts
  before the e-paper init and the IDF app mounts first in `app_main`; the SD pins don't overlap the
  e-paper SPI pins (9–12, 3, 46).
- Mount failure leaves the FS absent and pages fall back (lunar → SPIFFS) or show "SD卡未初始化". Keep
  that graceful degradation. Upstream never unmounts before `axp_pwr_off()` (inferred risk: FAT
  corruption if a write is in flight, e.g. `clock_partial.bin`); `fclose`/`fsync` before power-off.

**Buttons.**
- The rotary switch has external 10 k pull-ups (`board_pins.h`); upstream also enables internal
  pull-ups, which is harmless. GPIO0 is a strapping pin, so reading it after boot is fine, but held at
  power-on it enters download mode.
- Simplest port: keep the architecture (5 ms `esp_timer` or `Ticker` → per-button state machine →
  FreeRTOS event group or queue). Use the semantics table above, not the code: fix the
  PRESS_REPEAT/`handle->event` bug, give Boot's long-start and long-hold separate codes, and use a
  queue instead of a clear-all event group so simultaneous events aren't lost.
- If you use a library such as OneButton, set click ≈ 300 ms, long press ≈ 1000 ms, debounce ≈ 15 ms
  to keep the vendor UI feel (inferred: OneButton's own defaults differ).
- `LONG_PRESS_HOLD` at 5 ms means "auto-repeat at whatever rate the consumer polls". In the alarm
  page that's one step per loop iteration, gated by e-paper partial refreshes (~0.3 s). Throttle
  explicitly in our code.
- Single-click latency is 300 ms because the click is only reported once a double-click is ruled out.
  For pure navigation keys (Up/Down) consider acting on `PRESS_UP` instead.

## 6. Licensing

`i2c_bsp.*`, `sdcard_bsp.*`, `button_bsp.*`, the page sources and the Arduino `04`/`05` examples
carry **no license or copyright header**; the upstream repo has no top-level LICENSE.
`multi_button.c` / `multi_button.h` start with:

```c
/*
 * Copyright (c) 2016 Zibin Zheng <znbin@qq.com>
 * All rights reserved
 */
```

That's Zibin Zheng's MultiButton library. Its upstream project is published under MIT (inferred; no
license text is in this copy). `readme.txt` (Chinese) only describes the timer → `button_ticks` →
event-group flow. Re-implement the state machine from the table above rather than copying the files.
