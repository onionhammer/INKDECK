# E-paper: SSD1677 driver, refresh modes, framebuffer, GUI_Paint, fonts

Upstream commit `9b12d40731a80213b927ee8a421cae4082952819`. Panel datasheet: `docs/datasheets/3.97inch-e-Paper-panel.pdf`
("panel PDF"; its command table on pp. 14-17 and reference program on p. 21 are images).
Anything marked **(inferred)** is our reading and hasn't been confirmed by source or datasheet. `//>` comments inside snippets are ours.

## Contents
1. [Upstream file map](#1-upstream-file-map)
2. [Hardware and SPI setup](#2-hardware-and-spi-setup)
3. [SSD1677 command sequences](#3-ssd1677-command-sequences) (init variants, update triggers, display routines, partial, 4-grey, clear, sleep, command table)
4. [Framebuffer format](#4-framebuffer-format)
5. [GUI_Paint, fonts, BMP](#5-gui_paint-fonts-bmp)
6. [How the factory app uses the driver](#6-how-the-factory-app-uses-the-driver)
7. [Porting notes](#7-porting-notes)
8. [License headers](#8-license-headers)

## 1. Upstream file map

Base: `https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/`. `08/` = `ESP-IDF/08_ESP32-S3_e-Paper-3.97/`.

| File | What | Key ranges |
| --- | --- | --- |
| `08/components/epaper_port/epaper_port.c` | SSD1677 driver (IDF `spi_master`) | [SPI/GPIO init L15-L62](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/components/epaper_port/epaper_port.c#L15-L62), [byte/buffer I/O, reset, busy L64-L176](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/components/epaper_port/epaper_port.c#L64-L176), [ALDO3 power + update triggers L182-L228](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/components/epaper_port/epaper_port.c#L182-L228), [Init/Init_Fast/Init_4GRAY L234-L399](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/components/epaper_port/epaper_port.c#L234-L399), [Clear + Display* L404-L543](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/components/epaper_port/epaper_port.c#L404-L543), [Partial L549-L601](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/components/epaper_port/epaper_port.c#L549-L601), [4Gray L603-L686](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/components/epaper_port/epaper_port.c#L603-L686), [Sleep L692-L702](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/components/epaper_port/epaper_port.c#L692-L702) |
| `08/components/epaper_port/epaper_port.h` | Pins, sizes (`EPD_SIZE_MONO 48000`, `EPD_SIZE_4GRAY 96000`), `Partial_refresh 0` / `Global_refresh 1`, API | [L12-L65](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/components/epaper_port/epaper_port.h#L12-L65) |
| `08/components/epaper_port/epaper_bsp.c` | Demo helpers `EPD_display_BMP` (draws a test pattern, ignores `path`), `EPD_display`, `EPD_display_Clear` | [L18-L95](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/components/epaper_port/epaper_bsp.c#L18-L95) |
| `08/components/epaper_lib/GUI_Paint.c/.h` | Drawing into the framebuffer (global `PAINT Paint`) | [SetPixel L115-L185](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/components/epaper_lib/GUI_Paint.c#L115-L185), [DrawString_CN L543-L625](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/components/epaper_lib/GUI_Paint.c#L543-L625), [colours .h L48-L60](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/components/epaper_lib/GUI_Paint.h#L48-L60) |
| `08/components/epaper_lib/GUI_BMPfile.c` | `GUI_ReadBmp` (1 bpp) / `GUI_ReadBmp_4Gray` (4 bpp) from SD | [L20-L181](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/components/epaper_lib/GUI_BMPfile.c#L20-L181) |
| `08/components/epaper_lib/Fonts/font.h`, `font.c`, `fonts.c` | `cFONT`/`sFONT`, glyph sizes, SD paths; glyph lookup; UTF-8→GB2312 table | [font.h L16-L38](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/components/epaper_lib/Fonts/font.h#L16-L38), [sizes L207-L256](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/components/epaper_lib/Fonts/font.h#L207-L256), [Get_Char_Font_Data L466-L662](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/components/epaper_lib/Fonts/font.c#L466-L662), [utf8_char_to_gb2312 L399](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/components/epaper_lib/Fonts/fonts.c#L399) |
| `08/main/Kconfig.projbuild` | `CONFIG_FONTnn_EMBEDDED`, SD-font fallback, `CONFIG_IMG_SOURCE_*` | [L22-L124](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/main/Kconfig.projbuild#L22-L124) |
| `ESP-IDF/01_E-Paper_Example/main/main.c` | Minimal demo of every mode | [L11-L141](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/01_E-Paper_Example/main/main.c#L11-L141) |
| `Arduino/examples/02_E-Paper_Example/EPD_3in97.cpp` | Same driver for Arduino; bit-banged SPI | [Reset/IO/busy L38-L86](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/Arduino/examples/02_E-Paper_Example/EPD_3in97.cpp#L38-L86), [Window L423-L478](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/Arduino/examples/02_E-Paper_Example/EPD_3in97.cpp#L423-L478), [Partial L484-L539](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/Arduino/examples/02_E-Paper_Example/EPD_3in97.cpp#L484-L539), [Sleep L734-L739](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/Arduino/examples/02_E-Paper_Example/EPD_3in97.cpp#L734-L739) |
| `Arduino/examples/02_E-Paper_Example/DEV_Config.cpp` | GPIO setup and bit-bang `DEV_SPI_WriteByte` | [L32-L103](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/Arduino/examples/02_E-Paper_Example/DEV_Config.cpp#L32-L103) |
| `08/main/main.cc` | Boot init, home menu refresh policy, idle sleep | [esp_home L125-L205](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/main/main.cc#L125-L205), [user_Task L285-L418](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/main/main.cc#L285-L418), [app_main EPD init L490-L502](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/main/main.cc#L490-L502) |
| `08/main/page_clock/page_clock.cc` | Minute updates, hourly full refresh, RTC-wake clock mode | [page_clock_show L342-L451](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/main/page_clock/page_clock.cc#L342-L451), [show_mode L454-L506](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/main/page_clock/page_clock.cc#L454-L506), [display_clock_mode_img/display_clock_img L1085-L1218](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/main/page_clock/page_clock.cc#L1085-L1218) |

`ESP-IDF/01_E-Paper_Example/components/epaper_port/epaper_port.c` is the same driver minus PMIC control: no `EPD_Power_ON/OFF`, and `EPD_Clear` sends byte by byte with a 1 ms delay per row.

## 2. Hardware and SPI setup

Pins (`epaper_port.h` L12-L20, Arduino `DEV_Config.h` L49-L54): SCLK 11, MOSI 12, CS 10, DC 9, RST 46, BUSY 3. All match `board::EPD_*`.

```c
// epaper_port_init()  epaper_port.c L36-L62
spi_bus_config_t buscfg = { .miso_io_num = -1, .mosi_io_num = EPD_MOSI_PIN, .sclk_io_num = EPD_SCLK_PIN,
                            .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = 65536, };
spi_device_interface_config_t devcfg = { .spics_io_num = -1, .clock_speed_hz = 20 * 1000 * 1000,
                                         .mode = 0, .queue_size = 1, };
ret = spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO);
ret = spi_bus_add_device(SPI3_HOST, &devcfg, &spi);
epaper_gpio_Init();   //> L15-L34: RST|DC|CS = GPIO_MODE_OUTPUT (pull-up on); BUSY = input with:
  gpio_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
  gpio_conf.pull_up_en = GPIO_PULLDOWN_ENABLE;      //> sic: value 1, so BUSY gets the PULL-UP
  // ...
  epaper_rst_1;

static void EPD_Reset(void)          // L103-L111. Arduino EPD_3IN97_Reset: 20 / 2 / 20 ms
{ epaper_rst_1; vTaskDelay(pdMS_TO_TICKS(50)); epaper_rst_0; vTaskDelay(pdMS_TO_TICKS(2));
  epaper_rst_1; vTaskDelay(pdMS_TO_TICKS(50)); }
static void EPD_SendCommand(UBYTE Reg) { epaper_dc_0; spi_send_byte(Reg); }   //> 1 polling transaction per byte
static void EPD_SendData(UBYTE Data)   { epaper_dc_1; spi_send_byte(Data); }
//> EPD_SendDataBuffer() L135-L159: epaper_dc_1, then spi_device_polling_transmit() in 4096-byte chunks
static void EPD_ReadBusy(void)       // L165-L176. Arduino: 100 ms, then poll every 10 ms
{ vTaskDelay(pdMS_TO_TICKS(100));
  while(1) { if(!ReadBusy){break;} vTaskDelay(pdMS_TO_TICKS(20)); } }   //> no timeout
```

- **SPI**: hardware SPI3 (`SPI3_HOST`), 20 MHz, mode 0, DMA auto, polling transmits, MSB first. Panel PDF: write-mode SCL max 20 MHz (TSCLCYC ≥ 50 ns), SDA latched on SCL rising edge, D7 first, CS# high ≥ 100 ns between transfers. 20 MHz is the datasheet maximum.
- **CS**: `spics_io_num = -1`, so the SPI driver never drives GPIO10. It's configured as a plain output and never set high. `EPD_Sleep` writes it low. So CS sits at the GPIO's default output level, low, and stays asserted (inferred). The Arduino driver toggles CS around every byte.
- **DC**: set by GPIO before each command byte (0) or data byte/buffer (1).
- **BUSY**: HIGH = busy (panel PDF note 5-4; Arduino comment `//LOW: idle, HIGH: busy`). It's also high throughout deep sleep.
- **Reset**: panel PDF reference program: VCI on, 10 ms, RES# low 200 µs, high, 200 µs, then wait for BUSY low. Waveshare uses much longer delays (50/2/50 ms).
- **Panel power** (08 only): `EPD_Power_ON()` = `enapwrstate(ALDO3)`, `EPD_Power_OFF()` = `disapwrstate(ALDO3)` (XPowersLib `enableALDO3()`/`disableALDO3()`, `axpPower/axp_prot.cpp` L197-L233). The 01 example and the Arduino example never touch the PMIC; they rely on ALDO3 being on at power-up.
- **Arduino `DEV_Config.cpp`**: the `SPI.*` calls are commented out. `DEV_SPI_WriteByte` bit-bangs MOSI/SCK with `digitalWrite`, MSB first, and pulls CS low/high around every byte. `GPIO_Config` calls `pinMode(EPD_PWR_PIN = -1, OUTPUT)`.

## 3. SSD1677 command sequences

### 3.1 Init variants
`EPD_Init()` ([L234-L285](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/components/epaper_port/epaper_port.c#L234-L285)). Statements are verbatim; whitespace is compressed. Arduino `EPD_3IN97_Init` is identical apart from the power-on lines.
```c
EPD_Power_ON();
vTaskDelay(pdMS_TO_TICKS(10));
EPD_Reset();
EPD_ReadBusy();
EPD_SendCommand(0x12);  //SWRESET
EPD_ReadBusy();
EPD_SendCommand(0x18); EPD_SendData(0x80);
EPD_SendCommand(0x0C); EPD_SendData(0xAE); EPD_SendData(0xC7); EPD_SendData(0xC3); EPD_SendData(0xC0); EPD_SendData(0x80);
EPD_SendCommand(0x01); //Driver output control
EPD_SendData((EPD_HEIGHT-1)%256); EPD_SendData((EPD_HEIGHT-1)/256); EPD_SendData(0x02);      //> DF 01 02
EPD_SendCommand(0x3C); //BorderWavefrom
EPD_SendData(0x01);
EPD_SendCommand(0x11); //data entry mode
EPD_SendData(0x01);
EPD_SendCommand(0x44); //set Ram-X address start/end position
EPD_SendData(0x00); EPD_SendData(0x00); EPD_SendData((EPD_WIDTH-1)%256); EPD_SendData((EPD_WIDTH-1)/256);    //> 00 00 1F 03
EPD_SendCommand(0x45); //set Ram-Y address start/end position
EPD_SendData((EPD_HEIGHT-1)%256); EPD_SendData((EPD_HEIGHT-1)/256); EPD_SendData(0x00); EPD_SendData(0x00);  //> DF 01 00 00
EPD_SendCommand(0x4E);   // set RAM x address count to 0;
EPD_SendData(0x00); EPD_SendData(0x00);
EPD_SendCommand(0x4F);   // set RAM y address count to 0X199;
EPD_SendData(0x00); EPD_SendData(0x00);                //> datasheet reference program sends DF 01 here
EPD_ReadBusy();
```
`EPD_Init_Fast()` ([L287-L342](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/components/epaper_port/epaper_port.c#L287-L342)) and `EPD_Init_4GRAY()` ([L344-L399](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/components/epaper_port/epaper_port.c#L344-L399)) use the same bytes in a different order, plus a forced temperature:
```c
EPD_Power_ON();
vTaskDelay(pdMS_TO_TICKS(500));
EPD_Reset(); EPD_ReadBusy(); EPD_SendCommand(0x12); /*SWRESET*/ EPD_ReadBusy();
// ... 0x0C AE C7 C3 C0 80 | 0x01 DF 01 02 | 0x11 01 | 0x44 00 00 1F 03 | 0x45 DF 01 00 00 | 0x4E 00 00 | 0x4F 00 00 | EPD_ReadBusy()
EPD_SendCommand(0x3C); //BorderWavefrom
EPD_SendData(0x01);
EPD_SendCommand(0x18);
EPD_SendData(0x80);
//Fast(1.5s)                      //> Init_4GRAY: comment "//4 Gray"
EPD_SendCommand(0x1A);
EPD_SendData(0x6A);               //> Init_4GRAY: EPD_SendData(0x5A);
```
`EPD_Init_Partial()` / `EPD_3IN97_Init_Partial()` are declared in both headers but **defined nowhere**. Partial mode re-programs itself inside `EPD_Display_Partial` (§3.4).

### 3.2 Update triggers (L198-L228; identical in Arduino)
```c
EPD_SendCommand(0x22);
EPD_SendData(0xF7);      //> EPD_TurnOnDisplay. _Fast and _4GRAY: 0xD7. _Part: 0xFF
EPD_SendCommand(0x20);
EPD_ReadBusy();
```

### 3.3 Display routines (all assume the full-screen window and counters from Init)
| Function (IDF, `epaper_port.c`) | RAM written | 0x22 | Notes |
| --- | --- | --- | --- |
| `EPD_Display` L454 | 0x24 ← 48000 B | F7 | Full refresh. RED RAM untouched |
| `EPD_Display_Base` L467 | 0x24 ← img, then 0x26 ← img | F7 | Full refresh that also sets the "old" image for later partials. **The app's "Global_refresh"** |
| `EPD_Display_Fast` L485 / `_Fast_Base` L498 | 0x24 / 0x24+0x26 | D7 | Needs `EPD_Init_Fast` (0x1A=0x6A) |
| `EPD_Display_OneShot` L514 | 0x24 in one 48000-byte transaction | F7 | Unused |
| `EPD_Clear` L404 | 0x24, 0x26 ← 0xFF (temp `malloc(48000)`) | F7 | White |
| `EPD_Clear_Black` L428 | 0x24, 0x26 ← 0x00, 96000 single-byte sends | F7 | |
| `EPD_Display_Partial` L549 | HW reset, window, 0x24 ← sub-image | FF | §3.4. **The app's "Partial_refresh"** |
| `EPD_Display_4Gray` L603 | 0x24 ← plane A, 0x26 ← plane B (byte by byte) | D7 | Needs `EPD_Init_4GRAY` (0x1A=0x5A), §3.5 |
| Arduino `EPD_3IN97_V2_Display_Window(_Base)` L423/L445 | Full frame 0x24 (+0x26): image at x/y, 0xFF elsewhere | F7 | Arduino only |

No routine re-sets 0x4E/0x4F before writing (the datasheet reference program does). A full frame of 480 rows brings the counter back to its start, so 0x24 followed by 0x26 lines up (inferred).

### 3.4 Partial refresh ([L549-L601](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/components/epaper_port/epaper_port.c#L549-L601))
```c
void EPD_Display_Partial(const UBYTE *Image, UWORD Xstart, UWORD Ystart, UWORD Xend, UWORD Yend)
{                                   //> Xend/Yend exclusive; Image = packed sub-buffer, Width bytes x (Yend-Ystart) rows
    if((Xstart % 8 + Xend % 8 == 8 && Xstart % 8 > Xend % 8) || Xstart % 8 + Xend % 8 == 0 || (Xend - Xstart)%8 == 0)
    { Xstart = Xstart / 8 ; Xend = Xend / 8; }
    else
    { Xstart = Xstart / 8 ; Xend = Xend % 8 == 0 ? Xend / 8 : Xend / 8 + 1; }
    UWORD Width = Xend - Xstart;
    UDOUBLE IMAGE_COUNTER = Width * (Yend - Ystart);
    Xend -= 1;
    Yend -= 1;
    EPD_Reset();                    //> hardware reset only: no power-on, no 0x12, no 0x01/0x0C/0x11 re-send
    EPD_SendCommand(0x18); EPD_SendData(0x80);
    EPD_SendCommand(0x3C); EPD_SendData(0x80);
    EPD_SendCommand(0x44);
    EPD_SendData((Xstart*8) & 0xFF); EPD_SendData(((Xstart*8) >> 8) & 0xFF);
    EPD_SendData((Xend*8) & 0xFF);   EPD_SendData(((Xend*8) >> 8) & 0xFF);
    EPD_SendCommand(0x45);
    EPD_SendData(Yend & 0xFF);   EPD_SendData((Yend >> 8) & 0xFF);      //> Arduino L519-L523 sends Ystart first, then Yend
    EPD_SendData(Ystart & 0xFF); EPD_SendData((Ystart >> 8) & 0xFF);
    EPD_SendCommand(0x4E);
    EPD_SendData((Xstart*8) & 0xFF); EPD_SendData(((Xstart*8) >> 8) & 0xFF);
    EPD_SendCommand(0x4F);
    EPD_SendData(Ystart & 0xFF); EPD_SendData((Ystart >> 8) & 0xFF);
    EPD_SendCommand(0x24);   //Write Black and White image to RAM
    EPD_SendDataBuffer(Image, IMAGE_COUNTER);
    EPD_TurnOnDisplay_Part();
}
```
- X is converted to bytes and back (`*8`). Xstart is truncated to a multiple of 8, so an unaligned region shifts left. The 0x44 end is the *first* pixel of the last byte, e.g. 792 rather than 799 for a full-width call (presumably harmless because RAM is written 8 px at a time (inferred)).
- Only the new image goes to 0x24. RED RAM (0x26) must already hold what's on screen: after `EPD_Display_Base`, or after a previous partial (§6).
- It leaves the RAM window at the partial region and the border at 0x80. A following full-frame write needs `EPD_Init` first (the app always does this).

### 3.5 4-grey conversion ([L603-L686](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/components/epaper_port/epaper_port.c#L603-L686))
```c
EPD_SendCommand(0x24);                       // old  data
for(i=0; i<IMAGE_COUNTER; i++) {             //> IMAGE_COUNTER = 100*480; reads Image[0..95999]
    temp3=0;
    for(j=0; j<2; j++) {
        temp1 = Image[i*2+j];
        for(k=0; k<2; k++) {
            temp2 = temp1&0xC0;
            if(temp2 == 0xC0) temp3 |= 0x00;
            else if(temp2 == 0x00) temp3 |= 0x01;
            else if(temp2 == 0x80) temp3 |= 0x01;
            else /*0x40*/ temp3 |= 0x00;
            temp3 <<= 1;
            temp1 <<= 2;
            // ... same test for the second pixel, then: if(j!=1 || k!=1) temp3 <<= 1;
            temp1 <<= 2;
        }
    }
    EPD_SendData(temp3);
}
EPD_SendCommand(0x26);   // same loop with: 0xC0->0x00 //white, 0x00->0x01 //black, 0x80->0x00 //gray1, 0x40->0x01 //gray2
// ...
EPD_TurnOnDisplay_4GRAY();
```
| 2-bit pixel | 0x24 bit | 0x26 bit | `EPD_Display_4Gray` comment | `GUI_Paint.h` name/comment |
| --- | --- | --- | --- | --- |
| `11` (= `WHITE`%4) | 0 | 0 | white | `GRAY1 0x03 //Blackest` |
| `10` | 1 | 0 | gray1 | `GRAY2 0x02` |
| `01` | 0 | 1 | gray2 | `GRAY3 0x01 //gray` |
| `00` (= `BLACK`) | 1 | 1 | black | `GRAY4 0x00 //white` |

The two comment sets contradict each other. `Paint_Clear(WHITE)` fills `0xFF` (all `11`), and the 01 demo draws `GRAY4` shapes on that canvas. So `11` = white and `00` = black, and the `GUI_Paint.h` comments are backwards (inferred). Which of `10`/`01` is lighter is unverified. The Arduino `EPD_3IN97_WritePicture_4Gray` (L626-L727) fills only the top-left 200×240 area and doesn't reset `temp3` in its 0x26 loop.

### 3.6 Sleep ([L692-L702](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/components/epaper_port/epaper_port.c#L692-L702))
```c
EPD_SendCommand(0x10); //enter deep sleep
EPD_SendData(0x01);
vTaskDelay(pdMS_TO_TICKS(10));
epaper_rst_0;
epaper_cs_0;
epaper_dc_0;
EPD_Power_OFF();                 //> ALDO3 off: controller unpowered, RAM lost; next use needs EPD_Init()
vTaskDelay(pdMS_TO_TICKS(10));
```
Arduino `EPD_3IN97_Sleep`: `0x10 0x01`, then `DEV_Delay_ms(100)` only. The sketch then calls `DEV_Module_Exit()` (RST low).

### 3.7 Command reference
Meanings are from the panel PDF unless marked.

| Cmd | Name | Data sent | Meaning / notes |
| --- | --- | --- | --- |
| 0x01 | Driver output control | `DF 01 02` | A[9:0]=479 → 480 gate lines (POR 0x2A7 = 680). B[2:0]=010: the PDF gives only POR=000; B1 is the SSD16xx "SM" scan bit (inferred) |
| 0x0C | Booster soft-start | `AE C7 C3 C0 80` | Not in the PDF command table. The PDF reference program (p. 21) sends exactly these 5 bytes |
| 0x10 | Deep sleep | `01` | Table lists only 00 normal / 11 deep sleep. Reference program also sends `01` (SSD1677 "mode 1" (inferred)). Leaving it needs RES#. Datasheet: 1 µA typ / 5 µA max, "RAM data not retain" |
| 0x11 | Data entry mode | `01` | ID=01: **Y decrement, X increment**. AM=0: counter advances in X first |
| 0x12 | SWRESET | — | Commands/parameters reset to defaults except 0x10. RAM unaffected. BUSY high while running |
| 0x18 | Temperature sensor | `80` | Internal sensor (POR 0x48 = external I2C sensor) |
| 0x1A | Write temperature register | `6A` (fast) / `5A` (4-grey) | A[11:0] takes 2 data bytes; Waveshare sends only the first (A[11:4]). Forces the OTP LUT for that temperature band. With integer °C in A[11:4]: 106 °C / 90 °C (inferred) |
| 0x20 | Master activation | — | Runs the sequence selected by 0x22. BUSY high until done |
| 0x22 | Display update control 2 | `F7` / `D7` / `FF` | PDF lists only `C7`, `90`, `47`; these confirm b6 analog on, b2 display, b1 analog off, b0 osc off. SSD16xx convention (inferred): b7 clock on, b5 load temperature, b4 load LUT, b3 display mode 2. So **F7** = full (mode 1) with a sensor read, **D7** = same but uses the 0x1A value (fast/4-grey LUT), **FF** = mode 2 (partial/differential) |
| 0x24 | Write RAM (BW) | frame | 1 = white, 0 = black |
| 0x26 | Write RAM (RED) | frame | B/W panel: previous image for mode 2, second bit-plane for 4-grey |
| 0x3C | Border waveform | `01` / `80` | `01`: VBD = GS transition, LUT1. `80`: VBD = VCOM (used by partial). POR `C0` = HiZ |
| 0x44 | RAM X start/end | `XS_lo XS_hi XE_lo XE_hi` | 10-bit, **pixel units** (POR end 0x3BF = 959) |
| 0x45 | RAM Y start/end | `YS_lo YS_hi YE_lo YE_hi` | 10-bit (POR end 0x2A7). Full init: start 479, end 0 (decrementing) |
| 0x4E / 0x4F | RAM X / Y counter | 2 bytes each | Waveshare sets `00 00` for both. The PDF reference program uses `4F DF 01` |

Never sent: 0x03/0x04 (voltages), 0x21 (RAM bypass/inverse), 0x2C (VCOM), 0x32 (custom LUT). Every waveform comes from OTP. The PDF calls the panel "800 gate × 480 source", but the code (like the reference program) drives 480 gates (0x01) × 800 X addresses (0x44).

## 4. Framebuffer format

- **1 bpp** (`Paint.Scale = 2`, the default in `Paint_NewImage`): `WidthByte = 100`, 480 rows, 48000 B, row-major. Bit `0x80 >> (X % 8)` is pixel X, so the MSB is the leftmost pixel. **1 = white, 0 = black** (`WHITE 0xFF`, `BLACK 0x00`; PDF: BW RAM 1 = white). Rows are sent top-down. Native layout is 800 wide × 480 high.
- **2 bpp** (`Paint_SetScale(4)`): `WidthByte = 200`, 96000 B (`EPD_SIZE_4GRAY`), 4 px/byte, MSB pair leftmost. Split into two planes by §3.5. Scales 6/7/16 (4 bpp) exist in `GUI_Paint` for other panels.
- **Rotation/mirroring** live only in `Paint_SetPixel`. Logical W/H are swapped for 90/270. `ROTATE_270`: `X = Ypoint; Y = HeightMemory - Xpoint - 1`. The app uses `Paint_NewImage(buf, EPD_WIDTH, EPD_HEIGHT, 270, WHITE)` for portrait (480×800) menu pages and rotation 0 for landscape clock/calendar. The buffer sent to the panel is always in native layout.
- **Allocation**: `heap_caps_malloc(EPD_SIZE_MONO, MALLOC_CAP_SPIRAM)` (main.cc L498; page_clock L351 adds a second buffer, `Image_Mono_last`). The 01 example uses plain `malloc(EPD_SIZE_MONO)`. Shipped `sdkconfig`: octal PSRAM, `CONFIG_SPIRAM_USE_MALLOC=y`.
- Partial sub-images are separate small Paint canvases, e.g. `Paint_NewImage(Image_Mono, Font16.Width * 7, Font16.Height, 0, WHITE)` in the 01 example. The partial region must be exactly that many bytes wide.

## 5. GUI_Paint, fonts, BMP

`GUI_Paint` keeps state in a global `PAINT Paint {Image, Width, Height, WidthMemory, HeightMemory, Color, Rotate, Mirror, WidthByte, HeightByte, Scale}`. Coordinates are logical (post-rotation). Most draw calls bail out (with a log) if a coordinate is `> Paint.Width/Height`.

| Function | Behaviour |
| --- | --- |
| `Paint_NewImage(img, w, h, rotate, color)` | Binds the buffer, sets memory size, rotation, Scale=2. Doesn't clear |
| `Paint_SelectImage(img)` / `Paint_SetRotate(r)` / `Paint_SetMirroring(m)` | Swap buffer / rotation (0/90/180/270) / mirror (NONE, HORIZONTAL, VERTICAL, ORIGIN) |
| `Paint_SetScale(2\|4\|6\|7\|16)` | Bits per pixel; recomputes `WidthByte` |
| `Paint_SetPixel(x, y, c)` | Rotate → mirror → bit write. Bounds test is `>`, not `>=` |
| `Paint_Clear(c)` / `Paint_ClearWindows(xs, ys, xe, ye, c)` | Fill buffer / fill rectangle (xe/ye exclusive) |
| `Paint_DrawPoint(x, y, c, DOT_PIXEL_1X1..8X8, DOT_FILL_AROUND\|RIGHTUP)` | Square dot of (2n−1)² px centred, or n² px |
| `Paint_DrawLine(xs, ys, xe, ye, c, width, LINE_STYLE_SOLID\|DOTTED)` | Bresenham. Dotted paints every 3rd dot `IMAGE_BACKGROUND` (white) |
| `Paint_DrawRectangle(xs, ys, xe, ye, c, width, DRAW_FILL_EMPTY\|FULL)` | Filled mode draws rows ys..ye−1 |
| `Paint_DrawCircle(x, y, r, c, width, fill)` | Midpoint circle |
| `Paint_DrawChar` / `Paint_DrawString_EN(x, y, s, sFONT*, fg, bg)` | ASCII ≥ 0x20, wraps at `Paint.Width`, stops at the bottom. `malloc`s a glyph buffer per character |
| `Paint_DrawString_CN(x, y, s, cFONT*, fg, bg)` | UTF-8 or GBK mixed text (below) |
| `Paint_DrawNum` / `Paint_DrawNumDecimals` / `Paint_DrawTime` | Integer / fixed decimals / `HH:MM:SS` with `sFONT` |
| `Paint_DrawBitMap(buf)` | Copies a full raw buffer |
| `Paint_ReadBmp(arr, x, y, w, h)` | Embedded 1-bpp array; **set bit = BLACK** (inverse of the framebuffer); w rounded up to 8 |
| `reassignCoordinates_EN/_CH(x, s, font)` (font.c) | Returns x minus half the string width, for centring |

**Fonts** ([font.h L16-L38](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/components/epaper_lib/Fonts/font.h#L16-L38)):
```c
typedef struct {
    const char *font_name_EN;      // English font file path
    const char *font_name_CH;      // The file path of Chinese fonts
    const char *font_name_CH_ASICC; // File path of Chinese ASCII extended font
    uint16_t Width_EN;  uint16_t Width_CH;  uint16_t Height;
    uint16_t size_EN;   uint16_t size_CH;   // bytes per glyph
    FONT_ENCODING_TYPE encoding;   // FONT_ENCODING_UTF8 = 0, FONT_ENCODING_GBK = 1
}cFONT;
typedef struct { const char *font_name; uint16_t Width; uint16_t Height; uint16_t size; }sFONT;
```
- Glyph = `size` bytes, row-major, each row `Width/8` bytes MSB-first (`size = Width*Height/8`; all widths are multiples of 8). A set bit is drawn in `fg`.
- Sizes as (EN width / CH width / height): 12 → 8/16/21, 16 → 16/24/28, 18 → 16/24/31, 24 → 24/32/41, 28 → 24/40/48, 36 → 32/48/62, 48 → 40/64/83. EN-only: `Font80` 80×106, `Font182` 120×182 (the clock digits). The name isn't the pixel height. Instances: `FontNN` (`sFONT`), `FontNN_UTF8`, `FontNN_GBK`.
- Data source: `.FON` files are linked via `EMBED_FILES` (`_binary_<file>_FON_start/_end`) when `CONFIG_FONTnn_EMBEDDED`. The embedded blob is picked by matching `size_EN`/`size_CH` (`get_embedded_font_data`, font.c L250). Otherwise each glyph is read with `fopen`/`fseek` from `/sdcard/font/{ASICC,UTF,GBK}/...`. Kconfig embeds only Font18 by default; the shipped `sdkconfig` embeds 12/16/18/24 + auxiliary (80/182).
- UTF-8 lookup (`Get_Char_Font_Data`): ASCII → EN file at `(c-0x20)*size_EN`. **U+4E00–U+9FFF → UTF CH file at `(u-0x4E00)*size_CH`** (a direct-indexed CJK block). U+FF01–FF5E (full-width ASCII) → CH_ASICC file at `(u-0xFF00)*size_CH`. Anything else → `utf8_char_to_gb2312()` table → GB2312 symbol-area offset `((b1-0xA1)*94+(b2-0xA1))*size_CH` in the GBK CH_ASICC file (embedded only for 18 pt). GBK fonts: rows A1–A9 are symbols, B0–F7 are hanzi.
- `Paint_DrawString_CN` walks the string by UTF-8 sequence length. Advance is `Width_EN` for 1-byte characters, else `Width_CH`. Unknown glyphs are skipped with a warning.
- The Arduino example has a different, compiled-in font system: `sFONT {table, Width, Height}` (STM fonts 8/12/16/20/24), and `cFONT` holding a small `CH_CN {index[4], matrix[]}` table searched linearly by UTF-8 bytes. Only the glyphs present in `font12CN.cpp`/`font24CN.cpp` render.

**BMP from SD** (`GUI_ReadBmp`, [L20-L107](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/components/epaper_lib/GUI_BMPfile.c#L20-L107)): reads the packed 14-byte file header and 40-byte info header, then a 2^bpp palette. If palette[0] is white, set bits are black. Rows are read bottom-up, padded to 4 bytes, and plotted with `Paint_SetPixel` at (x, y), with `vTaskDelay(1)` per row. Side effect: an 800×480 BMP forces `Paint_SetRotate(0)` and a 480×800 BMP forces `Paint_SetRotate(90)`. `GUI_ReadBmp_4Gray` takes 4-bpp BMPs (`color = nibble >> 2`) and calls `exit(0)` on error. The factory app uses compiled-in icons (`CONFIG_IMG_SOURCE_EMBEDDED`, `Paint_ReadBmp(gImage_*)`) unless `CONFIG_IMG_SOURCE_TFCARD` is set.

## 6. How the factory app uses the driver

- **Boot** (main.cc L490-L502): `epaper_port_init(); EPD_Init();`, then allocate `Image_Mono` in PSRAM. `EPD_Clear()` runs only on the Wi-Fi-enabled path.
- **Only two refresh modes are used**: `Global_refresh` → `EPD_Display_Base(buf)` and `Partial_refresh` → `EPD_Display_Partial(buf, 0, 0, EPD_WIDTH, EPD_HEIGHT)`, which is a full-screen differential update. Fast, 4-grey, OneShot and windowed partials appear only in demos. Pages typically define a `Forced_refresh_X()` (Base) and `Refresh_page_X()` (Partial) pair (clock, alarm, settings, weather, fiction).
- **When a full refresh happens**: the first home draw. **Function long-press (button code 12)** on the home menu (`EPD_Init(); esp_home(..., Global_refresh)`) and on most pages (clock, fiction, …). Clock page: when the hour changes (otherwise partial every minute). RTC-wake clock mode: at :00 and :30 (otherwise partial). Menu navigation and minute ticks are partial. No page counts partials to schedule a full refresh automatically.
- **Sleep policy**: interactive loops count 1 s key-wait timeouts. After `EPD_Sleep_Time` (60) idle ticks they call `EPD_Sleep()` (deep sleep + ALDO3 off). While asleep, each minute runs `EPD_Init()` → partial(previous frame) → draw → partial(new frame) → `EPD_Sleep()`. After `Unattended_Time` (10) such minutes it calls `axp_pwr_off()`. The clock page wraps every minute update in `EPD_Init` … `EPD_Sleep`. RTC-wake clock/calendar modes do draw → `EPD_Sleep()` → arm RTC alarm (clock: +1 min) → `axp_pwr_off()`.
- **Baseline restore after a power cycle (key pattern)**: the panel was unpowered, so RED RAM is undefined. The app first pushes the frame already on screen as a partial, then the new frame:
  ```c
  // page_clock.cc display_clock_img L1201-L1203
  EPD_Display_Partial(Image_Mono_last, 0, 0, EPD_WIDTH, EPD_HEIGHT);
  EPD_Display_Partial(Image_Mono, 0, 0, EPD_WIDTH, EPD_HEIGHT);
  ```
  main.cc L336-L339 does the same with the unchanged `Image_Mono`. In RTC-wake clock mode the last frame is saved to `/sdcard/GUI/clock_partial.bin` and reloaded before the first partial (`display_clock_mode_img` L1093-L1099). This only works if a mode-2 update leaves the shown image as the next "old" image in RED RAM (inferred from this usage; not documented).
- **Timings**: no measurements in the source. Panel PDF at 23 °C: full 3 s, fast 1.5 s (matches the `//Fast(1.5s)` comment), partial 0.3 s, 4-grey 3 s; peak current 120 mA. Driver overhead from the code: `EPD_Init` ≥ 10 + 102 (reset) + 3×100 (busy pre-delays) ≈ 0.41 s plus SWRESET. `Init_Fast`/`Init_4GRAY` add 0.5 s. Each partial adds a 102 ms reset and every update a ≥ 100 ms busy pre-delay. Arduino bit-bang SPI plus `delay(1)` per row adds ≥ 0.48 s per plane.

## 7. Porting notes

**Keep**
- The OTP-LUT sequences byte for byte: 0x0C `AE C7 C3 C0 80`, 0x18 `80`, 0x01 `DF 01 02`, 0x11 `01`, windows, and 0x22 `F7`/`D7`/`FF` with 0x1A `6A`/`5A`. No custom LUT is needed.
- The Base-then-partial model: write both RAMs on a full refresh, write only 0x24 on partials, and restore the previous frame after any power cut. Keep a copy of the last shown frame in PSRAM, or on SD across PMIC power-off.
- ALDO3 on → ≥ 10 ms → reset → SWRESET → init, and deep sleep (0x10 `01`) before cutting ALDO3. This matches the AGENTS.md low-power loop.

**Replace**
- **SPI**: use IDF `spi_master` on `SPI3_HOST` (or `SPI2_HOST`), 20 MHz (datasheet max), mode 0, with `spics_io_num = board::EPD_CS` so CS frames each transaction. Send a command as one 1-byte transaction (DC low), and its parameters as one transaction (DC high, `SPI_TRANS_USE_TXDATA` for ≤ 4 bytes) instead of one transaction per byte. Arduino `SPIClass` + `writeBytes` also works. (Inferred: in core 3.x on S3, `FSPI`→SPI2 and `HSPI`→SPI3; check `esp32-hal-spi.h`.) Map every pin to `board::EPD_*`.
- **Frame transfer from PSRAM**: keep 4 KB chunks, or copy through a small internal DMA buffer. Without one, `spi_master` may allocate an internal bounce buffer as large as the transaction (inferred; check IDF 5.5 behaviour for PSRAM buffers).
- **BUSY wait**: replace the 100 ms pre-delay and unbounded loop with a short settle, polling or a GPIO interrupt plus semaphore, and a timeout (e.g. 5 s full, 2 s partial) that reports an error. Decide the BUSY pull deliberately instead of inheriting the typo.
- **Partial**: re-send the window every time and restore the full window before full-frame writes. Require 8-aligned `x` and `w`. Use one 0x45 convention: with 0x11=`01`, start = bottom row and end = top row, as in IDF/`EPD_Init`.

**Pitfalls found in the vendor code**
1. CS is never deasserted (IDF), so there's no byte resynchronisation if SCK glitches (inferred).
2. `EPD_Display_Partial` relies on 0x01/0x0C/0x11/0x1A surviving a RES# pulse. The panel PDF doesn't say whether RES# restores POR defaults (inferred risk; verify, or re-send the init sequence).
3. 0x4F is `00 00` while the Y window starts at 479 in Y-decrement mode. The PDF reference program uses `DF 01`. With wrap-around this may roll the image by one row (inferred; check row 0/479 on hardware).
4. IDF and Arduino partials send 0x45 in opposite order under the same entry mode. Only one can be right for non-full-height windows; the app only exercises full-screen partials.
5. 0x1A sends 1 of its 2 data bytes. `EPD_Init_Partial` is declared but missing. `spi_send_data()` (L64) is unused.
6. **01 example overflow**: `malloc(EPD_SIZE_MONO)` (48000), then `Paint_SetScale(4)` + `EPD_Display_4Gray` touch 96000 bytes. Use `EPD_SIZE_4GRAY` for 4-grey.
7. `Paint_SetPixel` and the draw functions bound-check with `>`, so x == width writes into the next row and y == height writes past the buffer.
8. Per-byte transactions: `EPD_Clear_Black` and `EPD_Display_4Gray` make 96,000 single-byte SPI calls. `Paint_DrawChar` mallocs per glyph, and SD-backed fonts `fopen` per glyph.
9. `GUI_ReadBmp` silently changes the global rotation, leaks the `FILE*` on header errors, and reads `biBitCount` bytes into a 1-byte variable (overflow for non-1-bpp files).
10. `EPD_Sleep` cuts panel power. Anything after it (including `EPD_Display_Partial`, which doesn't power on) needs `EPD_Init` first.
11. Blocking delays everywhere: reset 102 ms, busy 100 ms + 20 ms polls, 500 ms in fast/4-grey init. `epaper_bsp.c` helpers add 500 ms + 1000 ms around each display.
12. 4-grey colour names in `GUI_Paint.h` contradict the driver (§3.5).

## 8. License headers

- **ESP-IDF** `epaper_port.c/.h`, `epaper_bsp.c/.h`, `GUI_Paint.c/.h`, `GUI_BMPfile.c/.h`, `Fonts/font.c/.h`, `Fonts/fonts.c/.h`, `01_E-Paper_Example/main/main.c`, `main.cc`, `page_clock.cc`: **no license header**. `epaper_lib/Debug.h` has the Waveshare permission text below. The repo has no top-level LICENSE at this commit; the only LICENSE files are `Arduino/libraries/SensorLib/LICENSE` and `ESP-IDF/02_Mic_test/components/codec_board/LICENSE`.
- **Arduino** `EPD_3in97.cpp/.h`, `DEV_Config.cpp/.h`, `GUI_Paint.cpp`: Waveshare header (`* | Author : Waveshare team`; EPD files claim "Function : 1.54inch e-paper V2", V1.0 2019-06-11). They contain this MIT-style text with no named copyright holder; `EPD_3in97.h` omits the first line:
  ```
  # Permission is hereby granted, free of charge, to any person obtaining a copy
  # of this software and associated documnetation files (the "Software"), to deal
  # in the Software without restriction, including without limitation the rights
  # to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  # copies of the Software, and to permit persons to  whom the Software is
  # furished to do so, subject to the following conditions:
  #
  # The above copyright notice and this permission notice shall be included in
  # all copies or substantial portions of the Software.
  #
  # THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  # IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  # FITNESS OR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  # AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  # LIABILITY WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  # OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
  # THE SOFTWARE.
  ```
- Arduino `font8/12/16/20/24.cpp` and `font12CN/font24CN.cpp`: `COPYRIGHT(c) 2014 STMicroelectronics`, 3-clause BSD text. (`GUI_Paint.cpp` uses `*` instead of `#` line prefixes.)
