# Audio: ES8311 codec, I2S, NS4150B amp, playback and recording

Vendor code at commit `9b12d40`. Paths are relative to `ESP-IDF/08_ESP32-S3_e-Paper-3.97/` unless
they start with `ESP-IDF/0x_…` or `Arduino/`. Pins and the I2C address match `include/board_pins.h`
(MCLK 13, BCLK 14, WS 47, DOUT 48, DIN 21, AMP_EN 39, ES8311 at 0x18). Codec registers are checked
against `docs/datasheets/ES8311-audio-codec.pdf` (Rev 10.0). Some behaviour lives inside the managed
component `espressif/esp_codec_dev` **1.3.6** (pinned in `dependencies.lock`). That code isn't in the
upstream repo. I read it from the component registry archive and label it "esp_codec_dev 1.3.6".

For voice (mic + speaker together, wake word, streaming to an AI server), see `ai-voice.md`. It covers XiaoZhi's
port to this board: one full-duplex I2S pair at 24 kHz, the amp switched with a 15 s idle timeout, and Opus.

## Contents

1. [Upstream file map](#1-upstream-file-map)
2. [Key facts](#2-key-facts)
3. [Codec and I2S bring-up (`es8311_bsp`)](#3-codec-and-i2s-bring-up-es8311_bsp)
4. [Volume, mute, mic gain](#4-volume-mute-mic-gain)
5. [Playback: built-in PCM and SD files](#5-playback-built-in-pcm-and-sd-files)
6. [Recording to SD (WAV)](#6-recording-to-sd-wav)
7. [Shutdown and amp handling](#7-shutdown-and-amp-handling)
8. [Minimal examples (02_Mic_test, 03_Music, Arduino 01_Audio_Test)](#8-minimal-examples)
9. [Porting notes](#9-porting-notes)
10. [Licensing](#10-licensing)

## 1. Upstream file map

Base URL: `https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/`

| What | Link |
| --- | --- |
| Pins, sample rate, MCLK multiple, mic gain defines | [es8311_bsp.h#L16-L33](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/components/es8311_bsp/es8311_bsp.h#L16-L33) |
| `es8311_codec_init()` (esp_codec_dev setup) | [es8311_bsp.c#L21-L121](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/components/es8311_bsp/es8311_bsp.c#L21-L121) |
| `i2s_driver_init()` (full-duplex std channel) | [es8311_bsp.c#L123-L154](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/components/es8311_bsp/es8311_bsp.c#L123-L154) |
| `es8311_audio_shutdown_cleanup()` | [es8311_bsp.c#L157-L197](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/components/es8311_bsp/es8311_bsp.c#L157-L197) |
| Caller: `axp_pwr_off()` runs the cleanup before PMIC shutdown | [axp_prot.cpp#L275-L282](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/components/axpPower/axp_prot.cpp#L275-L282) |
| I2S write / clock-reconfig / mute callbacks, `page_audio_int()` | [page_audio.cc#L82-L167](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/main/page_audio/page_audio.cc#L82-L167) |
| SD playback + volume/pause UI loop | [page_audio.cc#L170-L353](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/main/page_audio/page_audio.cc#L170-L353) |
| Built-in PCM playback (alarm sound) | [page_audio.cc#L357-L466](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/main/page_audio/page_audio.cc#L357-L466) |
| Audio menu, `page_audio_deinit()` | [page_audio.cc#L657-L750](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/main/page_audio/page_audio.cc#L657-L750) |
| WAV header, record filename, RX reconfig, record task + UI | [page_audio.cc#L753-L1080](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/main/page_audio/page_audio.cc#L753-L1080) |
| WAV/MP3 info parsers (ID3, frame header) | [page_audio.cc#L1352-L1630](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/main/page_audio/page_audio.cc#L1352-L1630) |
| `wav_header_t`, ID3/MP3 structs, `Fixed_volume_increment` | [page_audio.h#L29-L88](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/main/page_audio/page_audio.h#L29-L88) |
| Built-in clip header (skip the data) | [music.h#L1-L10](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/main/page_audio/music.h#L1-L10) |
| Init call order in `app_main` | [main.cc#L440-L491](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/main/main.cc#L440-L491) |
| 03_Music: minimal MP3-from-SD | [ESP-IDF/03_Music/main/main.c#L1-L139](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/03_Music/main/main.c#L1-L139) |
| 02_Mic_test: record 3 s / play back | [ESP-IDF/02_Mic_test/main/main.c#L13-L161](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/02_Mic_test/main/main.c#L13-L161), [audio_bsp.c#L18-L97](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/02_Mic_test/components/audio_bsp/audio_bsp.c#L18-L97), [board_cfg.txt#L86-L89](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/02_Mic_test/components/codec_board/board_cfg.txt#L86-L89) |
| Arduino: ESP_I2S + hand-rolled ES8311 driver | [01_Audio_Test.ino#L11-L70](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/Arduino/examples/01_Audio_Test/01_Audio_Test.ino#L11-L70), [es8311.cpp#L362-L420](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/Arduino/examples/01_Audio_Test/es8311.cpp#L362-L420) |

## 2. Key facts

| Item | Value in vendor code |
| --- | --- |
| Codec driver | `esp_codec_dev` ES8311 driver. Waveshare passes `ES8311_CODEC_DEFAULT_ADDR`, which is `0x30` (8-bit form). esp_codec_dev 1.3.6 shifts it right to 0x18 for `i2c_master` |
| I2S | `I2S_NUM_0`, ESP32 is master, one full-duplex STD (Philips) channel pair, 16-bit, stereo slots |
| Default rate | 16 kHz (`EXAMPLE_SAMPLE_RATE`). Changed at runtime by the MP3/WAV player and for the built-in clip |
| MCLK | `mclk_multiple = 384` in `i2s_driver_init()`, but `esp_codec_dev_open()` re-applies `I2S_STD_CLK_DEFAULT_CONFIG` (256×fs) right away, and the codec is set for `mclk_div` 256. **Effective MCLK = 256×fs** (esp_codec_dev 1.3.6 `audio_codec_data_i2s.c` `set_drv_fs`) |
| Amp (NS4150B) | GPIO39 is the codec's `pa_pin`. esp_codec_dev drives it **HIGH as soon as `es8311_codec_new()` opens the codec** and only drives it LOW on codec close/disable. Silence between clips is a DAC mute, not amp-off |
| Formats | MP3 + WAV from SD via `chmorgan/esp-audio-player` 1.0.7 (libhelix-mp3 1.0.3). `sdkconfig`: `CONFIG_AUDIO_PLAYER_ENABLE_MP3=y`, `…_WAV=y` |
| Recording | 16 kHz, 16-bit, mono, WAV to `/sdcard/music/record_%04d%02d%02d_%02d%02d%02d.wav` from the RTC. `years` is 0-99 (the app adds 2000 elsewhere), so names come out like `record_00261112_174535.wav` |
| Default volume | `int Volume = 90;` RAM only, resets on reboot, steps of 5 (`Fixed_volume_increment`) |

## 3. Codec and I2S bring-up (`es8311_bsp`)

Order in `app_main`: `i2c_master_init()` → … → `page_audio_int()`, which calls `i2s_driver_init()`
**then** `es8311_codec_init()`, so MCLK is already running when the codec is configured. In the snippets
below, comments starting `//>` are mine. Everything else is upstream text.

```c
// es8311_bsp.h
#define EXAMPLE_SAMPLE_RATE     (16000)
#define EXAMPLE_MCLK_MULTIPLE   (384) // If not using 24-bit data width, 256 should be enough
#define EXAMPLE_VOICE_VOLUME    80
#define EXAMPLE_MIC_GAIN        4
#define I2S_NUM                 I2S_NUM_0
#define I2S_MCLK_PIN            GPIO_NUM_13    // Master clock
#define I2S_BCK_PIN             GPIO_NUM_14    // Bit clock
#define I2S_WS_PIN              GPIO_NUM_47    // Word selection/frame clock
#define I2S_DATA_PIN            GPIO_NUM_21    // Data input
#define I2S_DATA_POUT           GPIO_NUM_48    // Data output
#define I2S_PA_PIN              GPIO_NUM_39    // PA power amplifier pins
#define I2C_NUM         (0)
```

```c
// es8311_bsp.c  i2s_driver_init()
i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM, I2S_ROLE_MASTER);
chan_cfg.auto_clear = true; // Auto clear the legacy data in the DMA buffer
ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle, &rx_handle));
i2s_std_config_t std_cfg = {
    .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(EXAMPLE_SAMPLE_RATE),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
    .gpio_cfg = { .mclk = I2S_MCLK_PIN, .bclk = I2S_BCK_PIN, .ws = I2S_WS_PIN,
                  .dout = I2S_DATA_POUT, .din = I2S_DATA_PIN, /* ... invert_flags all false */ },
};
std_cfg.clk_cfg.mclk_multiple = EXAMPLE_MCLK_MULTIPLE;
ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle, &std_cfg));
ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle, &std_cfg));
ESP_ERROR_CHECK(i2s_channel_enable(tx_handle));
ESP_ERROR_CHECK(i2s_channel_enable(rx_handle));
```

```c
// es8311_bsp.c  es8311_codec_init()  (trimmed)
audio_codec_i2c_cfg_t i2c_cfg = { .port = I2C_NUM, .addr = ES8311_CODEC_DEFAULT_ADDR,
                                  .bus_handle = (void *)i2c_bus_handle, };
const audio_codec_ctrl_if_t *i2c_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
es8311_codec_cfg_t es8311_cfg = {
    .ctrl_if = i2c_ctrl_if, .gpio_if = gpio_if,
    .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,  // Supports playback and recording
    .pa_pin = I2S_PA_PIN, .pa_reverted = false,
    .master_mode = false,   // The ESP32 serves as the I2S host
    .use_mclk = true, .digital_mic = false, .invert_mclk = false, .invert_sclk = false,
    .hw_gain = { .pa_voltage = 5.0, .codec_dac_voltage = 3.3, },
    .no_dac_ref = false,
    .mclk_div = 0,  // 0 indicates the use of the default value 256
};
const audio_codec_if_t *es8311_dev = es8311_codec_new(&es8311_cfg);
audio_codec_i2s_cfg_t i2s_cfg = { .port = I2S_NUM, .rx_handle = rx_handle, .tx_handle = tx_handle, };
const audio_codec_data_if_t *i2s_data_if = audio_codec_new_i2s_data(&i2s_cfg);
// ...
//> play_dev_handle (ESP_CODEC_DEV_TYPE_OUT) and record_dev_handle (..._IN) share es8311_dev + i2s_data_if
esp_codec_dev_sample_info_t fs = { .sample_rate = EXAMPLE_SAMPLE_RATE, .channel = 2, .bits_per_sample = 16, };
ESP_RETURN_ON_ERROR(esp_codec_dev_open(play_dev_handle, &fs), TAG, "open play device failed");
esp_codec_dev_sample_info_t fs_record = { .sample_rate = EXAMPLE_SAMPLE_RATE, .channel = 1, .bits_per_sample = 16, };
ESP_RETURN_ON_ERROR(esp_codec_dev_open(record_dev_handle, &fs_record), TAG, "open record device failed");
ESP_RETURN_ON_ERROR(esp_codec_dev_set_out_vol(play_dev_handle, 0), TAG, "set initial volume failed");
ESP_RETURN_ON_ERROR(esp_codec_dev_set_out_mute(play_dev_handle, true), TAG, "set initial mute failed");
ESP_RETURN_ON_ERROR(esp_codec_dev_set_in_gain(record_dev_handle, EXAMPLE_MIC_GAIN * 3.0f), TAG, "set mic gain failed");
```

What esp_codec_dev 1.3.6 does underneath. Re-implement this if you drop the component:

- `es8311_open()` (called from `es8311_codec_new`): writes REG44=0x08 twice ("first I2C write occasionally
  fails"), clock-manager defaults, REG00=0x80 (CSM on, slave), REG01=0x3F (MCLK from pin, all clocks on),
  REG13=0x10, REG1B=0x0A, REG1C=0x6A, then **REG44=0x58** because `no_dac_ref=false`. Datasheet REG44
  bits 6:4 = 5 means "ADC + DACR". The datasheet lists these as left + right, so the **mic is on the LEFT slot
  and a DAC loopback is on the RIGHT slot** (inferred from the "ADC + 0" / "0 + ADC" naming; the loopback is meant
  as an echo-cancel reference, also inferred). Then `es8311_pa_power(SETUP|ENABLE)` sets GPIO39 HIGH.
- `esp_codec_dev_open()` → I2S `set_fmt` (Philips slots, `I2S_STD_CLK_DEFAULT_CONFIG(rate)`, so 256×fs
  unless `fs.mclk_multiple` is set; Waveshare never sets it) → codec `set_fs` (word length, I2S format, clock
  dividers from a `{mclk, rate}` table) → codec `enable(true)`. That starts the codec: REG17=0xBF (ADC 0 dB),
  REG0E=0x02, REG12=0x00, REG14=0x1A (Mic1p-Mic1n, PGA 30 dB).
- A mono `fs` (channel=1) is turned into 2 slots with mask = slot 0 by the I2S data interface.
- The ES8311 DAC is mono. Datasheet REG09 bit 7 = 0 means "left channel data to DAC", so only the left
  slot of a stereo stream is heard.

## 4. Volume, mute, mic gain

| API (esp_codec_dev 1.3.6) | Effect |
| --- | --- |
| `esp_codec_dev_set_out_vol(h, v)` | Default curve maps v 0…100 to −50…0 dB, linear in dB. `es8311_set_vol` subtracts `hw_gain = 20·log10(3.3/5.0) ≈ −3.6 dB`, then REG32 = (dB + 95.5) × 2. **v=100 → +3.6 dB (0xC6), v=90 → −1.4 dB (0xBC), v=0 → −46.4 dB (0x62)**, so v=0 is not silent |
| `esp_codec_dev_set_out_mute(h, m)` | REG31 bits 6:5 (DAC DSM + DEM mute) set or cleared. The amp stays powered |
| `esp_codec_dev_set_in_gain(h, dB)` | REG16 ADC scale-up in 6 dB steps (rounded down). Waveshare passes 4×3 = **12 dB → REG16=2**. The analog PGA stays at 30 dB (REG14) |

Datasheet check: REG32 DAC_VOLUME 0x00 = −95.5 dB, 0.5 dB/step, 0xBF = 0 dB, 0xFF = +32 dB. REG16 bits 2:0 =
0…7 → 0…42 dB. REG14 bits 3:0 = PGA 0…30 dB in 3 dB steps, bit 4 selects Mic1p-Mic1n. REG17 ADC_VOLUME
uses the same scale as REG32.

## 5. Playback: built-in PCM and SD files

**Player setup** (`page_audio_int`, after the codec): `esp-audio-player` gets three callbacks.

```c
static esp_err_t bsp_i2s_write(void *audio_buffer, size_t len, size_t *bytes_written, uint32_t timeout_ms) {
    return i2s_channel_write(tx_handle, (char *)audio_buffer, len, bytes_written, timeout_ms);
}
static esp_err_t bsp_i2s_reconfig_clk(uint32_t rate, uint32_t bits_cfg, i2s_slot_mode_t ch) {
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(rate),          //> mclk_multiple = 256
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG((i2s_data_bit_width_t)bits_cfg, (i2s_slot_mode_t)ch),
        .gpio_cfg = { /* ... same pins as i2s_driver_init */ },
    };
    ret |= i2s_channel_disable(tx_handle);
    ret |= i2s_channel_reconfig_std_clock(tx_handle, &std_cfg.clk_cfg);
    ret |= i2s_channel_reconfig_std_slot(tx_handle, &std_cfg.slot_cfg);
    ret |= i2s_channel_enable(tx_handle);
    // ...
}
static esp_err_t audio_mute_function(AUDIO_PLAYER_MUTE_SETTING setting) {
    esp_codec_dev_set_out_mute(play_dev_handle, setting == AUDIO_PLAYER_MUTE);   //> condensed from if/else
}
audio_player_config_t config;
config.mute_fn = audio_mute_function; config.write_fn = bsp_i2s_write; config.clk_set_fn = bsp_i2s_reconfig_clk;
config.priority = 5; config.coreID = 1;
audio_player_new(config);
```

esp-audio-player 1.0.7 internals (from its source): it detects MP3, then WAV, from the `FILE*`. It
**converts mono to stereo** ("es8311 requires stereo input"), calls `clk_set_fn` only when rate/bits/channels
change, writes with `portMAX_DELAY`, calls `mute_fn(UNMUTE)` before and `mute_fn(MUTE)` after each file, and
**`fclose()`s the FILE itself**. Its decoder task has a 4 KB stack.

**SD file playback** `page_audio_play_file(name)`: unmute, set `Volume`, `fopen("/sdcard/music/<name>")`,
`audio_player_play(fp)` (the caller only fcloses on error). Then a UI loop on the caller's task:

```c
do {
    state = audio_player_get_state();
    button = wait_key_event_and_return_code(100);       //> TickType_t; CONFIG_FREERTOS_HZ=100 -> 1 s
    if (button == 0) {// Volume +
        Volume = Volume + Fixed_volume_increment;
        if (Volume > 100) Volume = 100;
        esp_codec_dev_set_out_vol(play_dev_handle, (int)Volume);
        // ... EPD_Init(), redraw volume, EPD_Sleep()
    } else if (button == 14) { // Volume -
        // ... mirror image, clamp at 0
    } else if (button == 7) { // mode control
        audio_play_state = !audio_play_state;
        if (audio_play_state) { audio_player_pause(); /* ... */ } else { audio_player_resume(); /* ... */ }
    } else if (button == 8 || button == 22) { // 退出
        esp_codec_dev_set_out_mute(play_dev_handle, true);
        audio_player_stop();
        break;
    }
    // ... per-minute clock redraw via RTC
} while (state != AUDIO_PLAYER_STATE_IDLE && state != AUDIO_PLAYER_STATE_SHUTDOWN);
esp_codec_dev_set_out_mute(play_dev_handle, true);
```

Buttons: 0 = Up click, 14 = Down click, 7 = Function click, 8 = Function double-click, 22 = BOOT
double-click (see `io.md`). The file list is `/sdcard/music`, filtered by `.wav/.WAV/.mp3/.MP3`
substrings (`get_music_files`). The paged picker uses `list_dir_page()` from `file_browser`.

**Built-in clip** (`page_audio_play_memory`, used by alarms in clock/calendar/weather modes and the
alarm page): writes `music.h`'s `audio_data[]` straight to I2S in 1 KB chunks. It bypasses esp-audio-player
and polls keys between chunks:

```c
esp_codec_dev_set_out_mute(play_dev_handle, false);
esp_codec_dev_set_out_vol(play_dev_handle, (int)Volume);
bsp_i2s_reconfig_clk(16000, 16, I2S_SLOT_MODE_MONO);
while (offset < AUDIO_SAMPLES * sizeof(uint16_t)) {
    // write_size = min(1024, remaining)
    bsp_i2s_write((void*)((uint8_t*)audio_data + offset), write_size, &bytes_written, 1000);
    offset += write_size;
    if (wait_key_event_and_return_code(1) != -1) break;   //> condensed; any key aborts
}
esp_codec_dev_set_out_mute(play_dev_handle, true);
```

`music.h` says `// Sampling rate: 24000Hz … Audio duration: 14.51s` and `AUDIO_SAMPLES = 348299` (348299/24000 =
14.51 s). The factory app plays it at **16 kHz**, so it runs about 1.5× slow and low. The Arduino example plays
the same clip at 24 kHz. Treat 24 kHz as correct (inferred from the header comment and the sample count).

## 6. Recording to SD (WAV)

`page_audio_record_control()` starts `page_audio_record_task` (`xTaskCreate`, 8 KB, prio 5) and drives
a `record_state` flag: Function click toggles RECORDING/PAUSED, double-click (8/22) sets STOPPING.

```c
//> configure_recording_mode(16000): RX only, mono, left slot
.slot_cfg = { .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT, .slot_bit_width = I2S_SLOT_BIT_WIDTH_16BIT,
              .slot_mode = I2S_SLOT_MODE_MONO, .slot_mask = I2S_STD_SLOT_LEFT,
              .ws_width = I2S_DATA_BIT_WIDTH_16BIT, .ws_pol = false, .bit_shift = true,
              .left_align = true, .big_endian = false, .bit_order_lsb = false },
ret |= i2s_channel_disable(rx_handle);
ret |= i2s_channel_reconfig_std_clock(rx_handle, &std_cfg.clk_cfg);
ret |= i2s_channel_reconfig_std_slot(rx_handle, &std_cfg.slot_cfg);
ret |= i2s_channel_enable(rx_handle);
esp_codec_dev_set_in_gain(record_dev_handle, (float)EXAMPLE_MIC_GAIN * 3.0f);

// page_audio_record_task
write_wav_header(fp, sample_rate, channels, bits_per_sample, 0);       //> placeholder sizes
uint8_t *buffer = (uint8_t*)malloc(buffer_size);                      //> buffer_size = 512
for (int i = 0; i < 5; i++) i2s_channel_read(rx_handle, dummy_buffer, sizeof(dummy_buffer), &dummy_bytes, 10);
while (record_state != RECORD_STATE_STOPPING) {
    if (record_state == RECORD_STATE_RECORDING) {
        ret = i2s_channel_read(rx_handle, buffer, buffer_size, &bytes_read, 50);
        if (ret == ESP_OK && bytes_read > 0) { fwrite(buffer, 1, bytes_read, fp); total_bytes += bytes_read; }
    } else if (record_state == RECORD_STATE_PAUSED) { vTaskDelay(pdMS_TO_TICKS(100)); }
}
fseek(fp, 0, SEEK_SET);
write_wav_header(fp, sample_rate, channels, bits_per_sample, total_bytes);   //> patch sizes
fclose(fp);
```

`write_wav_header` writes a plain 44-byte PCM header (`audio_format=1`, `fmt_size=16`,
`file_size = data_size + 44 − 8`) from `wav_header_t` (naturally aligned, so not `packed`). The mic data
it records is the left slot, which carries the ADC per REG44=0x58 (see §3). Reading the RX channel directly
bypasses `esp_codec_dev_read`.

## 7. Shutdown and amp handling

`axp_pwr_off()` → `es8311_audio_shutdown_cleanup()` → `axp2101.shutdown()`. The cleanup mutes, waits
100 ms, sets volume 0, waits 100 ms, disables TX then RX (50 ms each), closes both codec devices (50 ms each)
(close → `es8311_suspend` + PA LOW), then `gpio_set_level(I2S_PA_PIN, 0)` with 100 ms waits on both sides.
That is about 700 ms of blocking delays, aimed at avoiding a pop at power-off (inferred intent).
`page_audio_deinit()` only deletes the player. Nothing else in the factory app turns the amp off, so
**AMP_EN stays HIGH from boot to shutdown** whenever the app is running.

## 8. Minimal examples

- **03_Music** (`ESP-IDF/03_Music/main/main.c`): same `es8311_bsp` (identical to the factory copy) + same
  three callbacks. `app_main`: `_sdcard_init(); i2c_master_init(); page_audio_int();`, then
  `fopen("/sdcard/music/gs-16b-1c-44100hz.mp3")`, unmute, `esp_codec_dev_set_out_vol(…, 70)`,
  `audio_player_play(fp)`. Its lock file pins esp_codec_dev **1.5.4** and IDF 5.5.1 (the factory app pins 1.3.6 / IDF 5.4.1).
- **02_Mic_test**: uses Espressif's `codec_board` helper (v0.5.5) with the board entry
  `Board: S3_ePaper_3_97 / i2c: {sda: 41, scl: 42} / i2s: {bclk: 14, ws: 47, dout: 48, din: 21, mclk: 13} /
  in_out: {codec: ES8311, pa: 39, use_mclk: 1, pa_gain:6}`. `audio_play_init()` sets out vol 100, in gain 45 dB
  (→ REG16 max 42 dB) and opens both devices at 16 kHz/2 ch/16-bit. BOOT long-press records 3 s
  (16000·2·2·3 = 192000 bytes, `malloc`) with `esp_codec_dev_read`. Short press plays it back with
  `esp_codec_dev_write`. Note the stereo capture: per REG44 the right channel is the DAC loopback.
- **Arduino 01_Audio_Test**: no esp_codec_dev. A copy of Espressif's legacy `es8311` driver talks over `Wire`.

```cpp
#define EXAMPLE_SAMPLE_RATE (24000)
#define EXAMPLE_MCLK_MULTIPLE (256)
Wire.begin(I2C_SDA, I2C_SCL);
pinMode(PA_CTRL, OUTPUT);
digitalWrite(PA_CTRL, HIGH);                                     //> amp on, never turned off
es8311_handle_t es_handle = es8311_create(I2C_NUM_0, ES8311_ADDRRES_0);   //> 0x18
const es8311_clock_config_t es_clk = { .mclk_inverted = false, .sclk_inverted = false,
    .mclk_from_mclk_pin = true, .mclk_frequency = EXAMPLE_MCLK_FREQ_HZ, .sample_frequency = EXAMPLE_SAMPLE_RATE };
es8311_init(es_handle, &es_clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16);
es8311_voice_volume_set(es_handle, EXAMPLE_VOICE_VOLUME /*70*/, NULL);
es8311_microphone_config(es_handle, false);
i2s.setPins(I2S_BCK_PIN, I2S_LRCK_PIN, I2S_DOUT_PIN, I2S_DIN_PIN, I2S_MCK_PIN);
i2s.begin(I2S_MODE_STD, EXAMPLE_SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO, I2S_STD_SLOT_LEFT);
//> loop():
i2s.write((uint8_t *)audio_data, AUDIO_SAMPLES * 2);
```

`es8311_init` register sequence: REG00=0x1F, 20 ms, REG00=0x00, REG00=0x80, clock config (REG01=0x3F,
REG02…REG08 from the coefficient table), REG00 bit 6 cleared (slave), REG09/REG0A word length (16-bit = 3<<2),
REG0D=0x01, REG0E=0x02, REG12=0x00, REG13=0x10, REG1C=0x6A, REG37=0x08. `es8311_microphone_config`:
REG17=0xC8, REG14=0x1A. **Volume formula `reg32 = volume*256/100 − 1` maps 100 → 0xFF = +32 dB and 75 → 0xBF = 0 dB**,
so volumes above about 75 add digital gain and can clip. Here the codec is configured *before* `i2s.begin()`
starts MCLK. It works because register writes only need I2C (inferred).

## 9. Porting notes

**Verified on hardware** (our `src/es8311.cpp` + `src/audio.cpp`, 2026-11-12): Option B works. The
ES8311 ACKs at 0x18 on the shared `Wire` bus. Our init: REG44=0x08 ×2, REG00=0x1F, 20 ms, then the
§8 sequence with the 256×fs clock row (REG02=00 03=10 04=10 05=00 06=03 07=00 08=FF), REG09/0A=0x0C,
and volume from REG32 = 0xBF − (100 − v). Played through the speaker with IDF `i2s_std` TX only,
master, MCLK 256×fs, 16-bit stereo Philips, with the stereo downmixed into both slots. AMP_EN goes
HIGH after the codec init and LOW on stop; no pop was reported. Reclocking I2S alone between
44.1 kHz stereo and 22.05 kHz mono files worked with no codec change. libhelix (core-bundled)
decodes 44.1 kHz/192 kbps stereo in ~3.8 ms per frame (~14% of one core) with its state in PSRAM.
With 8×511-frame DMA buffers there were 0 underruns while the UI browsed. One SD CRC error ended
a track early before we dropped SD to 20 MHz (see `io.md`).

**Recording, verified on hardware** (`src/audio.cpp` recordStart/recordStep, 2026-11-13): I2S_NUM_0
as RX-only master at 16 kHz, MCLK 256×fs, then `es8311::begin()` plus REG14=0x1A, REG17=0xBF,
REG16=2 (PGA 30 dB + 12 dB digital); REG44 stays 0x08. Room noise read about −51 dBFS. **On the S3,
`I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(16BIT, MONO)` leaves `slot_mask = I2S_STD_SLOT_BOTH`** (only the
classic-ESP32 branch of the macro picks LEFT), so a "mono" RX channel still delivered both slots: a
19 s timer after 11 s of recording. Set `slot_mask = I2S_STD_SLOT_LEFT` yourself, as Waveshare's
`configure_recording_mode` does. After that, 10.43 s of wall time gave 330,752 data bytes (10.34 s,
with ~100 ms of start-up dropped). 8 KB `fwrite`s from internal RAM kept up with no lost samples.
The memo played back through the normal TX path as a WAV.

**Option A, recommended: keep `esp_codec_dev` from Arduino.** It isn't in the Arduino 3.3.12 precompiled libs
(checked `framework-arduinoespressif32-esp32s3-libs/include`). Vendor the Apache-2.0 component into `lib/`
and compile only `esp_codec_dev*.c`, `audio_codec_sw_vol.c`, `platform/*.c` and `device/es8311/es8311.c`
(its CMake picks device files from Kconfig). Define `CONFIG_CODEC_ES8311_SUPPORT=1` yourself, because
`esp_codec_dev_defaults.h` gates the ES8311 API on it. On IDF ≥5.3 it uses `i2c_master`, so pass
`.bus_handle = i2cBusHandle(0)` after `Wire.begin(...)`. For I2S, either create the channels with IDF calls as
Waveshare does, or use `ESP_I2S` and hand `i2s.txChan()` / `i2s.rxChan()` to `audio_codec_new_i2s_data` (inferred
to work; ESP_I2S exposes both handles).

**Option B: register-level driver** (port of the Arduino example) on the shared `Wire` bus. Use the
esp_codec_dev 1.3.6 `es8311_open`/`es8311_start` sequences from §3 rather than the older Arduino one if you
want the REG44 loopback layout and the I2C-noise workaround.

**MCLK.** The board wires MCLK (GPIO13). Use MCLK = 256×fs. `ESP_I2S` hard-codes `I2S_MCLK_MULTIPLE_256`, and
every common rate from 8 kHz to 48 kHz has a 256×fs row in both coefficient tables. Waveshare's 384 setting never takes effect.

**Rate changes.** When esp-audio-player switches to 44.1 kHz it only reclocks I2S TX; the codec keeps its
16 kHz divider set. That works because all 256×fs rows share the same dividers, except DAC OSR (0x20 at
≤16 kHz vs 0x10 at ≥22.05 kHz in esp_codec_dev 1.3.6). Inferred: call `esp_codec_dev_close` + `open` with the new
`fs` instead, which also keeps the codec state honest.

**Full-duplex clock.** esp_codec_dev notes "Set RX clock will not take effect if in full duplex mode … TX
is master". Waveshare's recording path reconfigures only RX, so after playing a 44.1 kHz MP3 the recording
could run at the wrong rate (inferred). Reconfigure TX too, or record via `esp_codec_dev_open(record, …)`.

**Amp / pop.** Follow AGENTS.md: AMP_EN HIGH only while playing. With esp_codec_dev that means
`esp_codec_dev_open(play)` before a clip and `esp_codec_dev_close(play)` after it. Or set `pa_pin = -1` and drive
GPIO39 yourself: unmute and ramp after amp-on, mute before amp-off. REG37 bits 7:4 (DAC ramp rate) can soften
transitions (`es8311_voice_fade` in the Arduino driver). Waveshare's ~700 ms cleanup is a good model for shutdown.

**Volume.** `esp_codec_dev` volume 0 ≠ silence (−46 dB). Always mute as well. Raw REG32 above 0xBF is
positive digital gain. The ES8311 DAC is mono (left slot), and stereo files lose the right channel
unless you downmix.

**Mono capture.** Mic = left slot. Right slot = DAC loopback when REG44=0x58. Read the left slot only, or set
`no_dac_ref = true` (REG44=0x08, ADC+ADC) if you don't need an echo reference.

**MP3 on Arduino.** The core bundles `chmorgan__esp-libhelix-mp3` (`mp3dec.h`). `ESP_I2S::playMP3()` only
decodes from memory, so for SD streaming either vendor esp-audio-player (Apache-2.0) or drive
`MP3FindSyncWord`/`MP3Decode` yourself as `ESP_I2S.cpp` does. `ESP_I2S::recordWAV()`/`playWAV()` cover
short in-RAM clips. Put large buffers (192 KB for 3 s stereo) in PSRAM.

**Blocking.** All vendor playback/recording loops block the UI task. `i2s_channel_write` with
`portMAX_DELAY` blocks the player task until DMA drains. `wait_key_event_and_return_code()` takes
**ticks**, and the vendor sdkconfig has `CONFIG_FREERTOS_HZ=100`. Check our core's tick rate before reusing raw
numbers like `100` or `1`.

## 10. Licensing

- Waveshare's own files (`es8311_bsp.*`, `page_audio.*`, `audio_bsp.*`, `main.c` files, `01_Audio_Test.ino`)
  carry **no license header**, and the repo has no LICENSE file.
- `Arduino/examples/01_Audio_Test/es8311.{h,cpp}`, `es8311_reg.h`: `SPDX-FileCopyrightText: 2015-2022 Espressif
  Systems (Shanghai) CO LTD`, `SPDX-License-Identifier: Apache-2.0`.
- `ESP-IDF/02_Mic_test/components/codec_board`: LICENSE "Espressif Modified MIT License" (use only with
  Espressif products), `SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT`. Headers say "ESPRESSIF MIT License".
- Managed components (not in the repo; component-registry metadata): `espressif/esp_codec_dev` 1.3.6 Apache-2.0,
  `chmorgan/esp-audio-player` 1.0.7 Apache-2.0, `chmorgan/esp-libhelix-mp3` 1.0.3 Apache-2.0 wrapper. The bundled
  Helix decoder ships `docs/RPSL.txt` and `docs/RCSL.txt` (RealNetworks licenses), so review those before redistributing.
