# UI design: INKDECK/OS screens

Design canvas (claude.ai artifact):
<https://claude.ai/artifact/CkQZ5d3cCFz6dFwQNNuwof>

The canvas is private to its owner. To give someone else access, use its **Share** menu. Each
screen is a separate artboard. Home and every app screen's `[BOOT]` key are linked, so you can
click through them in Play mode. Hovering a row on Home stands in for the rotary switch.

The direction is brutalist and terminal-like: a "cyberdeck" look built only from black ink on
paper.

## Orientation

| Screens | Size | How |
| --- | --- | --- |
| Home and all app screens | **480×800 portrait** | Panel rotated 90° in software. Same 48,000-byte 1 bpp framebuffer |
| Clock, power-off screen | **800×480 landscape** | Native panel orientation |

Artboards are drawn at 1 CSS px = 1 panel pixel, so sizes in the mockups map directly to device
pixels.

## Visual rules

- **1-bit only.** Every mark is ink or paper. The mockups use `#111111` ink on `#E8E6DF` paper so
  they look like the panel; on the device these become black and white. No opacity and no grey
  text.
- **Greys are dithers.** Fills that need a tone (progress bars, waveform not yet played, unplayed
  rain bars, the battery's partial cell) use a 2×2 checkerboard, which is 50 % ink.
- **Selection is inversion.** The cursor row is white-on-black with a `▶` marker. There's no
  hover or focus colour.
- **Structure:** 3 px rules between regions, 1 px dashed rules between rows, hard edges, no radii.
  The few "buttons" have a dithered drop shadow offset 6 px.
- **Type:** JetBrains Mono (400/500/700/800) for all UI. Literata for book text in the reader.
  Both are SIL OFL 1.1, so they can be converted to bitmap fonts for the firmware. The minimum
  size is 13 px for labels; body text is 15–19 px.
- **Icons:** stroke-only and pixel-stepped (square caps, crisp edges), drawn on a 48-unit grid.
  No emoji.

## Shared chrome

| Element | Height | Content |
| --- | --- | --- |
| Status bar | 36 px | Inverted module tag (`03 PODCASTS`), context text (path, feed, location), then battery glyph + %, clock |
| Section header | 30 px | `// LABEL` on the left, count or I2C address on the right, 3 px rule under it |
| Key-hint bar | 34 px | Up to 4 hints, each a key cap in a 2 px outline plus a verb (`[▲▼] MOVE`, `[●] PLAY`, `[HOLD ●] GET`, `[BOOT] BACK`) |

## Input mapping

The device has no touch screen. All navigation uses the buttons in `include/board_pins.h`:

| Hint | Button | GPIO |
| --- | --- | --- |
| `▲` / `▼` | Rotary switch up / down (`BTN_UP`, `BTN_DOWN`) | 4 / 6 |
| `●` | Rotary push (`BTN_PRESS`); `HOLD ●` = long press (≈1 s) | 5 |
| `BOOT` | Back (`BTN_BOOT`) | 0 |
| `PWR` | PMIC `PWRON` key: 1 s = on, 4 s = forced off | — |

## Screens

| Artboard | Orientation | What it shows | Hardware it exercises | Keys |
| --- | --- | --- | --- | --- |
| `Main` (Home) | Portrait | Preview of the selected module (big index, name, bus tag, 4 stats, `● OPEN`) above a 10-row module list | — | ▲▼ move · ● open · BOOT back · PWR off |
| `Reader` | Portrait | Book page (Literata 19/30, justified, inverted drop cap), page counter and progress bar | microSD (SDMMC 4-bit) | ▲ prev · ▼ next · ● menu · BOOT back |
| `Music` | Portrait | Now playing, 1-bit waveform overview with playhead, time, play state, volume, 4-track queue, signal chain | ES8311, NS4150B, I2S, ALDO1, microSD | ▲▼ volume · ● pause · HOLD ● skip · BOOT back |
| `Podcasts` | Portrait | Feed list, episode list with state (`[NEW]`, `[DL 62%]`, `[▶ 23m]`, `[✓]`), selected-episode card, SD cache meter | Wi-Fi, microSD, audio path | ▲▼ move · ● play · HOLD ● download · BOOT back |
| `Weather` | Portrait | Current temp + condition, feels/wind/RH/sunset, 12-hour temp + rain-chance bars, 6-day list with lo–hi range bars | Wi-Fi (Open-Meteo, synced hourly while the board is on; no wake from power-off) | ▲▼ day · ● detail · HOLD ● sync · BOOT back |
| `Sensors` | Portrait | SHTC3 temp/RH with 24 h sparkline, IMU bubble level + accel/gyro table, PMIC battery readings, live `i2cdetect` grid of the bus | SHTC3 `0x70`, QMI8658A `0x6B` (the canvas says 0x6A; the chip answers at 0x6B), TG28 `0x34`, PCF85063A `0x51`, ES8311 `0x18` | ▲▼ panel · ● hold · HOLD ● log · BOOT back |
| `System` | Portrait | Battery % + V/I + segmented gauge + charge config, rail table (DC1, ALDO1–3 on/off), sleep/wake/Wi-Fi/OTA/memory state, action list (sleep now, NTP sync, update check, reboot) | TG28 rails and charger, RTC wake, Wi-Fi, OTA partitions | ▲▼ move · ● run · BOOT back · PWR off |
| `Clock` | Landscape | Large HH:MM, weekday/date, ISO week + day of year, 24-cell day-progress bar, month calendar, alarm list with on/off | PCF85063A alarms, NTP | ▲▼ alarm · ● on/off · HOLD ● edit · BOOT back |
| `Sleep` (power-off) | Landscape | Left: dot-matrix field (36×28 cells of 14 px) with a pixel-art frog asleep on a lily pad, `Z z` rising off it. Three tones: solid ink outline, 50 % dither body, paper belly, plus a 25 % dither shadow one cell down-right. Unlit cells are 2 px dots. Strips above and below show halt time and what to resume. Right: inverted panel with the wake time (`15:00`, RTC alarm), reason, wake path (RTC INT → PWRON), battery, alarm, and "hold PWR 1 s to boot" | Last frame drawn before `axp2101.shutdown()`; RTC alarm boots the board | None (board is off) |

Home also lists **Recorder** (mic → WAV on SD), **Level** (IMU tilt) and **Files** (SD + FAT
browser). These rows show a preview but have no screen designed yet.

All on-screen content is sample data: book text (H. G. Wells, public domain), track list, the
fictional podcast feeds, weather and sensor values.
