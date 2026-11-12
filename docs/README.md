# Reference documentation — Waveshare ESP32-S3-ePaper-3.97

Vendor material for the target board, mirrored on 2026-11-12 from:

- Product page: <https://www.waveshare.com/esp32-s3-epaper-3.97.htm>
- Wiki: <https://docs.waveshare.com/ESP32-S3-ePaper-3.97>
- Example code: <https://github.com/waveshareteam/ESP32-S3-ePaper-3.97> (not mirrored — clone it if you need the drivers)

The distilled facts live in [`../include/board_pins.h`](../include/board_pins.h) (pins, I2C
addresses) and [`../AGENTS.md`](../AGENTS.md) (power architecture, driver notes, gotchas). Use this
folder when you need to check the primary sources.

## board/

| File | What it is |
| --- | --- |
| `ESP32-S3-ePaper-3.97-schematic.pdf` | Full schematic (page 1) + silkscreen/placement (page 2). Page 1 has the authoritative **GPIO allocation table** (bottom centre). |

## mechanical/  (from Waveshare's "Structure" zip, rev 2026-08-21)

| File | What it is |
| --- | --- |
| `ESP32-S3-EPAPER-3.97-20260821.pdf` | 2D dimension drawing |
| `ESP32-S3-EPAPER-3.97-20260821.dxf` | 2D outline for CAD (9 MB) |
| `ESP32-S3-EPAPER-3_97-20260821.stp` | 3D STEP model of the board (22 MB) — for enclosure design |

## datasheets/

| File | Part | Role on the board |
| --- | --- | --- |
| `ESP32-S3-datasheet.pdf` | ESP32-S3 | MCU (in ESP32-S3-WROOM-1-N16R8 module) |
| `ESP32-S3-technical-reference-manual.pdf` | ESP32-S3 | Register-level TRM (15 MB) |
| `3.97inch-e-Paper-panel.pdf` | 3.97" 800×480 EPD, SSD1677 controller | Display |
| `TG28-PMIC.pdf` | TG28 | PMIC / charger / fuel gauge (AXP2101 register-compatible) |
| `ES8311-audio-codec.pdf` | ES8311 | Audio codec (mic in, speaker out) |
| `PCF85063A-RTC.pdf` | PCF85063A | Real-time clock (alarm can power the board on) |
| `SHTC3-temp-humidity.pdf` | SHTC3 | Temperature & humidity sensor |
| `QMI8658A-IMU.pdf` | QMI8658A (Rev A) | 6-axis IMU — not linked from the wiki; fetched from Waveshare's file server |

Not mirrored (no datasheet published by Waveshare): NS4150B class-D speaker amplifier.

## wiki/

Markdown conversions of each wiki page (images in `wiki/images/`). Note: the wiki's tutorial pages
contain boilerplate saying "this tutorial uses the ESP32-S3-Zero" — ignore that; the pins in the
examples are for this board.

| File | Content |
| --- | --- |
| `00-Overview.md` | Features, onboard resources, interface diagram, dimensions |
| `01-Resources-And-Documents.md` | Link list + community projects built on this board |
| `02-Arduino.md` | Arduino IDE setup (board settings screenshot) and the 6 Arduino examples |
| `03-ESP-IDF.md` | ESP-IDF/VS Code setup and the 8 IDF examples, incl. factory firmware walkthrough |
| `04-Firmware-Flashing.md` | Flashing .bin images with Espressif Flash Download Tool |
| `05-ESP32-AI-Tutorials.md` | XiaoZhi AI voice assistant firmware |
| `06-FAQ.md` | Vendor FAQ |
| `07-Technical-Support.md` | Support contact |
