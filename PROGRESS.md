# Progress: INKDECK/OS UI

Building the UI specified in [`docs/ui-design.md`](docs/ui-design.md) (and its design canvas)
one phase at a time, on top of the prototype e-reader. Each phase ends flashed and checked on
the board.

## Working on the UI

- **Spec first.** Read `docs/ui-design.md` and the screen's artboard on the design canvas. The
  artboards are drawn at 1 CSS px = 1 panel pixel, so their sizes carry over directly.
- **A screen** subclasses `ui::Screen` (`src/ui/nav.h`) and lives in `src/apps/`. It draws only
  its body. The navigator draws the status bar and key-hint bar around it.
- **Building blocks** live in `src/ui/components.h`, `src/ui/type.h` (text styles) and
  `src/ui/draw.h` (rules, dithers). Add a component there before drawing it by hand in a screen.
- **Fonts:** add a face or size to `FONTS` in `tools/gen_fonts.py`, then run
  `uv run tools/gen_fonts.py`. The generated `src/ui/fonts/jbmono.*` files are committed.
- **Checking without the board in hand:** `uv run tools/screenshot.py COM4 shot.png --keys ddp`
  sends keys over USB and saves the framebuffer as a PNG. The keys are `u`/`d` dial, `p`/`P`
  press/hold, `b`/`B` BOOT/hold BOOT, `-`/`+` for volume, `r` for the PMIC registers, `i` for a raw IMU reading, `c` to reset the RTC as if the battery had been out, `o` to press PWR (Sleep, then power-off) and `z` for two minutes of standby (`src/devtools.h`). Unknown keys are ignored, so `.` pads a wait. Add `--log`
  to see render timings and errors as they happen. Turn the volume down (`--keys=------------`)
  before testing playback near people.

## Key language

Every screen follows this (from `docs/ui-design.md`, "Input mapping"). Details are in
`src/ui/nav.h`.

| Key | Meaning |
| --- | --- |
| ▲ / ▼ (dial) | Move the cursor, turn the page, or change the value on screen |
| ● | Primary action on the selection (open, play, pause, run) |
| HOLD ● (1 s) | Secondary action, if the screen has one; always listed in the hint bar |
| BOOT | Back. The screen can use it first (parent folder); otherwise it pops the screen |
| HOLD BOOT | Home from anywhere, with a full refresh. Screens never see this key |

## Phases

### ✅ 1. Navigation scaffolding

- [x] `ui::Screen` interface: tag/context for the status bar, hints, `onKey`, a 1 s `onTick`,
  `draw(body)`, enter/leave hooks, and an opt-out from chrome or portrait.
- [x] Navigator stack: `push`/`pop`/`home`/`replace`. Popping is safe from inside `onKey`.
  Global BOOT/HOLD BOOT handling.
- [x] Render loop: drains queued keys before drawing. A key a screen uses causes a partial
  refresh. Full refresh on HOLD BOOT, on orientation change, and when the flip budget is
  spent (originally every 10th partial; see Decisions). The
  status bar redraws itself when the clock minute or battery changes.
- [x] Long press is 1 s, per the spec (it was 0.8 s).
- [x] Serial dev hooks (key injection, framebuffer dump) and `tools/screenshot.py`.

### ✅ 2. Shared components

- [x] JetBrains Mono as hinted 1-bit bitmap fonts, 12 to 104 px; with Literata (phase 5),
  about 160 KB in all. They cover ASCII, Latin-1 and the UI symbols (▲▼▶▸●○✓−–—…‘’“”←↑→↓■□•).
  Big display faces carry only capitals, digits or symbols. Anything else is transliterated to
  ASCII.
- [x] Text styles that carry the canvas's letter-spacing; UTF-8 measure, draw, ellipsize,
  wrap and upper-case.
- [x] Primitives: 3 px rules, 1 px dashed rules, frames, 50 % and 25 % dithers.
- [x] Status bar: module tag, context, battery glyph and percentage (live from the PMIC; shows
  "USB" when there's no battery), and the clock (from the RTC; shows `--:--` until the time is
  set).
- [x] Section header, key-hint bar with key caps, list rows (one and two lines, ▶ selection,
  tags, trailing text) plus `ListCursor` with page-wise paging, tags and pills, progress bar,
  segment meter, button with dithered shadow, stat cell.
- [x] Added in phase 4:
  - A notice band (inverted, in place of a section header, for one-off errors).
  - Front-ellipsized path context in the status bar.
  - Named UI glyphs (`ui::glyph::kDial`, `kPress`, …).
- [ ] Still to do, each with the phase that first needs it:
  - Stroke icons on the 48-unit grid (none of the designed screens needs one yet).
  - Temperature in the status bar (8).
  - Literata (5).

### ✅ 3. Home, v1

- [x] Preview of the selected module: big index, bus tag, name, blurb, 2×2 live stats, HW
  line, `● OPEN` button. Below it the 10-row module list; the dial wraps around.
- [x] FILES opens Files at the card root. READER opens Files in `/Books`, and MUSIC opens
  Files in `/Music` (any case; the root if the folder is missing), or the player while
  something is playing. The other modules open a placeholder that shows what's planned and in
  which phase.
- [ ] The READER preview needs reading state (last book, place, library size), which comes
  with phase 5.

### ✅ 4. Files

There's no artboard, so it follows Home's list language (`src/apps/files.cpp`).
- [x] The prototype browser is now a model with no drawing (`src/dirlist.*`): folders first,
  case-insensitive, sizes looked up and cached on demand.
- [x] One 34 px row per entry: the name (EPUB/MP3 without the extension), a type tag (DIR,
  EPUB, MP3, or the extension), and a 4-character size, so the tag and size columns line up.
  20 rows per page.
- [x] The path in the status bar (front-ellipsized) and `// FOLDER` with the position in the
  header.
- [x] Hints follow the state:
  - ● OPEN, READ or PLAY, depending on the selection;
  - HOLD ● PLAYER while audio is on;
  - BOOT UP in a folder, BACK at the root.
- [x] The playing track is marked ▶ in place of its size. Errors show in an inverted notice
  band. Empty folder and no-card states, with ● to retry the mount.
- [x] BOOT goes up and restores the cursor on the folder you came from. Back from the reader
  lands on the same row.
- [x] Checked on hardware: browsing, paging (23 books, then a 686-file folder), opening a book,
  BOOT up. Drawing a page takes 16 ms, or 145 ms with 20 size look-ups in the 686-file folder,
  plus 0.6 s for the partial refresh.
- [ ] Not exercised on hardware yet: playing an MP3 from this screen (the playlist code is
  unchanged from the prototype), the notice band, and the no-card state.
- [ ] Long folders are slow to cross: 686 files is 35 pages, and the dial steps one row at a
  time. Option: let a held ▲▼ speed up to page steps, in `input` or `ListCursor`, so every list
  gets it.
- [ ] The onboard 9.9 MB FAT partition (the canvas's FILES blurb mentions it) isn't mounted
  or browsable.

### ✅ 5. Reader

`src/apps/reader.cpp` for the screens; `src/book_reader.*` for the model (book, pages,
position); `src/typeset.*` for the layout.
- [x] Literata from google/fonts (variable font; optical size follows the pixel size) at 16/26,
  19/30 (the spec's) and 23/36, each regular, bold (600), italic and bold italic. About 45 KB.
- [x] Book text keeps its typography: chapter text is stored as glyph slots instead of ASCII,
  so curly quotes, dashes, ellipses and Latin-1 letters come through. Titles, authors and TOC
  labels are UTF-8.
- [x] Typesetter:
  - text sits on a fixed line grid with capitals centred in the line;
  - justified lines; first-line indents, but none after a heading;
  - half a line of space around centred headings;
  - an inverted 84 px drop cap (JetBrains Mono 68 ExtraBold) on each chapter's first long
    paragraph, with the text flowing round it; the page breaks early if the box won't fit.
- [x] Chrome as on the artboard:
  - the book title in the status bar;
  - `§ CHAPTER ---- AUTHOR`, with the chapter title from the TOC (roman numeral without one);
  - `P. 3/23 [bar] 3%`: page within the chapter, whole-book progress weighted by chapter
    size;
  - hints ▲ PREV, ▼ NEXT, ● MENU, BOOT BACK.
- [x] Table of contents parsed from the EPUB 2 NCX, or else the EPUB 3 nav document.
- [x] ● opens a **menu sheet** (`ui::MenuScreen`, a new reusable overlay that slides up over
  the screen below):
  - RESUME;
  - CONTENTS (a list to jump to any entry);
  - TEXT SIZE (re-lays the page live under the sheet);
  - LIBRARY (back to Files, or Files in the book's folder).
- [x] The position is saved per book in NVS: when leaving the reader, every 10 page turns, and
  on chapter jumps or size changes. Skipped when unchanged. READER on Home resumes the last
  book (Files in `/Books` if there isn't one). The Home preview shows LAST, PLACE and FONT.
- [x] Checked on hardware:
  - opening a book (0.16 s); paging through the front matter;
  - the drop cap; menu, contents jump and live text size;
  - resume from Home, including after the board lost power completely.
  - A 19 KB chapter loads in 170 ms and paginates in 33 ms; a page draws in about 60 ms.
- [ ] No hyphenation, so some justified lines run loose. Many EPUBs carry soft hyphens, which
  would be cheap line-break points (they're dropped today).
- [ ] No kerning. Images show as `[image]` until phase 5b. Only the last position per book
  is kept; no bookmarks yet.
- [ ] Page numbers count within the chapter. Whole-book numbers would mean paginating every
  chapter up front.
- [ ] Every spine item can get a drop cap, front matter included (the copyright notice had
  one). Limiting them to TOC chapters is an easy tweak if it bothers you.

### 🔶 5b. Reader: images and cover art

Started 2026-11-12: pictures in the text work (`picture.*`, `book_pictures.*`). Done so far:
- [x] Decoders: bitbank2's JPEGDEC 1.8.5 (8-bit grey output, its free 1/2-1/8 scaling,
  progressive files at 1/8 from their DC scan) and PNGdec 1.1.6. Both are Apache-2.0.
  PNGdec's line buffer is raised to 2400 px RGBA with a build flag (the default fits 320 px).
  Its ESP32-S3 SIMD file includes an esp-dsp header the assembler can't resolve, so
  `compat/dsps_fft2r_platform.h` stands in. Work buffers are in PSRAM.
- [x] Parsing: `<img src alt>` and the `<svg><image href>` wrapper covers use become a
  `flow::kImage` paragraph pointing into the chapter's picture list (`Epub::Image`). GIF,
  SVG files and broken pictures take one line: `[alt text]`, or `[image]`.
- [x] Layout: a centred block with half a line of space around it, and the text after it
  back on the line grid. A picture shrinks to fit the column and page, and grows up to 2x
  only if it's already a good part of the column (ornaments keep their size). If it doesn't
  fit what's left of a page, it shrinks into it when it keeps at least half its height,
  otherwise it starts the next page.
- [x] Rendering: area-average scaling, a 1st-99th percentile contrast stretch with a gamma
  of 0.8 (dithered pictures read dark on the panel), then Atkinson dithering, which keeps
  line art and small print crisp. The last 3 bitmaps of the chapter are cached and copied
  straight into the framebuffer.
- [x] Checked on hardware with *All Systems Red*: the cover (a 500×800 JPEG drawn at 362×579
  under the chapter's "Cover" heading) and the title page, itself a picture of typeset
  text, which stays legible. The first draw takes 1.4 s for the cover (decode 680 ms,
  scale + dither 349 ms) and 0.7 s for the title page (decode 154 ms); redraws from the
  cache 145 ms. JPEG decode time depends on the file, not just its size.
- [x] **Cover thumbnail on Home** (`covers.*`): the READER preview shows the last book's
  cover (about 70 × 110 px) framed in 2 px, with a dithered shadow like the OPEN button's
  (not on the canvas, which has no spot for one). The stats move right of it. The cover is
  found from the OPF: the EPUB 3 `cover-image` property, else EPUB 2's
  `<meta name="cover">`, else an image named "cover" (`Epub::coverPath()`). It's rendered
  once per book file and box, and cached as 1-bit on the card in
  `/sdcard/.inkdeck/covers/` (Files hides dot-folders). Books without a usable cover are
  cached too, so they aren't retried. The last one is also kept in RAM. Checked on hardware:
  Home draws in 77 ms at boot with the cached cover (61 ms without one).
- [ ] Headings that books hide with CSS ("Cover", "Title Page") still show; CSS is ignored.

Still to do:
- [x] The cover as the first page for books that name a cover picture but don't open with a
  page showing it (`Reader::addCoverPage()`): the cover goes on a page of its own before the
  first chapter's text, using a new `flow::kPageBreak`. A first chapter that already shows the
  cover, or is a picture with little text (a cover page using another file), is left alone.
  Positions saved in that first chapter before this change can land a page early. Checked
  only that *Consider Phlebas*, which has its own cover page, gets no second one; not yet
  seen on a book that needs it (testing stopped at the user's request).
- [ ] Maybe: thumbnails in Files rows; GIF pictures; the panel's 4-grey mode for picture pages
  (about 3 s refresh); faster JPEG decode for heavy files (680 ms for this cover).
- The same decoder can show embedded MP3 cover art (ID3 `APIC`) later; the Music artboard has
  no spot for it yet.

### ✅ 6. Music

`src/apps/music.cpp`. The prototype player screen and the FreeSans fonts are gone.
- [x] The canvas layout, top to bottom:
  - `// NOW PLAYING` with `TRK n/N`;
  - the title upper-cased at 50 px (two lines), dropping to 38 px for long titles, then the
    artist;
  - the waveform overview with a playhead;
  - `elapsed / length` and `−remaining`;
  - the play-state pill and a 10-segment volume meter;
  - the 4-row queue (current track inverted, then the next three, with artist and length);
  - the signal chain:
    - CODEC: sample rate;
    - FILE: bitrate and VBR;
    - AMP: on during a playback session;
    - RAIL: ALDO1, read from PMIC register 0x90, checked in the TG28 datasheet §6.13.2.75.
- [x] **Waveform:** 72 points, each decoded from a few frames at that spot in the file
  (`mp3info::waveform`). Frames only count where two valid headers line up, which skips false
  syncs. This ran in a background task (`trackinfo`, core 0, below the audio task), and the
  screen redraws when results arrive. It took 2.5 s for a 154 MB podcast and 5.7 s for the
  next one with playback running.
- [x] **Track info:**
  - Tags are now UTF-8.
  - Queue rows get tags and lengths from a probe (about 30–50 ms per file).
  - Lengths are accurate for VBR files, from the Xing/Info/VBRI frame count.
- [x] Keys: ▲▼ volume ±5, ● pause/play (from stopped at the end of the queue it replays the
  last track), HOLD ● skip. HOLD BOOT means home, so there's no stop; a paused player keeps
  the amp on.
- [x] Fixed a prototype bug: skipping while paused started the next track with the DAC still
  muted. The unmute is in, but I haven't listened to it.
- [x] Checked on hardware, muted with the new `-`/`+` dev keys: play from Files, waveform and
  queue fill in, pause, skip, the title falling back to 38 px.
- [x] The time and playhead redraw every 10 s while playing (`kLiveRedrawMs`). With a full
  refresh every 10th partial, that meant a full-screen flash about every 100 s. Phase 8's
  flip budget (see Decisions) now counts the pixels changed rather than the updates, so these
  small redraws rarely trigger one.
- [ ] Not on the canvas, not built: repeat and shuffle (the canvas shows REPEAT ALL / SHUF OFF
  pills), a whole-queue duration, cover art (see 5b).

### ✅ 7. System

- [x] `apps/system.cpp`, laid out as on the canvas. All values are live, and the screen only reads:
  - **Battery:** the gauge percentage in 96 px; the battery voltage from the PMIC ADC;
    the 10-cell gauge, with a part-full cell dithered.
  - **Pill:** where power comes from (ON USB / ON BATTERY).
  - **Charger line:** the settings the PMIC holds now (charge current, target voltage, cut-off).
  - **Rails:** DC1 and ALDO1–3, on/off state and voltage.
  - **State rows:** next RTC alarm, boot reason (the ESP32 reset reason; for a cold start,
    the PMIC's power-on source and its last power-off source), the Wi-Fi MAC, OTA slots,
    free heap and PSRAM. The section header shows uptime.
  - Redraws when the battery, power source or uptime minute changes.
- [x] Not on the canvas: the TG28 has no battery-current ADC, so the canvas's "−62 mA" became
  the charger state (CHARGING CC/CV, DISCHARGING, FULL, IDLE).
- [x] Actions:
  - REBOOT asks first in a sheet, shows "REBOOTING…", then restarts. Checked: the next boot
    reports SOFTWARE RESET.
  - SLEEP NOW and CHECK FOR UPDATE say they aren't built yet, in place of the section
    header. SYNC TIME works (phase 9).
- [x] Home's SYSTEM row opens it. Its preview shows battery, power, uptime and clock state.
- [x] New dev key `r`: prints the PMIC's configuration and status registers.
- [x] Fonts: 96 px digits and a 22 px face. The digit-only faces now also carry `%+-.`.
- [x] **PMIC configuration** (`pmic::configure()`, every boot; agreed 2026-11-12). The registers
  this board runs with were dumped and compared with Waveshare's `axp_init()`
  (`references/power.md` §2). Only five settings are written, each read back:
  - PWR key: 1 s to power on, 4 s to force off.
  - ADC: USB and system voltage on (shown under the charger line).
  - Charging for the 1500 mAh cell: precharge 50 mA, constant current 500 mA (Waveshare
    uses 200 mA), stop at 25 mA.
  - Deliberately not copied from Waveshare: ALDO3 stays at the chip's 3.0 V (3.3 V is the
    panel's maximum), and backup-cell charging stays off (unsafe with a primary coin cell).
    Rails, cut-off and input limits keep the chip's defaults, which already match.
- [ ] Chip temperature: the TG28 die-temperature ADC is off by default, and its scaling isn't
  in the datasheet.

### ✅ 8. Sensors

- [x] Drivers, written from the datasheets (`references/sensors.md` lists the vendor bugs
  avoided):
  - `shtc3`: ID checked, CRC on every word, sleeps between readings.
  - `qmi8658`: soft reset; ±4 g and ±256 °/s at 56 Hz with the low-pass filters on;
    little-endian data (confirmed: about 1 g on one axis at rest).
- [x] **The IMU is at 0x6B, not 0x6A** (verified on hardware: the bus scan found 0x6B, and
  reads at 0x6A NACK). Fixed in `board_pins.h`, the design doc and the skill reference.
- [x] `sensor_hub`, the service layer:
  - A background task samples temperature/humidity every 5 s and keeps half-hour averages
    for the 24 h sparkline. They're in RAM, so the history restarts at each boot.
  - It runs the I2C bus scan and the CSV log.
  - Gyro zero-rate offset: this chip read −4 °/s on X at rest, within the datasheet's ±10.
    The hub learns the offset while the board is still and subtracts it.
  - IMU readings come out in screen axes.
  - (Named `sensor_hub`, not `sensors`, so `apps/sensors.h` doesn't shadow it.)
- [x] `apps/sensors.cpp`, laid out as on the canvas: ENV, IMU (bubble level, accel/gyro table,
  tilt), PMIC and the `i2cdetect` grid with device names. Status bar: `I2C · 5/5 ACK`.
  - ▲▼ PANEL picks the panel that updates live, shown with an inverted header: ENV every 5 s,
    IMU every 1 s, PMIC and BUS every 5 s. The others refresh once a minute. A refresh
    redraws only when a value really moved; deadbands keep sensor noise from redrawing.
  - ● holds or resumes all readings.
  - HOLD ● starts or stops the CSV log, `SD:/logs/sensors.csv`. It writes a row every 10 s
    of every reading (raw chip axes) and keeps going after leaving the screen. Checked: two
    rows in 25 s.
  - Not on the canvas: the PMIC panel's IBAT and DIE cells became USB voltage and charger
    state. The TG28 has no battery-current ADC, and its die temperature isn't enabled.
- [x] Home: the SENSORS row opens it, the preview shows temp, humidity, tilt and bus, and
  Home's status bar shows the temperature (`Screen::statusTemp()`).
- [x] Dev key `i`: one raw IMU reading in chip axes.
- [x] **IMU axes in portrait** (pose tests with the board in hand, 2026-11-12): screen right
  = chip −Y, up = chip −X, out of the screen = chip −Z (`kScreenAxes` in `sensor_hub.cpp`).
  Lying flat it reads about +4.5° on both axes. That's the desk or the accelerometer's
  factory offset (the datasheet allows ±100 mg, about ±6°). A zero button would belong to
  the Level app (phase 10).
- [x] **Temperature calibrated:** the SHTC3 read 25.9 °C and 33 % against a 22.7 °C room,
  with the board on USB, charging. `sensor_hub` subtracts 3.2 °C and recomputes humidity for
  the cooler room (same water vapour), which gives about 40 % against the room's 37 %
  (household hygrometers are ±3–5 %). The board's heat depends on charging and load, so on
  battery it probably reads a little low.
- [x] Display units are a build flag: `-DINKDECK_FAHRENHEIT=1` in `platformio.ini` (set, at
  the user's request) shows °F; unset or 0 shows °C (`src/units.h`). The CSV log stays in
  °C.

### 🔶 9. Clock, power-off and the network (was phases 9, 10 and 11)

Combined on 2026-11-12 because the three were planned around one mechanism: the RTC alarm
boots the board, both for the user's alarms and for the hourly weather sync, and the Sleep
screen shows that wake plan. In the end the weather sync runs only while the board is on
(9d). Built in this order, each step flashed and checked on the board.

**✅ 9a. Clock (landscape)** (`apps/clock.cpp`)
- [x] Landscape chrome: the navigator already switched orientation (full refresh on the
  change); the status and hint bars just draw at 800 px. New: a screen can drop the status-bar
  clock (`Screen::statusClock()`; the Clock does, since it is one), and every status bar shows
  `WIFI` while the radio is on.
- [x] Laid out as on the artboard: HH:MM in JetBrains Mono ExtraBold 172 px (176 px overflows
  the GFX glyph format's 8-bit y offset; the difference doesn't show), weekday and date, ISO
  week and day of the year, the 24-cell day bar (past hours solid, the current one dithered)
  with the day's progress and sunrise/sunset, the month calendar (Monday first, today
  inverted, always six rows tall so the alarms don't move), the alarm list with the next
  ring in its header. The status bar says when NTP last set the clock (`timesync::lastSync()`,
  kept in NVS). Redraws once a minute; a full refresh comes every 30 minutes (flip budget).
- [x] Alarms (`alarms.*`): three slots in NVS, each a local time (so daylight saving moves
  with it), a repeat (once, every day, Mon–Fri, Sat–Sun or one weekday) and on/off. ▲▼ picks
  one, ● turns it on or off, HOLD ● edits it.
- [x] Not on the canvas: the editor, a panel over the right column. The hour is set first
  (inverted), then minutes, repeat and on/off: ▲▼ sets, ● moves to the next field, HOLD ●
  saves, BOOT cancels. It shows when the alarm will next ring. Opening it turns the alarm on.
- [x] Sunrise and sunset computed on the board (`suntimes.*`, the Almanac for Computers
  method; checked against NOAA for Chicago and London) from `LATITUDE`/`LONGITUDE` in `.env`
  (shared with Weather). Without them the day bar shows no sun times and Home's preview says
  to set them. Set in this board's `.env` since 2026-11-12.
- [x] Home's CLOCK row opens it; the preview shows the time, next alarm, sun times and the
  last NTP sync.
- [x] Checked on hardware: layout, the minute redraw, editing and saving an alarm, ● on/off,
  the alarms surviving a reboot (NVS), WK 46 · DAY 316 for 2026-11-12 (as on the canvas), the
  sync time after SYNC TIME.
- [x] Setting the RTC: System → SYNC TIME (NTP). A manual set-time screen would still help
  without Wi-Fi.
- [x] Time zone: `TIMEZONE` in `.env`, a POSIX TZ string with the DST rule (US Central here),
  or `timezone` in `SD:/inkdeck.ini` (below).

**✅ 9b. Alarms that ring**
- [x] `apps::pollAlarms()`, called from `loop()` about once a second, checks each window
  (last check, now] so a ring is never missed or repeated while the UI is busy. A clock that
  jumps more than 10 minutes (NTP, first set) doesn't ring for what it skipped.
- [x] The ring sheet: an overlay at the bottom of whatever screen is showing (portrait or
  landscape), inverted, with the time in 96 px and the snooze time. ● or BOOT stops, HOLD ●
  snoozes 9 minutes (kept in NVS, so a power-off can wake for it). The tone stops by itself
  after 2 minutes; the sheet then says MISSED until a key.
- [x] The tone (`audio::ring()`): 2 kHz, four 120 ms beeps and a 600 ms pause, with 5 ms ramps
  against clicks, generated in the audio task at the music volume but at least 50 %. Music that
  was playing is paused, and stays paused.
- [x] A one-off alarm turns itself off once it rings.
- [x] Checked on hardware: alarm 3 set to 16:38 rang at 16:38:00 on the Clock, the sheet
  appeared, ● stopped it and the alarm switched itself off. Snooze isn't checked on hardware
  yet.
- [x] While the board is on, alarms are checked in software and the RTC's alarm interrupt
  stays off. With it on, INT holds PWRON low until the flag is cleared, and 4 s of that
  forces the PMIC off (`references/power.md` §7, pitfall 1).

**✅ 9c. Power-off: Sleep and standby** (`power.*`, `apps/sleep.cpp`, standby in
`apps/clock.cpp`)
- [x] **PMIC write agreed 2026-11-12:** the soft power-off, register 0x10 bit 0 (datasheet
  §6.5.4.3; what XPowersLib's `shutdown()` does), `pmic::powerOff()`. Every rail but RTCLDO
  goes off, the ESP32 included; the panel keeps its image.
- [x] **Sleep** (the frog), laid out as on the artboard: the halt time, the dot-matrix field
  (36 × 28 cells: ink, 50 % dither body, paper belly, 25 % dither shadow, 2 px dots), RESUME
  with the last book, and the inverted wake panel (80 px wake time, reason, path, battery and
  voltage, the alarm). Entry: System → SLEEP NOW, or a short press of PWR from any screen.
  It wakes only for the next alarm or snooze; with none it says NO WAKE SET and only PWR
  brings it back.
- [x] **Standby** (agreed with the user: idle auto-off on battery should keep the clock
  current): after 10 minutes without a key, on battery only (not on USB), with nothing
  playing or syncing, the board shows the Clock face (no cursor, "STANDBY", hint "PWR hold 1 s
  to wake") and powers off. The RTC wakes it every minute: `apps::standbyWake()` at the start
  of `setup()` redraws the face and powers off again, with no radio, audio, SD or sensors.
  The face is drawn from a small record in NVS (time, battery, alarms, last sync), so the
  previous frame can be redrawn as the panel's "old" image and the update is a partial
  refresh; a full one at :00 and :30 clears the ghosting. An alarm due at that wake boots
  normally instead, and rings.
- [x] The PWR key needs no PMIC change: GPIO1 (`PWR_KEY_SENSE`) reads HIGH while PWRON is
  low, so `input` treats it as a key (`Key::Power`, released within 2 s; longer is someone
  forcing the board off). A press already held at boot (PWR held to power on) is ignored
  until released. The navigator sends it to `apps::sleepNow()`; screens never see it.
- [x] Power-off order (`power::off()`): stop audio (with a pause against a speaker pop, only
  if it was on), close the sensor CSV, Wi-Fi off, arm the RTC (moved on a minute if the
  target is less than 2 s away, since a missed alarm only fires next month), record the mode
  in NVS, PMIC off. Both screens wait one second after drawing, so a key can still cancel.
- [x] Boot (`power::begin()`, right after `rtc::begin()`): reads the alarm flag, then
  disarms the RTC (AIE off, AF cleared) before anything slow, so INT can't hold PWRON low
  into the 4 s forced-off. The wake reason comes from the alarm flag, the ESP32 reset reason
  and PMIC 0x20. An alarm wake opens the Clock and rings over it (the alarm window starts at
  the armed time, since booting takes a couple of seconds). The 500 ms wait for the USB
  console is skipped on RTC wakes.
- [x] System: SLEEP NOW works; the SLEEP row states the auto-off rule; NEXT WAKE shows the
  wake a power-off would arm (the RTC is armed only on the way off).
- [x] Dev keys: `o` presses PWR; `z` enters standby for two wakes, then the board stays on,
  so the USB console comes back. `tools/screenshot.py --after b` grabs the Sleep screen in
  its last second, then cancels the power-off.
- [x] Checked on hardware, on USB with the battery in:
  - Standby: off at 23:00:41; the RTC woke it at 23:01:00; `setup()` began at 13 ms, the
    partial refresh (5,822 px) was done at 0.83 s, then off again; the next wake booted
    normally (dev-tool limit).
  - Sleep with alarm 3 set three minutes ahead: off at 23:13; the RTC woke it at 23:16:00,
    Home, then the Clock with the ring sheet 2.7 s after power-on; ● stopped it.
  - PMIC 0x20 read `01` (PWRON) after an RTC wake. The soft power-off works with USB
    attached; the board stays off until the alarm.
  - Not checked yet: waking with the PWR key and the boot ignoring that press (needs a hand
    on the board), and the idle auto-off (needs the board off USB).
- [ ] Battery life in standby isn't measured. Each wake is about 1 s awake plus a 0.6 s
  partial refresh, much like the vendor's clock mode (which claims 15 days on the 1500 mAh
  cell).

**9c+. Reading and music (the user's rules, 2026-11-12)** (`apps/idle.*`, `power::nap()`)
- [x] Reading (the Reader open, its menu included) and playing music never power off on
  idle until the battery is below 15 % (`power::kLowBatteryPercent`). Paused music counts as
  idle. Below 15 % any idle power-off goes to Sleep (no minute wakes) instead of standby.
- [x] Reading on battery light-sleeps as soon as the page is drawn (`Screen::reading()`; it
  first waited 3 s, which wasn't needed: a held button or a queued key keeps it awake): the CPUs
  stop and RAM, PSRAM and peripherals keep their state, until a button (dial, press, BOOT, PWR,
  as GPIO wake sources) or the next minute boundary, for alarms. The IMU goes to standby while
  napping, and the Reader saves its place when a nap starts (`Screen::onNap()`). Nothing
  redraws while napping, so the status-bar clock updates with the next page turn. The System
  screen's SLEEP row shows "napped N min · M naps" once it has happened.
- [x] Checked on hardware by the user: on battery, every page turn came instantly out of
  light sleep. The wake pins are kept in their awake configuration during sleep
  (`gpio_sleep_sel_dis`); before that, a BOOT press seemed not to wake it.
- [x] **USB after light sleep is unreliable.** Each nap drops the USB connection. Twice the
  board's USB device then stayed dead after USB was plugged back in, until a reset (once a
  watchdog reset): after a test mode that napped on USB in 5 s cycles, and after one battery
  session. Another battery session (7 naps) came back by itself. So the board naps only on
  battery, and once it has napped, USB power with no host talking for 3 s
  (`usb_serial_jtag_is_connected()`) makes it restart cleanly, the Reader's place saved
  first (`restartIfUsbStuck()` in `apps/idle.cpp`). On a charger with no computer that's one
  restart. The restart path itself hasn't been triggered on hardware yet.
- Testing note: light sleep can't be watched over USB. The System screen's nap count is the
  check; plug in afterwards.
- Not measured: the current while napping. The rails (audio codec, mic, panel) stay up;
  switching ALDO1/ALDO2 off while reading would save more but is a PMIC write.

**✅ 9d. Weather** (`forecast.*`, `apps/weather.cpp`, `ui/icons.*`)
- [x] Open-Meteo (open-meteo.com, no API key, free for non-commercial use): one HTTPS request
  (`esp_http_client` with the core's certificate bundle, parsed with cJSON) for the current
  conditions, 48 hours and 7 days. The location comes from `.env`, rounded to two decimals
  (about 1 km) in the request. Values stay in SI. `INKDECK_FAHRENHEIT` now means US units:
  °F, plus mph and inches (`units.h`). The file is `forecast.h`, since a `weather.h` in `src/`
  is shadowed by `apps/weather.h` for includes from `apps/`.
- [x] Laid out as on the artboard: NOW (UPDATED and NEXT sync times, the temperature in
  JetBrains Mono ExtraBold 130 px with an icon, the condition in 28 px, FEELS / WIND / RH /
  the next sunrise or sunset), NEXT 12 H (temperature, rain-chance bars, hour), 6 DAYS
  (inverted tag on the cursor's day, icon, condition, lo-hi range bars on a shared scale).
  The status bar's context reads `LOC:HOME`, or the sync state. The 6-day temperature columns
  are 6 px wider than on the canvas, so "−12°" and "102°" fit.
- [x] Icons (`ui/icons.*`): the canvas's sun, cloud, partly cloudy, rain and showers, drawn
  from its 48-unit paths at any size. Moon, fog, snow and storm were drawn to match. At night
  the NOW icon shows the moon for clear and partly cloudy skies.
- [x] Not on the canvas: ▲▼ moves a day cursor and ● opens a sheet for that day: sky,
  high/low, rain chance and amount, wind, sunrise and sunset, daylight, and that day's hours
  every 3 h (as far as the 48 hours reach). ▲▼ steps through the days inside it; ● or BOOT
  closes it.
- [x] Cache: the last forecast is kept in NVS (about 2 KB, namespace `weather`), so the screen
  opens at once and works offline. Once the current conditions are over an hour old, the
  forecast for the present hour stands in for them.
- [x] Sync: HOLD ● syncs now. While the board is on, `weather::poll()` (from `loop()`) syncs
  at the top of the hour after the last forecast. It retries 15 minutes after a failure and
  skips automatic syncs on battery below 15 %. Power-off and reading naps wait for a running
  sync. `wlan` is shared now: each connect has a matching disconnect, and the radio goes off
  after the last user, so a time sync and a weather sync can overlap.
- [x] Home: the WEATHER row opens it; the preview shows now, today's low and high, the
  likeliest rain in the next 12 hours, and when the forecast was fetched. It redraws when a
  background sync starts or ends.
- [x] Checked on hardware (2026-11-12): the boot sync (Wi-Fi join plus fetch in 4.6 s, a
  3.7 KB reply, 48 hours and 7 days), the layout with real data, the day cursor, the day
  sheet, the forecast surviving a reboot from NVS, and Home's preview. The first attempt
  failed with "HTTP_CLIENT: Out of buffer": the request line (about 600 bytes) didn't fit
  `esp_http_client`'s default 512-byte send buffer, so it's 1 KB now.
- Not seen yet: the hourly sync firing at the top of the hour, a failed sync's message with
  the network down, and the icons other than moon, cloud and partly cloudy (all ten checked
  in an offline render of the same paths).
- [x] No RTC wake for weather while off (the user's choice, 2026-11-12). Neither the Sleep
  screen nor the standby face shows weather, so it would only pre-fetch for the next time
  Weather opens, at about 6 s of Wi-Fi (roughly 100 mA) a wake. Power-off stays radio-free;
  a stale forecast syncs at the next boot.
- [x] The room (the user's request, 2026-11-13): the status bar shows the SHTC3's reading in
  whole degrees (`IN 73°F`). With no forecast for the present hour (none yet, too old, no
  Wi-Fi, clock unset) the NOW panel shows the room instead: `// INSIDE (NO FORECAST)`, the
  temperature in 130 px, the reason in the condition's place, and the room's humidity under RH.
  Checked with dev command `!wxforget` (drops the saved forecast): the room showed while the
  re-sync ran, then the forecast.
- [x] The NOW icon picks day or night from the sun times worked out on the board; the
  forecast's own flag is for the start of its hour, so at 07:47 it still showed the moon.
- [x] **Fixed: a stale Weather screen** (the user saw it on the morning of 2026-11-13). Two
  causes. The navigator called `onTick()` only for the top screen, so Weather under its day
  sheet never reloaded after a sync; the screen under an overlay now ticks too. And while the
  player was loaded, internal RAM was too short for Wi-Fi to start, so the hourly syncs
  failed (see "C++ allocations prefer PSRAM" under Decisions).

**🔶 9e. Podcasts** (`podcast_library.*`, `rss.*`, `netget.*`, `apps/podcasts.cpp`)
- [x] Shows come from two places: RSS feeds listed in `SD:/Podcasts/feeds.txt` (one URL per
  line, `#` comments; the screen creates the file with an explanation if it's missing), and
  any other folder of MP3s in `SD:/Podcasts/` (the user already keeps one there, 686 episodes
  named `NN - Title.mp3`). A folder's episodes are its files in name order, with the number
  and title taken from the name; the card shows the file's own title tag and length once
  `trackinfo` has read them.
- [x] Feeds: a refresh reads each feed only until it has its 30 newest items (`rss.*`, a
  forgiving scanner: CDATA, entities, HTML descriptions to plain text, RFC 822 dates, iTunes
  durations; tested on the PC against a 2 MB NPR feed: 30 items from its first 400 KB). On the
  board: 30 episodes from 136 KB in 1.5 s. The parsed feed is cached on the card, so the screen
  opens offline. Opening the screen refreshes feeds older than 3 hours; HOLD ● on the show
  list refreshes now.
- [x] Downloads (● GET) go to `SD:/Podcasts/<show title>/<date> <title>.mp3` through
  `net::Get`: a streaming HTTPS GET that follows the tracking redirects podcast links use.
  Progress shows in the row (`[DL 62%]`), the card and the status bar; several can queue
  (`[QUEUED]`); HOLD ● cancels. About 275 KB/s (42 MB in 2.5 min). The `.part` file's size is
  committed every megabyte, so a download cut off by a reset resumes: verified, a flash at
  6,144 KB, then HTTP 206 for exactly the remaining 8,211 KB. Resumes send `If-Range` with the
  file's ETag (kept in `.part.id`): hosts that stitch in ads can serve a different file each
  time, and a plain range request would splice two (one test download before this fix got
  7.5 MB where 5.9 MB were left). Downloads check free space; nothing is ever deleted
  automatically (the artboard's 1 GB budget would mean deleting the user's own files).
- [x] Playing (● PLAY / RESUME) goes through the music player from the saved position
  (`audio::play()` takes a start time and jumps by bitrate: exact for constant-bitrate files).
  A folder show plays on into the following files. `podcasts::poll()` saves the position
  every 30 s and on pause, and marks an episode played in its last 45 s. State per show lives
  on the card in `SD:/.inkdeck/podcasts/`. Verified: played 1:00, switched episodes, came back
  to 1:00; after a reboot it resumed at 1:21.
- [x] States as on the artboard: `[NEW]` (a feed episode from the last 7 days, untouched),
  `[DL 62%]`, `[▶ 23m]`, `[✓]`, plus `[SD]` (downloaded), `[51%]` (partial), `[QUEUED]`.
  HOLD ● on an episode marks it played or unplayed. The SD CACHE bar is the podcasts' share of
  the space they could use (theirs plus free), labelled with their size.
- [x] Two levels of focus, since the artboard has two lists: the show list first (● opens a
  show, with the cursor on the episode last listened to), then its episodes (BOOT goes back
  to the shows).
- [x] Home: the PODCASTS row opens it; the preview shows shows, new episodes, network
  activity and the space used.
- Not checked yet: marking played at the end of an episode, cancel, a non-MP3 feed, a full
  card, a feed that fails.
- Test data left on the card: the NPR *Up First* feed in `feeds.txt` (dev command `!feed`)
  and four of its episodes, about 80 MB, in `SD:/Podcasts/Up First from NPR/`. The *Colom*
  episode there predates the `If-Range` fix and may have a splice.

**Playback from anywhere (the user's request, 2026-11-13)**
- [x] While music or a podcast is playing or paused, the status bar shows an inverted ▶ or
  ‖ before the battery, on every screen.
- [x] HOLD BOOT then opens the player instead of going home (`apps::openPlayer()`, through
  `nav::onHoldBack()`); menus and sheets close first. HOLD BOOT on a player opened that way
  goes back to where it came from, like switching back to the last app. On a player reached
  the usual way (Home's MUSIC row, ● PLAY), HOLD BOOT still goes home. Verified on the board:
  Podcasts → HOLD BOOT → player → HOLD BOOT → the same Podcasts list.
- [x] `audio::play()` keeps at most 64 tracks from the chosen one (a folder of 686 made a
  685-path queue).

**Settings on the card (the user's idea, 2026-11-13)** (`settings.*`)
- [x] `SD:/inkdeck.ini` overrides the time zone and the location built in from `.env`:
  `timezone` (POSIX TZ), `latitude` and `longitude` (both or neither), in any `[section]`, `;`
  or `#` comments. Values are checked the way `gen_secrets.py` checks `.env`; a bad one is
  logged and the built-in value stays. Wi-Fi keys are refused with a log line: a card is easy to
  lift, so the network stays in the firmware.
- [x] Read at boot right after the card mounts (before the first frame). A missing file is
  written with every line commented out and the common US and European zones listed. The
  values last read from the card are kept in NVS, so standby wakes (which don't mount the card)
  and boots without a card keep them; a card whose file doesn't set a value puts the built-in
  one back.
- [x] A saved forecast for another location isn't shown (`Forecast` now records where it's
  for), so moving the location syncs a new one at once.
- [x] System: a SETTINGS row says where they come from (`.env (built in)` or
  `SD:/inkdeck.ini · TZ, LOCATION`); the state rows are 21 px now to fit a seventh.
- [x] Checked on the board with dev commands `!append`, `!cat` and `!rm`: a Pacific zone and a
  Greenwich location from the card (the clock showed Pacific time, the forecast re-synced for
  Greenwich, the Wi-Fi line was refused), then the file removed: a clean template, back to
  Central time.
- [x] **Found on the way: an SD write straight from flash loses its sectors.** The first
  template came back with its first 512 bytes zeroed. `!sdtest` showed why: 1 KB written from a
  string constant in one call reached the card as zeros, while internal RAM and PSRAM were
  exact. The SDMMC driver DMAs whole sectors from the caller's buffer. Both templates are now
  written from RAM (AGENTS.md, Gotchas).

**Network groundwork (done)**
- [x] Credentials are compiled in from the gitignored `.env` (`scripts/gen_secrets.py`, after
  lollipop-smell), with the time zone. On-device provisioning (`references/network.md` §3)
  may not be needed while the network comes from `.env`.
- [x] `wlan` joins on demand and turns the radio off afterwards. The System screen's WIFI row
  shows off / connecting / on with RSSI.
- [x] SYNC TIME (`timesync`, its own task): join, SNTP (pool.ntp.org, Cloudflare, Google via
  `esp_netif_sntp`), write the RTC on a second boundary, Wi-Fi off. The notice reports the
  phase, then "TIME SET · 16:08 CST", plus how far off the clock was if it was already set.
  Verified 2026-11-12: joined in 2–4 s, NTP answered in about 2 s, a second sync reported +0.014
  s ("WAS RIGHT"), and after a reboot the RTC gave the right local time. The first Wi-Fi use
  costs about 50 KB of heap that stays allocated; repeat syncs don't grow it.
- [x] Boot-time sync: if the RTC comes up unset (first boot, battery out), `main.cpp` starts
  one sync after the first frame. Verified with dev key `c` + reboot: started at 2.9 s,
  joined at 5.8 s, RTC set at 7.0 s. One attempt; after a failure, SYNC TIME retries by hand.
  The System screen follows a sync that's already running when it opens.
- Wi-Fi is on only while something needs it (a sync takes about 5 s), so the WIFI row
  normally reads "off". The radio draws roughly 100 mA, so the network screens should keep
  sessions short too.

### ⬜ 10. Recorder, Level

Listed on Home but not designed yet.

## Decisions

- **HOLD BOOT means home**, so the prototype's HOLD BOOT actions moved: the reader's text
  size is in its ● menu, and the player's stop was dropped (● pauses). The one exception
  (2026-11-13): while audio is playing or paused, HOLD BOOT opens the player, and HOLD BOOT
  there goes back.
- **C++ allocations prefer PSRAM** (`src/psram_new.cpp`, 2026-11-13). ESP-IDF keeps every
  `malloc()` under 4 KB in internal RAM, and our strings are small and many: playing from a
  686-episode folder took internal RAM from 203 KB to 55 KB free, and Wi-Fi then failed to
  start (`ESP_ERR_NO_MEM`). With `new` and `delete` going to PSRAM first, 209 KB stays free
  while playing. Anything that must be internal (DMA buffers, task stacks, data an IRAM
  interrupt touches) is allocated with `heap_caps_malloc()`. TLS buffers go to PSRAM too
  (`net::begin()`): the core's mbedTLS otherwise allocates about 40 KB of internal RAM per
  connection. Dev key `h` prints the heap.
- **Lists page whole screens** instead of scrolling a row at a time. That's fewer, cleaner
  e-paper updates.
- **Screen changes use a partial refresh.** Menus are overlays, so opening one only redraws
  the sheet area.
- **Full refreshes come from a flip budget** (`screen.cpp`, since phase 8), not every 10th
  update. Ghosting builds up with the pixels a partial refresh flips, so a full refresh comes
  once the flips add up to two whole screens (768,000 px) or after 30 partials. A screen
  change flips 25–40k pixels, so that's still roughly every 10 screen changes. A live reading
  flips a few hundred to 2,000, so a live screen rarely flashes. A redraw that changes no
  pixels is skipped.
- **Menus are bottom sheets** (`ui::MenuScreen`) over the screen they belong to, rather than
  separate screens. Any screen whose spec says "● menu" or "● detail" can use it.
- **The RTC counts as unset before 2025.** On this board it once came back at 2000-01-01 with
  its oscillator-stop flag clear, so the flag alone isn't enough (verified on hardware).
- **The RTC and the system clock hold UTC**; `TIMEZONE` from `.env` turns it into local time,
  DST included. The vendor keeps local time in the RTC with no DST, which a Central-time clock
  can't use. So RTC alarm registers are UTC: phase 9 converts the local wake time when it arms
  one. The sensor log's `time` column carries the UTC offset (`…T16:08:26-0600`).
- **Reading positions live in NVS**, not on the SD card: they belong to the device and survive
  a card swap.
- **Fonts are generated, not hand-made:** FreeType monochrome hinting from the pinned
  JetBrains Mono 2.304 release (sha256-checked). The generated sources are committed, so a
  build needs no Python.

## Known issues

- Podcast downloads run at about 275 KB/s, so a 100 MB episode takes about 6 minutes. Not
  profiled yet: candidates are Wi-Fi modem sleep, TLS, and SD writes from PSRAM buffers.

- SD at 20 MHz: the long-run check of the CRC-error fix never reported a result, so it's
  still unconfirmed.
- Home redraws whenever its status-bar temperature moves by 0.1°, every 5–30 s in the logs
  (about 30 px each). Harmless on USB, but on battery it's wasted refreshes until the 10-minute
  auto-off. A deadband in `statusTemp()` would fix it.
- 2026-11-12: the board dropped off USB three times during phases 4–6 and came back a few
  minutes later. Afterwards the PMIC reported no battery (it read 98 % and charging earlier),
  and the RTC had lost power (oscillator-stop flag set), so the board lost power completely.
  - The PMIC's power-off source register was empty, which fits a total power loss.
  - The battery turned out to be unplugged, so nothing carried the board through a USB
    dropout. The USB side (cable or port) is still the suspect for the drops.
  - When the battery was plugged back in, the gauge read 5 % with the cell at 4.17 V and
    charging. The gauge restarts from nothing after a total power loss and has to climb back;
    the voltage is the reading to trust until it does.
