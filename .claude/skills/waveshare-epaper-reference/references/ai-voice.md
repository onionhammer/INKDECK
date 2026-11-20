# AI voice: XiaoZhi on this board (wake word, AFE, Opus, cloud protocol)

Waveshare's AI-voice support for this board is **XiaoZhi** (小智), an open-source voice-chatbot firmware.
The vendor repo has only prebuilt images (`Firmware/xiaozhi/*.bin`). The wiki has a flashing tutorial,
mirrored in [`docs/wiki/05a-XiaoZhi-AI.md`](../../../../docs/wiki/05a-XiaoZhi-AI.md). The source lives
upstream in **<https://github.com/78/xiaozhi-esp32>**, which carries a board port for this exact board
(`main/boards/waveshare/esp32-s3-epaper-3.97/`, added 2026-03-14 in PR #1808, moved 2026-07-27).

**Pinned upstream:** commit `67ca62d4bb7d91e514d7ccd5e6977745002f41f4` (2026-11-12). Every snippet and link
below is from that commit, not from the Waveshare commit the other references use. In the snippets,
comments starting `//>` are mine. Everything else is upstream text. XiaoZhi is a full ESP-IDF ≥ 6.0
application, so none of it drops into our Arduino build as is. §11 is the porting plan and a standalone
sample written for our stack.

## Contents

1. [Prebuilt images](#1-prebuilt-images)
2. [Upstream file map](#2-upstream-file-map)
3. [Key facts](#3-key-facts)
4. [Board port: pins, PMIC, buttons](#4-board-port-pins-pmic-buttons)
5. [Codec: `Es8311AudioCodec`](#5-codec-es8311audiocodec)
6. [Audio pipeline: tasks, queues, Opus](#6-audio-pipeline-tasks-queues-opus)
7. [AFE: wake word and VAD](#7-afe-wake-word-and-vad)
8. [Device states and listening modes](#8-device-states-and-listening-modes)
9. [Server protocol](#9-server-protocol)
10. [E-paper UI](#10-e-paper-ui)
11. [Porting notes and a standalone sample](#11-porting-notes-and-a-standalone-sample)
12. [Licensing](#12-licensing)

## 1. Prebuilt images

| Source | File | Notes |
| --- | --- | --- |
| Vendor repo at `9b12d40`, `Firmware/xiaozhi/` | `ESP32-S3_e-Paper-3.97_xiaozhi.bin` (9.9 MB) | `ReadMe_EN.txt`: "Pure voice, no display" |
| same | `ESP32-S3_e-Paper-3.97_xiaozhi-xs.bin` (11.2 MB) | "The e-ink screen is displayed in landscape" |
| [waveshareteam/ESP32-AIChats](https://github.com/waveshareteam/ESP32-AIChats/tree/master/xiaozhi-esp32) | `v2.5.2/ESP32-S3/v2.5.2_waveshare-esp32-s3-epaper-3.97.zip` | Tested by Waveshare, lags upstream releases |
| [XiaoZhi releases](https://github.com/78/xiaozhi-esp32/releases) | per-board assets | Official builds |

The images are merged binaries written at `0x0` (wiki `04-Firmware-Flashing.md`). Flashing one replaces our
firmware and its partition table. Get back with `pio run -t upload`, or use the BOOT recovery in AGENTS.md if
the port has gone. First boot opens a `Xiaozhi-xxxxxx` hotspot for Wi-Fi setup (`192.168.4.1`, 2.4 GHz only).
After that the device speaks a 6-digit code that binds it to an account on <https://xiaozhi.me>.

## 2. Upstream file map

Base URL: `https://github.com/78/xiaozhi-esp32/blob/67ca62d4bb7d91e514d7ccd5e6977745002f41f4/`

| What | Link |
| --- | --- |
| Board pins, sample rates | [config.h#L1-L44](https://github.com/78/xiaozhi-esp32/blob/67ca62d4bb7d91e514d7ccd5e6977745002f41f4/main/boards/waveshare/esp32-s3-epaper-3.97/config.h#L1-L44) |
| PMIC init (`class Pmic`) | [waveshare-s3-epaper-3.97.cc#L22-L54](https://github.com/78/xiaozhi-esp32/blob/67ca62d4bb7d91e514d7ccd5e6977745002f41f4/main/boards/waveshare/esp32-s3-epaper-3.97/waveshare-s3-epaper-3.97.cc#L22-L54) |
| Power-save timer, I2C bus, buttons | [waveshare-s3-epaper-3.97.cc#L65-L105](https://github.com/78/xiaozhi-esp32/blob/67ca62d4bb7d91e514d7ccd5e6977745002f41f4/main/boards/waveshare/esp32-s3-epaper-3.97/waveshare-s3-epaper-3.97.cc#L65-L105) |
| Codec instance, battery, power-save hook | [waveshare-s3-epaper-3.97.cc#L147-L176](https://github.com/78/xiaozhi-esp32/blob/67ca62d4bb7d91e514d7ccd5e6977745002f41f4/main/boards/waveshare/esp32-s3-epaper-3.97/waveshare-s3-epaper-3.97.cc#L147-L176) |
| E-paper LVGL flush and init | [custom_lcd_display.cc#L17-L79](https://github.com/78/xiaozhi-esp32/blob/67ca62d4bb7d91e514d7ccd5e6977745002f41f4/main/boards/waveshare/esp32-s3-epaper-3.97/custom_lcd_display.cc#L17-L79) |
| AXP2101 helper (battery, `PowerOff`) | [boards/common/axp2101.cc](https://github.com/78/xiaozhi-esp32/blob/67ca62d4bb7d91e514d7ccd5e6977745002f41f4/main/boards/common/axp2101.cc) |
| ES8311 codec: ctor, reset, amp, duplex I2S | [es8311_audio_codec.cc#L9-L171](https://github.com/78/xiaozhi-esp32/blob/67ca62d4bb7d91e514d7ccd5e6977745002f41f4/main/audio/codecs/es8311_audio_codec.cc#L9-L171) |
| DMA sizes, default volume | [audio_codec.h#L15-L16](https://github.com/78/xiaozhi-esp32/blob/67ca62d4bb7d91e514d7ccd5e6977745002f41f4/main/audio/audio_codec.h#L15-L16), [#L64-L65](https://github.com/78/xiaozhi-esp32/blob/67ca62d4bb7d91e514d7ccd5e6977745002f41f4/main/audio/audio_codec.h#L64-L65) |
| Pipeline diagram, Opus encoder config, queue sizes | [audio_service.h#L29-L80](https://github.com/78/xiaozhi-esp32/blob/67ca62d4bb7d91e514d7ccd5e6977745002f41f4/main/audio/audio_service.h#L29-L80) |
| `Initialize`, `Start` (tasks) | [audio_service.cc#L48-L171](https://github.com/78/xiaozhi-esp32/blob/67ca62d4bb7d91e514d7ccd5e6977745002f41f4/main/audio/audio_service.cc#L48-L171) |
| Mic read + resample, input/output/Opus tasks | [audio_service.cc#L195-L530](https://github.com/78/xiaozhi-esp32/blob/67ca62d4bb7d91e514d7ccd5e6977745002f41f4/main/audio/audio_service.cc#L195-L530) |
| 15 s codec power-down | [audio_service.cc#L837-L857](https://github.com/78/xiaozhi-esp32/blob/67ca62d4bb7d91e514d7ccd5e6977745002f41f4/main/audio/audio_service.cc#L837-L857) |
| AFE config (WakeNet, VAD, AEC) | [afe_audio_engine.cc#L51-L256](https://github.com/78/xiaozhi-esp32/blob/67ca62d4bb7d91e514d7ccd5e6977745002f41f4/main/audio/engines/afe_audio_engine.cc#L51-L256) |
| VAD edges, 60 ms re-framing | [afe_audio_engine.cc#L483-L513](https://github.com/78/xiaozhi-esp32/blob/67ca62d4bb7d91e514d7ccd5e6977745002f41f4/main/audio/engines/afe_audio_engine.cc#L483-L513) |
| Packet structs, binary headers, listening modes | [protocol.h#L10-L41](https://github.com/78/xiaozhi-esp32/blob/67ca62d4bb7d91e514d7ccd5e6977745002f41f4/main/protocols/protocol.h#L10-L41) |
| JSON control messages (device → server) | [protocol.cc#L77-L117](https://github.com/78/xiaozhi-esp32/blob/67ca62d4bb7d91e514d7ccd5e6977745002f41f4/main/protocols/protocol.cc#L77-L117) |
| WebSocket: framing, headers, hello | [websocket_protocol.cc#L24-L260](https://github.com/78/xiaozhi-esp32/blob/67ca62d4bb7d91e514d7ccd5e6977745002f41f4/main/protocols/websocket_protocol.cc#L24-L260) |
| Incoming JSON handling (server → device) | [application.cc#L570-L764](https://github.com/78/xiaozhi-esp32/blob/67ca62d4bb7d91e514d7ccd5e6977745002f41f4/main/application.cc#L570-L764) |
| State changes (what audio runs in each state) | [application.cc#L1033-L1107](https://github.com/78/xiaozhi-esp32/blob/67ca62d4bb7d91e514d7ccd5e6977745002f41f4/main/application.cc#L1033-L1107) |
| OTA check = server discovery, activation | [ota.cc#L72-L281](https://github.com/78/xiaozhi-esp32/blob/67ca62d4bb7d91e514d7ccd5e6977745002f41f4/main/ota.cc#L72-L281), [#L490-L560](https://github.com/78/xiaozhi-esp32/blob/67ca62d4bb7d91e514d7ccd5e6977745002f41f4/main/ota.cc#L490-L560) |
| Kconfig: OTA URL, wake word, AFE, AEC | [Kconfig.projbuild#L3](https://github.com/78/xiaozhi-esp32/blob/67ca62d4bb7d91e514d7ccd5e6977745002f41f4/main/Kconfig.projbuild#L3-L7), [#L987-L1089](https://github.com/78/xiaozhi-esp32/blob/67ca62d4bb7d91e514d7ccd5e6977745002f41f4/main/Kconfig.projbuild#L987-L1089) |
| Protocol docs | [docs/websocket.md](https://github.com/78/xiaozhi-esp32/blob/67ca62d4bb7d91e514d7ccd5e6977745002f41f4/docs/websocket.md), [docs/mqtt-udp.md](https://github.com/78/xiaozhi-esp32/blob/67ca62d4bb7d91e514d7ccd5e6977745002f41f4/docs/mqtt-udp.md), [docs/mcp-protocol.md](https://github.com/78/xiaozhi-esp32/blob/67ca62d4bb7d91e514d7ccd5e6977745002f41f4/docs/mcp-protocol.md), [docs/custom-board.md](https://github.com/78/xiaozhi-esp32/blob/67ca62d4bb7d91e514d7ccd5e6977745002f41f4/docs/custom-board.md) |

## 3. Key facts

| Item | XiaoZhi on this board |
| --- | --- |
| Build | ESP-IDF ≥ 6.0.1, `idf.py menuconfig` → Xiaozhi Assistant → Board Type → "Waveshare ESP32-S3-e-Paper-3.97" (`BOARD_TYPE_WAVESHARE_ESP32_S3_ePaper_3_97`). Partition table `partitions/v2/16m.csv` |
| Codec rate | **24 kHz in and out** (one duplex I2S pair, so the rates must match). MCLK 256×fs |
| Uplink | Mic 24 kHz → resampled to 16 kHz → AFE (VAD, optional wake word) → Opus 16 kHz mono, **60 ms frames**, VBR + DTX, complexity 0 |
| Downlink | Opus at the rate in the server hello (default 24 kHz, 60 ms) → decoded straight to the codec rate. A resampler is made only if they differ |
| Wake word | Kconfig default **disabled**. `scripts/build.py` picks `wn9_nihaoxiaozhi_tts` ("你好小智") for zh-CN/zh-TW builds and `wn9_hiesp` ("Hi ESP") otherwise. The model needs the `model` flash partition |
| AEC | No echo reference on this board (`input_reference_ = false`), and `USE_DEVICE_AEC` isn't offered for it. So the listening mode is **auto-stop, half-duplex**: the mic isn't streamed while the reply plays |
| Transport | WebSocket or MQTT + UDP, whichever the OTA endpoint returns. Default OTA URL `https://api.tenclass.net/xiaozhi/ota/` |
| Amp (GPIO39) | Follows "output enabled": on when playback starts, off after **15 s** without output (unless the mic is also running) |
| PMIC | Re-inits rails, charges at **200 mA** (comment says 400 mA), PWR click = power-off, auto power-off after 300 s idle on battery |
| Unused here | RTC, SHTC3, IMU, SD card, rotary switch. GPIO1 and GPIO0 are the only buttons |

## 4. Board port: pins, PMIC, buttons

**Pins.** Every pin in `config.h` matches `include/board_pins.h`:

```c
#define AUDIO_INPUT_SAMPLE_RATE  24000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000
#define AUDIO_I2S_GPIO_MCLK  GPIO_NUM_13
#define AUDIO_I2S_GPIO_WS    GPIO_NUM_47
#define AUDIO_I2S_GPIO_BCLK  GPIO_NUM_14
#define AUDIO_I2S_GPIO_DIN   GPIO_NUM_21
#define AUDIO_I2S_GPIO_DOUT  GPIO_NUM_48
#define AUDIO_CODEC_PA_PIN       GPIO_NUM_39
#define AUDIO_CODEC_I2C_SDA_PIN  GPIO_NUM_41
#define AUDIO_CODEC_I2C_SCL_PIN  GPIO_NUM_42
#define AUDIO_CODEC_ES8311_ADDR  ES8311_CODEC_DEFAULT_ADDR   //> 0x30, 8-bit form of 0x18 (see audio.md §2)
#define BOOT_BUTTON_GPIO        GPIO_NUM_0
#define VBAT_PWR_GPIO           GPIO_NUM_1                   //> our board::PWR_KEY_SENSE
#define EPD_SPI_NUM        SPI3_HOST
#define EPD_DC_PIN    GPIO_NUM_9
#define EPD_CS_PIN    GPIO_NUM_10
#define EPD_SCK_PIN   GPIO_NUM_11
#define EPD_MOSI_PIN  GPIO_NUM_12
#define EPD_RST_PIN   GPIO_NUM_46
#define EPD_BUSY_PIN  GPIO_NUM_3
```

**I2C.** The board creates its own `i2c_master` bus on port 0 (internal pull-ups, `glitch_ignore_cnt = 7`) and
hands it to both the PMIC and the codec.

**PMIC** (`class Pmic`, run once in the board constructor). Raw register writes, no XPowersLib:

```cpp
WriteReg(0x22, 0b110);  // PWRON > OFFLEVEL as POWEROFF Source enable
WriteReg(0x27, 0x10);   // hold 4s to power off
// Disable All DCs but DC1
WriteReg(0x80, 0x01);
// Disable All LDOs
WriteReg(0x90, 0x00);
WriteReg(0x91, 0x00);
// Set DC1 to 3.3V
WriteReg(0x82, (3300 - 1500) / 100);
// Set ALDO1 to 3.3V
WriteReg(0x92, (3300 - 500) / 100);
// Set ALDO2 to 3.3V
WriteReg(0x93, (3300 - 500) / 100);
// Set ALDO3 to 3.3V
WriteReg(0x94, (3300 - 500) / 100);
// Enable ALDO1、ALDO2、ALDO2
WriteReg(0x90, 0x07);
WriteReg(0x64, 0x03);  // CV charger voltage setting to 4.2V
WriteReg(0x61, 0x02);  // set Main battery precharge current to 50mA
WriteReg(0x62, 0x08);  // set Main battery charger current to 400mA ( 0x08-200mA,
                       // 0x09-300mA, 0x0A-400mA )
WriteReg(0x63, 0x01);  // set Main battery term charge current to 25mA
```

What that does, checked against `docs/datasheets/TG28-PMIC.pdf` and `power.md`:

| Write | Effect | Compared with ours (`pmic::configure()`) |
| --- | --- | --- |
| `0x22=0x06` | Bit 2: die over-temperature (level 2) shutdown on (POR default 1). Bit 1: PWRON held past OFFLEVEL powers off. Bit 0 = 0: that press means power-off, not restart (bits 1:0 default from eFuse) | We don't write it. The 4 s forced-off already works on our boards (power.md) |
| `0x27=0x10` | IRQLEVEL 1.5 s, OFFLEVEL `00` = 4 s, **ONLEVEL `00` = 128 ms** | Ours keeps ONLEVEL at 128 ms too and writes OFFLEVEL 4 s the same, so `0x27` ends at `0x10` under either |
| `0x80=0x01` | Only DCDC1 on (default DCDC1–4) | Unchanged in ours |
| `0x90=0x00` → `0x90=0x07`, `0x91=0x00` | All LDOs off, then ALDO1–3 on at 3.3 V (`0x92`–`0x94` = 28 → 0.5 V + 28×0.1 V). DLDO2 off | ALDO3 (panel) and ALDO1/2 (audio, mic) drop briefly at boot. Harmless before the panel is initialised |
| `0x82=18` | DC1 = 1.5 V + 18×0.1 V = 3.3 V | Default |
| `0x61`/`0x62`/`0x63`/`0x64` | 50 mA precharge, **200 mA** CC (`0x08`, the comment's "400mA" is wrong by its own table), 25 mA stop, 4.2 V | We charge at 500 mA |

The `Axp2101` helper turns on the die-temperature ADC (`0x30` bit 4, read-modify-write). It reads charge state from
`0x01[6:5]` (1 = charging, 2 = discharging), the gauge percentage from `0xA4`, and powers off with `0x10 |= 0x01`. Our
`pmic::powerOff()` uses the same bit (verified on hardware).

**Buttons.**

```cpp
boot_button_.OnClick([this]() {
    auto& app = Application::GetInstance();
    // During startup (before connected), pressing BOOT button enters Wi-Fi config mode
    // without reboot
    if (app.GetDeviceState() == kDeviceStateStarting) {
        EnterWifiConfigMode();
        return;
    }
    app.ToggleChatState();
});
pwr_button_.OnClick([this]() {                     //> Button(VBAT_PWR_GPIO, 1): GPIO1, active high
    GetDisplay()->SetChatMessage("system", "OFF");
    vTaskDelay(pdMS_TO_TICKS(1000));
    pmic_->PowerOff();
});
```

GPIO1 reads HIGH while PWRON is low, from the PWR key *or* an asserted RTC INT (board_pins.h). XiaoZhi never touches
the RTC, so a stale RTC alarm left by our firmware would read as a held PWR key (inferred).

**Power-save timer:** `PowerSaveTimer(-1, 100, 300)`, enabled only while `IsDischarging()`. -1 means no CPU-frequency
change. 100 s idle means "sleep mode", and 300 s calls `OnShutdownRequest`: show "OFF", wait 1 s, `PowerOff()`. No RTC
alarm is armed, so it stays off until PWR is pressed.

## 5. Codec: `Es8311AudioCodec`

Same `esp_codec_dev` ES8311 driver as Waveshare's factory app (audio.md §3), but at version **~1.6.2**
(`main/idf_component.yml`) and with a different lifecycle. The constructor creates the duplex channels first, then
the codec interfaces:

```cpp
duplex_ = true; // 是否双工                          //> "full duplex?"
input_reference_ = false; // 是否使用参考输入，实现回声消除   //> "use a reference input for echo cancel?"
input_channels_ = 1; // 输入通道数
input_gain_ = 30;
// ...
i2s_chan_config_t chan_cfg = {
    .id = I2S_NUM_0, .role = I2S_ROLE_MASTER,
    .dma_desc_num = AUDIO_CODEC_DMA_DESC_NUM,        //> 6
    .dma_frame_num = AUDIO_CODEC_DMA_FRAME_NUM,      //> 240 = 10 ms at 24 kHz
    .auto_clear_after_cb = true, .auto_clear_before_cb = false, .intr_priority = 0,
};
// std_cfg: .sample_rate_hz = output_sample_rate_, .mclk_multiple = I2S_MCLK_MULTIPLE_256,
//          16-bit data, I2S_SLOT_BIT_WIDTH_AUTO, I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH, Philips
ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle_, &std_cfg));
ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle_, &std_cfg));
ESP_ERROR_CHECK(i2s_channel_enable(tx_handle_));
ESP_ERROR_CHECK(i2s_channel_enable(rx_handle_));
```

```cpp
void Es8311AudioCodec::ResetCodec() {        //> called before es8311_codec_new()
    // Hold the ES8311 digital blocks in reset for several milliseconds, as
    // recommended by the initialization guide. Normal codec initialization
    // releases the reset and starts the state machine.
    uint8_t reset_value = 0x1F;
    ESP_ERROR_CHECK(static_cast<esp_err_t>(
        ctrl_if_->write_reg(ctrl_if_, 0x00, 1, &reset_value, 1)));
    vTaskDelay(pdMS_TO_TICKS(5));
}
```

`es8311_codec_cfg_t` is zero-initialised, then given `codec_mode = BOTH`, `pa_pin = GPIO39`, `use_mclk = true` and
`hw_gain` 5.0 V / 3.3 V. That leaves `no_dac_ref = false` (REG44 = ADC + DAC loopback, audio.md §3), so the mic is on
the left slot. The mono `fs` below reads slot 0 only.

**Open/close and the amp.** The `esp_codec_dev` device exists only while input or output is enabled:

```cpp
void Es8311AudioCodec::UpdateDeviceState() {
    if ((input_enabled_ || output_enabled_) && dev_ == nullptr) {
        esp_codec_dev_cfg_t dev_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT, .codec_if = codec_if_, .data_if = data_if_, };
        dev_ = esp_codec_dev_new(&dev_cfg);
        esp_codec_dev_sample_info_t fs = { .bits_per_sample = 16, .channel = 1, .channel_mask = 0,
                                           .sample_rate = (uint32_t)input_sample_rate_, .mclk_multiple = 0, };
        ESP_ERROR_CHECK(esp_codec_dev_open(dev_, &fs));
        ESP_ERROR_CHECK(esp_codec_dev_set_in_gain(dev_, input_gain_));
        ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(dev_, output_volume_));
    } else if (!input_enabled_ && !output_enabled_ && dev_ != nullptr) {
        ESP_ERROR_CHECK(esp_codec_dev_close(dev_));
        esp_codec_dev_delete(dev_);
        dev_ = nullptr;
    }
    if (pa_pin_ != GPIO_NUM_NC) {
        int level = output_enabled_ ? 1 : 0;
        gpio_set_level(pa_pin_, pa_inverted_ ? !level : level);
    }
}
```

So the amp is forced LOW whenever output is disabled, including while only the mic runs. `AudioService` enables
output when a decoded frame reaches the output task, and its 1 s power timer disables input or output after
`AUDIO_POWER_TIMEOUT_MS` (15 s) without traffic. Output stays on while the mic runs: "Keep TX clock when duplex RX
is active; otherwise RX may stall on some boards". This is the per-use amp switching AGENTS.md says to test. XiaoZhi
does it in production with a 15 s tail, so pops would show up at most once per conversation (not verified by us).

Other details: `Read`/`Write` call `esp_codec_dev_read`/`_write` (blocking DMA). Volume defaults to 70 and persists
in NVS (`audio`/`output_volume`). A stored value ≤ 0 becomes 10 at start. Input gain is a plain `esp_codec_dev_set_in_gain(30)`.
I haven't re-checked how esp_codec_dev 1.6.x maps 30 dB onto REG14/REG16. In 1.3.6 it would be REG16 = 5 (audio.md §4).

## 6. Audio pipeline: tasks, queues, Opus

The header comment is the best summary:

```cpp
/*
 * There are two types of audio data flow:
 * 1. (MIC) -> [Audio Engine] -> {Encode Queue} -> [Opus Encoder] -> {Send Queue} -> (Server)
 * 2. (Server) -> {Decode Queue} -> [Opus Decoder] -> {Playback Queue} -> (Speaker)
 */
#define OPUS_FRAME_DURATION_MS 60
#define MAX_ENCODE_TASKS_IN_QUEUE 2
#define MAX_PLAYBACK_TASKS_IN_QUEUE 2
#define MAX_SEND_PACKETS_IN_QUEUE (2400 / OPUS_FRAME_DURATION_MS)   //> 40 packets = 2.4 s
#define AUDIO_POWER_TIMEOUT_MS 15000
//> AS_OPUS_ENC_CONFIG(): 16 kHz, mono, 16-bit, ESP_OPUS_BITRATE_AUTO, 60 ms,
//> ESP_OPUS_ENC_APPLICATION_AUDIO, complexity 0, FEC off, DTX on, VBR on
```

| Task | Stack | Prio | Job |
| --- | --- | --- | --- |
| `audio_input` | 6 KB (AFE builds), pinned to core 0 | 8 | Waits on event bits. Reads **10 ms** chunks (160 samples at 16 kHz, so 240 at 24 kHz before resampling) and `Feed()`s the AFE |
| `audio_afe` | 4 KB, **stack in PSRAM** (`xTaskCreateStatic`, TCB internal) | 3 | `fetch_with_delay` → VAD edges + re-frames output into 960-sample (60 ms) frames → encode queue |
| `opus_codec` | 24 KB | 2 | Decodes first (if the playback queue has room), then encodes. A full send queue drops its oldest packet ("stale realtime audio is useless") |
| `audio_output` | 4 KB | 4 | Pops PCM, enables output if needed, `codec_->OutputData()` |

Resampling uses `esp_ae_rate_cvt` (`espressif/esp_audio_effects`, complexity 2, speed mode). The uplink resampler
exists only because the codec isn't at 16 kHz. Opus encode/decode is `espressif/esp_audio_codec` (`esp_opus_enc_*`,
`esp_opus_dec_*`). On a server hello with a different rate or frame length, `SetDecodeSampleRate` re-opens the
decoder and adds an output resampler. When voice processing starts, the input task waits 120 ms once
(`audio_input_need_warmup_`) before reading.

## 7. AFE: wake word and VAD

`AfeAudioEngine::Initialize()` (ESP-SR, pinned `==2.4.7`):

```cpp
models_ = esp_srmodel_init("model");                  //> the "model" flash partition
// wakenet_model_name = esp_srmodel_filter(models_, ESP_WN_PREFIX, nullptr); MultiNet present -> custom wake word
int ref_num = codec_->input_reference() ? 1 : 0;     //> 0 on this board, so input_format = "M"
afe_config_t* afe_config =
    afe_config_init(input_format.c_str(), models_, AFE_TYPE_FD, AFE_MODE_LOW_COST);
afe_config->aec_init = codec_->input_reference();     //> false
afe_config->aec_mode = AEC_MODE_FD_LOW_COST;
afe_config->aec_nlp_level = AEC_NLP_LEVEL_VERYAGGR;
afe_config->ns_init = false;
afe_config->vad_init = kUseAfeForVoiceProcessing;     //> CONFIG_USE_AUDIO_PROCESSOR, default y on S3 + PSRAM
afe_config->vad_mode = VAD_MODE_0;
afe_config->vad_min_noise_ms = 100;
afe_config->wakenet_init = wake_detector_ == WakeDetector::kWakeNet;
// ... up to two WakeNet models from the partition
afe_config->agc_init = false;
afe_config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
```

The fetch side reports `VAD_SPEECH`/`VAD_SILENCE` edges (`on_vad_change`) and passes processed audio on in 60 ms
frames. WakeNet is created disabled and toggled per device state (§8). AGC and noise suppression are off ("this
project does not provide an NSNet model"). The log prints internal-heap free/min/largest before and after AFE
creation, which tells you how tight internal RAM is.

## 8. Device states and listening modes

`GetDefaultListeningMode()` returns `kListeningModeAutoStop` when AEC is off, as here, and `kListeningModeRealtime`
otherwise.

| State | Audio | Notes |
| --- | --- | --- |
| Starting / WifiConfiguring | Wake word off | BOOT click during Starting → Wi-Fi hotspot mode |
| Activating | – | OTA check, 6-digit code spoken/shown until bound on xiaozhi.me |
| Idle | Voice processing off, **wake word on** | BOOT click or wake word → Connecting |
| Connecting | – | Opens the audio channel (WebSocket or UDP), sends `listen start` |
| Listening | Voice processing on, 60 ms Opus frames streamed | Auto mode waits for playback to drain before starting the mic |
| Speaking | Voice processing off. Wake word stays on (AFE WakeNet), so you can interrupt | `tts stop` → Listening (auto mode) or Idle (manual) |

The wake word fires `listen` / `detect` and, if the model build sets `CONFIG_SEND_WAKE_WORD_DATA`, uploads the cached
wake-word audio (a 2 s, 16 kHz cache in PSRAM), presumably so the server can check it (inferred).

## 9. Server protocol

**Discovery (OTA check).** At boot the device calls `CONFIG_OTA_URL` (GET, or POST with a JSON body describing
the device) with headers `Activation-Version`, `Device-Id` (MAC), `Client-Id` (UUID), optional `Serial-Number`,
`User-Agent`, `Accept-Language`. The response can carry `activation` (`message`, `code`, optional `challenge`),
`mqtt` or `websocket` settings (stored in NVS namespaces of the same names), `server_time`, and `firmware`
(`version`, `url`) for OTA. With a serial number provisioned, activation POSTs `{"algorithm":"hmac-sha256",
"challenge", "hmac"}` to `<ota>/activate`, using the eFuse `HMAC_KEY0`. Without one, the user enters the spoken code.

**WebSocket.** Connect to the stored `url` with:

```cpp
if (!token.empty()) {
    // If token not has a space, add "Bearer " prefix
    if (token.find(" ") == std::string::npos) {
        token = "Bearer " + token;
    }
    websocket_->SetHeader("Authorization", token.c_str());
}
websocket_->SetHeader("Protocol-Version", std::to_string(version_).c_str());   //> default 1
websocket_->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
websocket_->SetHeader("Client-Id", Board::GetInstance().GetUuid().c_str());
```

then send the client hello and wait for the server's:

```cpp
cJSON_AddStringToObject(root, "type", "hello");
cJSON_AddNumberToObject(root, "version", version_);
//> + device capabilities (Protocol::AddClientCapabilities)
cJSON_AddStringToObject(root, "transport", "websocket");
cJSON_AddStringToObject(audio_params, "format", "opus");
cJSON_AddNumberToObject(audio_params, "sample_rate", 16000);
cJSON_AddNumberToObject(audio_params, "channels", 1);
cJSON_AddNumberToObject(audio_params, "frame_duration", OPUS_FRAME_DURATION_MS);
//> server hello must say "transport":"websocket"; may give "session_id" and
//> "audio_params": {"sample_rate", "frame_duration"} for the downlink
```

**Binary frames** carry one Opus packet each. Protocol version 1 is the raw packet. Version 2 prefixes `BinaryProtocol2`
(`uint16 version, uint16 type (0 = Opus, 1 = JSON), uint32 reserved, uint32 timestamp, uint32 payload_size`, big-endian,
packed, 16 bytes; the timestamp is for server-side AEC). Version 3 prefixes `BinaryProtocol3` (`uint8 type, uint8 reserved,
uint16 payload_size`, 4 bytes).

**JSON text frames**, all with `"session_id"`:

| Direction | `type` | Fields / meaning |
| --- | --- | --- |
| → server | `listen` | `"state":"start"` + `"mode":"auto"\|"manual"\|"realtime"`, `"state":"stop"`, `"state":"detect"` + `"text":<wake word>` |
| → server | `abort` | Stop TTS. Optional `"reason":"wake_word_detected"` |
| → server | `mcp` | `"payload"`: JSON-RPC for device tools (MCP). This board registers `self.disp.network` (re-enter Wi-Fi setup) |
| ← device | `tts` | `"state":"start"\|"stop"\|"sentence_start"` (+ `"text"` subtitle) |
| ← device | `stt` | `"text"`: what the server heard |
| ← device | `llm` | `"emotion"`: drives the face/emoji UI |
| ← device | `mcp` | JSON-RPC from the server: tool calls into the device |
| ← device | `system` | `"command":"reboot"` (the only one handled) |
| ← device | `alert` | `"status"`, `"message"`, `"emotion"`: shown with a vibration sound |
| ← device | `notify`, `custom` | A notification with an audio URL and subtitles. Custom payloads only with `CONFIG_RECEIVE_CUSTOM_MESSAGE` |

The default server (xiaozhi.me / `api.tenclass.net`) needs an account. The protocol is documented in the repo's
`docs/`, and community servers implement it, for example
[xinnan-tech/xiaozhi-esp32-server](https://github.com/xinnan-tech/xiaozhi-esp32-server) (MIT). Point a build at your own
server by changing `CONFIG_OTA_URL`, or by having the OTA response return your `websocket.url` and `token`.

## 10. E-paper UI

`CustomEpdDisplay` puts the XiaoZhi LVGL 9 chat UI on the panel:

```cpp
void CustomEpdDisplay::lvgl_flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* color_p) {
    CustomEpdDisplay* driver = (CustomEpdDisplay*)lv_display_get_user_data(disp);
    uint16_t* buffer = (uint16_t*)color_p;
    driver->EPD_Clear();
    for (int y = area->y1; y <= area->y2; y++) {
        for (int x = area->x1; x <= area->x2; x++) {
            uint8_t color = (*buffer < 0x7fff) ? DRIVER_COLOR_BLACK : DRIVER_COLOR_WHITE;
            driver->EPD_DrawColorPixel(x, y, color);
            buffer++;
        }
    }
    driver->EPD_DisplayPart();          //> 0x24 <- all 48000 bytes, then 0x22 = 0xFF, 0x20, wait BUSY
    lv_disp_flush_ready(disp);
}
```

- LVGL renders full frames in RGB565 (`LV_DISPLAY_RENDER_MODE_FULL`, 768,000 bytes in PSRAM), plus the 48,000-byte
  1-bpp panel buffer in PSRAM. `lvgl_port` task priority 2, 50 ms timer.
- Every LVGL change sends the whole frame with the partial-update trigger `0x22 = 0xFF` (the value Waveshare's
  `EPD_Display_Partial` uses, epaper.md §3.3), but with no reset, window or `0x26` write in between. There's one
  full refresh at boot (`EPD_Init` + `EPD_Clear` + `EPD_Display`, `0x22 = 0xF7`) and **none after that**.
- `EPD_Init_Partial`, `EPD_DisplayPartBaseImage` and `EPD_Sleep` are defined but never called. The panel never
  deep-sleeps, and ghosting builds up for as long as the device runs (inferred from the code, not observed).
- The threshold `< 0x7fff` keys on the RGB565 red channel's top bit, so anything darker than mid-red goes black.

## 11. Porting notes and a standalone sample

**What the Arduino core already has** (pioarduino `framework-arduinoespressif32-esp32s3-libs`, checked):

| Need | In our core? | Notes |
| --- | --- | --- |
| ESP-SR (AFE, WakeNet, VAD, MultiNet) | **Yes**, `espressif__esp-sr` 2.5.3 + the `ESP_SR` Arduino library | `esp_sr/srmodels.bin` (3.3 MB) is built for `CONFIG_SR_WN_WN9_HIESP` ("Hi ESP") and English MultiNet 7. `ESP_SR` uses `AFE_TYPE_SR` with commands. For XiaoZhi-style VAD + uplink, call `afe_config_init(..., AFE_TYPE_FD, ...)` directly as §7 does (`AFE_TYPE_FD` and `_VC` both exist in 2.5.3's `esp_afe_config.h`) |
| `model` partition | **No.** Ours is `app3M_fat9M_16MB.csv` | The core's `esp_sr/partitions.csv` puts `model` (3.9 MB) at `0xC10000` by shrinking the data partition to 6 MB. PlatformIO doesn't flash `srmodels.bin` (only the Arduino IDE's `platform.txt` hook copies it), so write it with esptool at the `model` offset (inferred) |
| Opus encode/decode | No | `esp_audio_codec` is an IDF managed component (binary libs). In a plain Arduino PlatformIO env, use libopus (BSD-3) built as a library instead (inferred) |
| Resampler | No | Avoidable. An Opus decoder can output any of 8/12/16/24/48 kHz whatever the encoder's input rate (RFC 6716), so encode the mic at 24 kHz and the server still decodes the 16 kHz it asked for (inferred) |
| WebSocket client | No | `esp_websocket_client` is a managed component. Arduino libraries exist (e.g. links2004 `WebSockets`). TLS goes through our `net::begin()` PSRAM buffers |
| cJSON | Yes, `espressif__cjson` | |

**What our code has and still needs** (updated 2026-11-13, when the Recorder was built):

- The mic works: `es8311::micBegin()` writes REG14 = 0x1A (Mic1p-Mic1n, PGA 30 dB), REG17 = 0xBF (ADC 0 dB) and
  REG16 for 0–42 dB digital gain; REG44 = 0x08 gives ADC on both slots. `audio::record()` captures 16 kHz mono from
  an RX-only I2S channel into a WAV (verified on hardware; audio.md §9 has the slot-mask trap).
- Still half the picture for voice: playback (TX) and recording (RX) each create their own simplex channel and take
  turns. Full-duplex voice needs one TX+RX pair at one rate (`i2s_new_channel(&chan, &tx, &rx)`). Pick 24 kHz like
  XiaoZhi.
- Keep XiaoZhi's memory split: AFE stack and buffers in PSRAM (`AFE_MEMORY_ALLOC_MORE_PSRAM`), TCB and DMA internal.
  XiaoZhi reserves 100 KB of internal RAM (`CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=102400`). We already have Wi-Fi + TLS
  in the ~230 KB, so measure with dev key `h` before adding WakeNet.
- Don't copy the PMIC block: it fights `pmic::configure()` (200 vs 500 mA, all LDOs off then ALDO1-3 on) and new TG28 writes need
  the owner's approval anyway.
- Without AEC, stay half-duplex: mute the uplink while TTS plays, or the server hears its own voice. XiaoZhi keeps AFE
  WakeNet running during playback so the wake word can interrupt (§8). With no echo reference, how well it hears you
  over the speaker is untested.

**Standalone sample: one push-to-talk turn against a XiaoZhi-protocol server.** No wake word, no AFE, protocol v1.
This is a sketch written for our stack from the pieces above. **It has not been compiled or run.** `ws` stands for
whichever WebSocket client you pick, and `opus_*` is the libopus API.

```cpp
//> Sketch, not compiled. Fill in ws (WebSocket lib) and the server URL/token.
#include <driver/i2s_std.h>
#include <opus.h>
#include "board_pins.h"
#include "es8311.h"

constexpr int kRate = 24000, kFrameMs = 60, kFrame = kRate * kFrameMs / 1000;  // 1440 samples
i2s_chan_handle_t tx, rx;
OpusEncoder *enc;  OpusDecoder *dec;
std::string sessionId;

void voiceBegin() {
  // 1. One duplex pair, XiaoZhi's CreateDuplexChannels settings, our pins.
  i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  cc.dma_desc_num = 6; cc.dma_frame_num = 240;
  i2s_new_channel(&cc, &tx, &rx);
  i2s_std_config_t sc = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kRate),                       // 256 x fs
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
      .gpio_cfg = {.mclk = (gpio_num_t)board::I2S_MCLK, .bclk = (gpio_num_t)board::I2S_BCLK,
                   .ws = (gpio_num_t)board::I2S_WS, .dout = (gpio_num_t)board::I2S_DOUT,
                   .din = (gpio_num_t)board::I2S_DIN}};
  i2s_channel_init_std_mode(tx, &sc);  i2s_channel_init_std_mode(rx, &sc);
  i2s_channel_enable(tx);  i2s_channel_enable(rx);                      // MCLK runs before codec init
  es8311::begin();
  es8311::micBegin(12);                                                 // PGA 30 dB + 12 dB digital
  // 2. Opus: encode the 24 kHz mic directly (no resampler), decode at the codec rate.
  int err;
  enc = opus_encoder_create(kRate, 1, OPUS_APPLICATION_AUDIO, &err);    // XiaoZhi's mode; VOIP may suit speech
  opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(0));  opus_encoder_ctl(enc, OPUS_SET_DTX(1));
  dec = opus_decoder_create(kRate, 1, &err);
  // 3. ws.setHeader("Authorization", "Bearer <token>"); "Protocol-Version: 1"; "Device-Id"; "Client-Id"
  //    ws.connect(url); ws.sendText(R"({"type":"hello","version":1,"transport":"websocket",
  //      "audio_params":{"format":"opus","sample_rate":16000,"channels":1,"frame_duration":60}})");
  //    wait for {"type":"hello",...}: keep session_id; check audio_params.sample_rate (expect 24000)
}

void talk(bool (*keyHeld)()) {                                          // call on key-down
  ws.sendText(R"({"session_id":")" + sessionId + R"(","type":"listen","state":"start","mode":"manual"})");
  static int16_t st[kFrame * 2], mono[kFrame];
  static uint8_t pkt[1500];
  while (keyHeld()) {
    size_t got;
    i2s_channel_read(rx, st, sizeof st, &got, portMAX_DELAY);           // 60 ms, stereo slots
    for (int i = 0; i < kFrame; i++) mono[i] = st[2 * i];               // left slot = ADC
    int n = opus_encode(enc, mono, kFrame, pkt, sizeof pkt);
    if (n > 0) ws.sendBinary(pkt, n);                                   // send DTX packets too, as XiaoZhi does
  }
  ws.sendText(R"({"session_id":")" + sessionId + R"(","type":"listen","state":"stop"})");
}

// 4. WebSocket callbacks.
void onText(const char *json) {
  //> {"type":"stt","text":...}            -> show what was heard
  //> {"type":"tts","state":"start"}       -> digitalWrite(board::AMP_EN, HIGH); es8311::mute(false)
  //> {"type":"tts","state":"sentence_start","text":...} -> subtitle (partial refresh, see epaper.md)
  //> {"type":"tts","state":"stop"}        -> after the DMA drains: mute, AMP_EN LOW (audio.md §9 order)
}
void onBinary(const uint8_t *data, size_t len) {
  static int16_t pcm[kFrame], st[kFrame * 2];
  int n = opus_decode(dec, data, len, pcm, kFrame, 0);                  // 60 ms at 24 kHz
  for (int i = 0; i < n; i++) st[2 * i] = st[2 * i + 1] = pcm[i];       // DAC hears the left slot
  size_t w;
  i2s_channel_write(tx, st, n * 4, &w, portMAX_DELAY);                  // runs on the WS task: queue it in real code
}
```

Next steps beyond the sketch, roughly in XiaoZhi's order: move encode and decode to their own task with
queues (§6), add the OTA-style discovery or a fixed URL in `settings`, and add ESP-SR VAD so `"mode":"auto"`
works without holding a key. WakeNet comes last, once the `model` partition and internal-RAM budget are settled.

## 12. Licensing

- **xiaozhi-esp32**: MIT, "Copyright (c) 2025 Shenzhen Xinzhi Future Technology Co., Ltd." (repo `LICENSE`). Board
  files carry no separate header. Copying with attribution is fine. Still, re-implement against our modules rather
  than lifting whole files, which depend on XiaoZhi's `Board`/`Application` classes anyway.
- **ESP-SR** (models, AFE): Espressif's own license (GitHub reports "Other"). Read its `LICENSE` before shipping
  `srmodels.bin` outside a device.
- `esp_codec_dev`: Apache-2.0. `esp_audio_codec` / `esp_audio_effects`: Espressif components shipped as binaries; check
  each component's license file. libopus: BSD-3-Clause.
- Waveshare's `Firmware/xiaozhi/*.bin`: binaries in a repo with no license.
