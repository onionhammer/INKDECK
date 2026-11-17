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
| Clock, Level, pictures held in landscape, power-off screen | **800×480 landscape** | Native panel orientation turned 180° (firmware), so the rotary wheel is on the top edge. The navigator swaps ▲ and ▼ there: rolling left is ▲, right is ▼, except in the picture viewer, which turns with the board and keeps the portrait sense (`Screen::swapsDial()`) |

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
| Status bar | 36 px | Inverted module tag (`02 PODCASTS`), context text (path, feed, location), then battery glyph + %, clock. While audio is playing or paused, an inverted ▶ or ‖ comes first in the right block (firmware addition) |
| Section header | 30 px | `// LABEL` on the left, count or I2C address on the right, 3 px rule under it |
| Key-hint bar | 34 px | Up to 4 hints, each a key cap in a 2 px outline plus a verb (`[▲▼] MOVE`, `[●] PLAY`, `[HOLD ●] GET`, `[BOOT] BACK`) |
| Menu sheet (firmware addition) | 3 px rule + 30 px header + 34 px a row | Rises from the bottom over the screen below (whose status bar stays): `// TITLE`, then one list row per item, value right-aligned. It grows to fit, up to the whole body; a longer list shows a page at a time, with the cursor's place (`03/12`) on the header's right. Hints `[▲▼] MOVE [●] SELECT [BOOT] CLOSE` (`src/ui/menu.h`) |

## Input mapping

The device has no touch screen. All navigation uses the buttons in `include/board_pins.h`:

| Hint | Button | GPIO |
| --- | --- | --- |
| `▲` / `▼` | Rotary switch up / down (`BTN_UP`, `BTN_DOWN`); swapped on landscape screens, where the wheel is on top | 4 / 6 |
| `●` | Rotary push (`BTN_PRESS`); `HOLD ●` = long press (≈1 s) | 5 |
| `BOOT` | Back (`BTN_BOOT`); `HOLD BOOT` = home. While audio is playing or paused, `HOLD BOOT` opens the player instead, and `HOLD BOOT` on that player goes back to where you were | 0 |
| `PWR` | PMIC `PWRON` key: 1 s = on, 4 s = forced off | — |

While the lock screen is up, `HOLD BOOT` goes no further down than the lock (a full refresh on the
lock itself) and never opens the player. `PWR` works as usual.

## Screens

| Artboard | Orientation | What it shows | Hardware it exercises | Keys |
| --- | --- | --- | --- | --- |
| `Main` (Home) | Portrait | Preview of the selected module (big index, name, bus tag, 4 stats, `● OPEN`) above a 10-row module list. Order (the user's, by use; differs from the canvas): Reader, Podcasts, Music, Weather, Clock, Recorder, Sensors, Level, Files, System. Each screen's status-bar tag carries its number (`apps::moduleTag()`) | — | ▲▼ move · ● open · BOOT back · PWR off |
| `Reader` | Portrait | Book page (Literata 19/30, justified, inverted drop cap), page counter and progress bar | microSD (SDMMC 4-bit) | ▲ prev · ▼ next · ● menu · BOOT back |
| `Music` | Portrait | Now playing, 1-bit waveform overview with playhead, time, play state, volume, 4-track queue, signal chain | ES8311, NS4150B, I2S, ALDO1, microSD | ▲▼ volume · ● pause · HOLD ● skip · BOOT back |
| `Podcasts` | Portrait | Feed list, episode list with state (`[NEW]`, `[DL 62%]`, `[▶ 23m]`, `[✓]`), selected-episode card, SD cache meter | Wi-Fi, microSD, audio path | ▲▼ move · ● play · HOLD ● download · BOOT back (firmware: ● opens a show, then ● plays or GETs an episode, HOLD ● marks it played or cancels a download, HOLD ● on the show list refreshes; see `src/apps/podcasts.h`) |
| `Weather` | Portrait | Current temp + condition, feels/wind/RH/sunset, 12-hour temp + rain-chance bars, 6-day list with lo–hi range bars | Wi-Fi (Open-Meteo, synced hourly while the board is on; no wake from power-off) | ▲▼ day · ● detail · HOLD ● sync · BOOT back |
| `Sensors` | Portrait | SHTC3 temp/RH with 24 h sparkline, IMU bubble level + accel/gyro table, PMIC battery readings, live `i2cdetect` grid of the bus | SHTC3 `0x70`, QMI8658A `0x6B` (the canvas says 0x6A; the chip answers at 0x6B), TG28 `0x34`, PCF85063A `0x51`, ES8311 `0x18` | ▲▼ panel · ● hold · HOLD ● log · BOOT back |
| `System` | Portrait | Battery % + V/I + segmented gauge + charge config, rail table (DC1, ALDO1–3 on/off), sleep/wake/Wi-Fi/OTA/memory state, action list (sleep now, NTP sync, update check, reboot). Firmware adds AUTO FULL REFRESH with its value on the right (2/5/10/20 SCREENS, OFF), PIN LOCK (OFF, ON, `ON · 5 MIN`; ● sets a PIN, or once there is one opens a `// PIN LOCK` sheet with CHANGE PIN and CLEAR PIN, all through the Lock screen's wheels) and WI-FI DRIVE (opens Wi-Fi Drive). Seven don't fit the body, so the actions are an `// ACTIONS` menu sheet (SLEEP NOW, SYNC TIME (NTP), AUTO FULL REFRESH, PIN LOCK, UPDATE OVER WI-FI, WI-FI DRIVE, REBOOT) that ● opens on the action last run. One that changes a value (AUTO FULL REFRESH) leaves it open, one that opens a screen takes its place, and one with something to say closes it. The body ends with an `// ACTIONS` band instead: the count on the right (or the last action's notice in place of the header), AUTO FULL REFRESH's and PIN LOCK's values on the left (`FULL REFRESH`, `PIN LOCK`) and a `● ACTIONS` button on the right. STATE has one more row: SD CARD (size, mounted). The update check is UPDATE OVER WI-FI, which opens the Updater. The context line shows the build's git commit in place of the IDF version (`FW 0.1.0 · 60bf715`; IDF moved to the Updater) | TG28 rails and charger, RTC wake, Wi-Fi, OTA partitions | ● actions · BOOT back · PWR off. In the sheet: ▲▼ move · ● run · BOOT close |
| `Updater` (firmware addition) | Portrait | Under System's tag, context `UPDATE OVER WI-FI`. `// STATE` panel with the phase in an inverted pill (JOINING, LISTENING, RECEIVING, DONE, FAILED, STOPPED), a 38 px headline (`9 MIN LEFT`, `42%`, `UPDATED`, `NO UPDATE`), a detail line (`1.24 / 2.71 MB into the idle slot`, or the last attempt's error) and System's 10-cell gauge. Then `// ADDRESS` (host, IP, network; port on the right), `// ON THE COMPUTER` (the `pio` command, password and firewall notes), `// FIRMWARE` (running build, OTA slots; IDF on the right) | Wi-Fi, OTA partitions (espota) | Listening: BOOT cancel. Receiving: BOOT does nothing (hint `BOOT —`). Stopped or failed: ● listen again · BOOT back |
| `Wi-Fi Drive` (firmware addition) | Portrait | Under System's tag, context `WI-FI DRIVE`, laid out like the Updater. `// STATE` panel with the phase in an inverted pill (JOINING, READY, CONNECTED, READING, WRITING, FAILED, STOPPED), a 38 px headline (`INKDECK.LOCAL` while serving, else `JOINING…`, `NO DRIVE`, `STOPPED`; the font has capitals only) and a detail line (`or 192.168.1.42 · HOME-NET`, the error, `● starts it again`). Then `// SIGN IN` (DIGEST on the right: USER `inkdeck`, with a count of wrong sign-ins; PASSWORD), `// TRANSFER` (the rate in KB/S on the right while data moves, `IDLE · STOPS IN 4 MIN` in the idle stop's last five minutes, else `WEBDAV · PORT 80`: WRITTEN, READ, LAST change such as `wrote Books/Dune.epub`) and `// ON THE COMPUTER` (SAME NETWORK: Finder's and Windows' way in, when it stops). Counts redraw at most every 5 s. Opening it starts the WebDAV server (`src/webdav.h`) and leaving it stops it; the board's own use of the card carries on, and leaving rereads `inkdeck.ini` and Podcasts if the computer changed them. With a PIN set and `files` in `INKDECK_PIN_ON`, it opens through Files' wheels unless System's own PIN already let you in (`src/apps/wifi_drive.h`) | Wi-Fi, mDNS (`_webdav._tcp`), microSD (FatFs) | Serving: BOOT stop (with data moving in the last 4 s it asks first: `// STOP SHARING?` CANCEL / STOP, `COPY UNDER WAY`). Stopped or failed: ● start · BOOT back |
| `Lock` (firmware addition) | Portrait | Tag `LOCKED`; context `ENTER PIN`, or `NEW PIN ON UNLOCK` / `PIN OFF ON UNLOCK` while a change from the card waits. A pixel-art padlock in the Sleep frog's sprite language (12 px cells: ink outline, 50 % dither shackle and body, paper keyhole, 25 % dither shadow one cell down-right) in a 290 px band. Then `// PIN` with `DIGIT 2/4` on the right, and one combination wheel per digit (4–8, at most 84 px wide, 12 px apart). The active wheel has an inverted 104 px window with its digit in 64 px ExtraBold, the digits on either side of it in 28 px above and below, and ▲ / ▼ outside those: ▲ turns the wheel to the digit above, ▼ to the one below. Entered wheels are framed with an 18 px block (masked); wheels still to come show a dash. After a wrong PIN, a `! WRONG PIN` notice at the bottom until the next key. Comes up at every start, under the off screens (Sleep, Standby, desk face), and after `lock_minutes` idle (`src/apps/lock.h`), when the build's `INKDECK_PIN_ON` names `power_on` (`src/pin_places.h`). System's PIN LOCK uses the same screen under System's tag, with the context `CURRENT PIN`, `NEW PIN` or `NEW PIN AGAIN`. NEW PIN has eight wheels, `DIGIT 5 · 4 TO 8` on the right; the four it may leave out have a 1 px dashed outline. A second entry that differs goes back to NEW PIN with `! THE TWO DIDN'T MATCH: AGAIN`. With `settings` or `files` in `INKDECK_PIN_ON`, Home's SYSTEM or FILES opens through the same screen under that module's tag (`10 SYSTEM`, `09 FILES`; context as the lock's) when a PIN is set and the module hasn't been on top in the last minute; the right PIN replaces it with the module. It isn't a lock: HOLD BOOT goes home from it. The same wheels come back in front of System or Files when its screen returns to the top more than a minute after it was last used, or stays on top a minute without a key; there BOOT on the first wheel says `HOME` and goes home. After three wrong PINs in a row on any of these screens, the next try waits 30 s, doubling with each further miss up to 30 min: the foot shows `! WRONG PIN: WAIT 25 S` (5 s steps, then whole minutes), the wheels take no keys and the hints show only BOOT (when it does something). The count is kept in NVS | — | ▲▼ digit · ● next / unlock · BOOT previous digit. Before System or Files: ● OPEN on the last wheel, BOOT on the first wheel back to Home. In System's steps: ● OK / SAVE / CLEAR on the last wheel, HOLD ● done (NEW PIN, from the 4th wheel), BOOT on the first wheel cancels |
| `Clock` | Landscape | Large HH:MM, weekday/date, ISO week + day of year, a weather strip under the date (not on the canvas: icon, outside temperature, condition, today's high/low; inside temperature and RH), 24-cell day-progress bar, month calendar, alarm list with on/off | PCF85063A alarms, NTP | ▲▼ alarm · ● on/off · HOLD ● edit · BOOT back |
| `Recorder` | Portrait | State header (inverted while recording) and a 96 px timer with a REC / PLAY / READY pill; a 72-bar waveform (recording: one peak per second up to "now"; a memo: its overview with the playhead); a 20-segment meter: the mic's input (IN, −60…0 dBFS) while recording or with NEW RECORDING selected, the playback volume (VOL, 5 % a segment) while a memo plays or is selected; GAIN / SIZE / FREE / MIC cells; the memo list with `● NEW RECORDING` first, then memos newest first (date, weekday, size, length). Memos are 16 kHz mono WAV in `SD:/Recordings/` | ES8311 ADC (mic, PGA 30 dB + 0–42 dB digital), I2S receive, ALDO2, microSD; playback through the audio path | Ready: ▲▼ memo · ● rec / play · HOLD ● delete (asks) · BOOT back. Recording: ▲▼ gain · ● stop · HOLD ● pause. Playing: ▲▼ volume · ● stop |
| `Level` | Landscape | Left: 250 px bubble level (rim = 10°, rings at 2° and 5°). Right: pitch and roll in 40 px, the low edge or corner with "raise it x mm per 10 cm" (or ✓ LEVEL), TONE / ZERO / RATE / IMU cells. Bottom: a spirit vial for the long sides (±5°). The live one, picked by how the board lies, has the inverted header; the other is dithered. Landscape because the long side is the edge you stand it on | QMI8658A accelerometer, speaker (the tone) | ▲▼ tone on/off · ● hold · HOLD ● zero / unzero · BOOT back |
| `Sleep` (power-off) | Landscape | Left: dot-matrix field (36×28 cells of 14 px) with a pixel-art frog asleep on a lily pad, `Z z` rising off it. Three tones: solid ink outline, 50 % dither body, paper belly, plus a 25 % dither shadow one cell down-right. Unlit cells are 2 px dots. Strips above and below show halt time and what to resume. Right: inverted panel with the wake time (`15:00`, RTC alarm), reason, wake path (RTC INT → PWRON), battery, alarm, and "hold PWR 1 s to boot" | Last frame drawn before `axp2101.shutdown()`; RTC alarm boots the board. On USB, PWR shows the Clock face with its weather strip instead (the desk face), and the board stays on to keep it current | None (board is off); on the desk face any key wakes |
| `Closed book` (firmware addition, power-off) | Portrait | No chrome. The open book's cover across the whole panel above a 44 px band: scaled to fill the 480×756 area when that crops at most an eighth of its width or height (centred), otherwise fitted inside on paper. A book without a cover picture gets a 3 px frame with a dithered 6 px shadow, `§ BOOK`, a dashed rule and its title in 38 px ExtraBold capitals, up to nine lines. The band: a 3 px paper rule, then ink with `HOLD [PWR] 1 s TO RESUME` in the Sleep panel's type (the key cap in a 2 px outline) and the book's progress (`24%`) on the right. Drawn with a full refresh when a book has sat open in the Reader for 15 minutes on battery (10 with the battery below 15 %); never on USB (`src/apps/closed_book.h`) | Last frame drawn before the PMIC's power-off; the next start (PWR, an alarm, USB plugged in) opens the book at its page | None (board is off); any key in the second before the power-off, PWR included, goes back to the page |

Home also lists **Files**, which has no artboard and follows Home's list language: one row per
entry (name, type tag, size), the path in the status bar. With a PIN set, it can open behind the
Lock screen's wheels unless it was used in the last minute (Lock above). The Reader (no book to
resume, or its LIBRARY) and Music (nothing playing) open the same browser confined to
`SD:/Books` or `SD:/Music`: `BOOT BACK` at that folder, no HOLD ● DELETE, no ONBOARD FLASH or
share rows, and no PIN. Without the folder, or for a book outside it, they open Files proper. The card's root starts
with an `ONBOARD FLASH` row that opens the 9.9 MB FAT partition in the same browser. A held dial
moves a page at a time in any long list. The ● verb follows the selection: OPEN, READ (EPUB,
or any file that reads as text), PLAY, VIEW (JPEG, PNG). HOLD ● deletes the selection (any entry but
`ONBOARD FLASH`) after a menu sheet asks: `// DELETE NAME.EXT?` or `// DELETE FOLDER NAME?`,
then CANCEL (selected) and DELETE, with the file's size or what the folder holds
(`47 ITEMS INSIDE`, `EMPTY`) right-aligned on the DELETE row. A folder with anything in it shows
an inverted `// DELETING NAME…` header, with the count on the right, while it goes. Neither of
the screens Files opens below has an artboard or a row on Home:

- **Picture viewer** (`src/apps/viewer.h`): the picture fitted to the whole panel (at most 2×)
  and centred on paper, portrait or landscape as the board is held (the IMU). Turning the board
  turns the picture once the new pose has held for two readings a second apart (a full
  refresh); 30–60° is neither, so it doesn't flicker near 45°, and upside down keeps the
  current orientation. Only the tilt decides, not the picture's shape: lying flat, it keeps the
  current orientation (portrait if it opened flat). The dial isn't swapped in landscape here, so
  the same roll steps the same way either way. The chrome (status bar
  `09 FILES · 03/12 name.jpg`, hints `[▲] PREV [▼] NEXT [●] HIDE [BOOT] BACK`) is drawn over the
  picture's edges, and ● hides or shows it without redrawing the picture. Counts as reading.
- **Text reader** (`src/apps/textview.h`): the Reader artboard's body with continuous scrolling
  instead of pages. `§ NAME.TXT - - - 12 KB` in place of the chapter line, the text column, a
  1 px dashed scrollbar track with a solid 5 px thumb in the right margin, and the progress strip
  as `LN 120/2400 [=====:::::] 5%`. Status tag `09 FILES`; Home's READER stays with EPUBs.
  Keys: ▲▼ scroll · ● menu (text size, font SERIF/MONO, scroll step, to start, folder) ·
  BOOT back.

**Network shares** (`src/shares.h`, from `smb =` and `nfs =` lines in `SD:/inkdeck.ini`) follow
the card root's `ONBOARD FLASH` row, a row each: the share's name, an `SMB` or `NFS` tag, no size. Opening one pushes a
Files screen for it with a `// CONNECTING…` header and the share's name and
its URL (no password) in the body while it joins Wi-Fi and mounts the share (hints: `BOOT
BACK` only). Then it lists the share like the card: the folder name in the header (the share's
name at its root), `SMB:/NAME/path/` or `NFS:/NAME/path/` in the status bar, and the same ●
verbs. If the mount fails, the body says `CAN'T CONNECT` with the reason (`WI-FI: NETWORK NOT
FOUND`, `SIGN-IN FAILED (USER OR PASSWORD?)`, `NO SUCH SHARE`, `NO SUCH EXPORT`, `ACCESS DENIED`,
`NO ANSWER FROM THE SERVER`) and ● RETRY. Shares are read-only: no HOLD ● DELETE.

All on-screen content is sample data: book text (H. G. Wells, public domain), track list, the
fictional podcast feeds, weather and sensor values.
