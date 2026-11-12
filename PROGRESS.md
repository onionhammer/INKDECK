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
  press/hold, `b`/`B` BOOT/hold BOOT, `-`/`+` for volume, `r` for the PMIC registers, `i` for a raw IMU reading and `c` to reset the RTC as if the battery had been out (`src/devtools.h`). Unknown keys are ignored, so `.` pads a wait. Add `--log`
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
  - Landscape chrome (9).
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

### ⬜ 5b. Reader: images and cover art

Show the pictures in EPUBs instead of the `[image]` placeholder, plus each book's cover.
- **Decode:**
  - JPEG, PNG and GIF read from the EPUB zip (manifest media types).
  - JPEG via the ROM's TJpgDec or bitbank2's JPEGDEC; PNG via PNGdec.
  - Work buffers in PSRAM.
- **Fit to the panel:** scale to the text column, or the full page for large images. Dither to
  1-bit, with Floyd–Steinberg or Atkinson for photos and a threshold for line art. Option: the
  panel's 4-grey mode for image pages (about 3 s refresh).
- **Layout:** an image is a block in the flow text: its own page when tall, or a fixed-height
  block between paragraphs. Keep the alt text as a caption fallback. Also handle SVG `<image>`
  wrappers, which many covers use.
- **Cover art:**
  - Find it from the OPF's `<meta name="cover">` or the manifest's `properties="cover-image"`.
  - Show it as the book's first page.
  - Show it as a thumbnail in Home's READER preview (last book) and maybe in Files rows.
  - Cache the 1-bit thumbnail on SD, so Home doesn't decode JPEGs on every draw.
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
    header. SYNC TIME works (phase 11).
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
  the Level app (phase 12).
- [x] **Temperature calibrated:** the SHTC3 read 25.9 °C and 33 % against a 22.7 °C room,
  with the board on USB, charging. `sensor_hub` subtracts 3.2 °C and recomputes humidity for
  the cooler room (same water vapour), which gives about 40 % against the room's 37 %
  (household hygrometers are ±3–5 %). The board's heat depends on charging and load, so on
  battery it probably reads a little low.
- [x] Display units are a build flag: `-DINKDECK_FAHRENHEIT=1` in `platformio.ini` (set, at
  the user's request) shows °F; unset or 0 shows °C (`src/units.h`). The CSV log stays in
  °C.

### ⬜ 9. Clock (landscape)

- First landscape screen.
- [x] Setting the RTC: System → SYNC TIME (NTP, phase 11). A manual set-time screen would
  still help without Wi-Fi.
- [x] Time zone: `TIMEZONE` in `.env`, a POSIX TZ string with the DST rule (US Central here).
- Alarms that boot the board, and a calendar.

### ⬜ 10. Sleep / power-off

- The frog screen.
- The RTC-alarm → PWRON wake lifecycle and PMIC shutdown (AGENTS.md, "Power architecture").

### ⬜ 11. Wi-Fi, Weather, Podcasts

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
- Then the two network screens.

### ⬜ 12. Recorder, Level

Listed on Home but not designed yet.

## Decisions

- **HOLD BOOT always means home**, so the prototype's HOLD BOOT actions moved: the reader's
  text size is in its ● menu, and the player's stop was dropped (● pauses).
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
  can't use. So RTC alarm registers are UTC: phase 10 converts the local wake time when it arms
  one. The sensor log's `time` column carries the UTC offset (`…T16:08:26-0600`).
- **Reading positions live in NVS**, not on the SD card: they belong to the device and survive
  a card swap.
- **Fonts are generated, not hand-made:** FreeType monochrome hinting from the pinned
  JetBrains Mono 2.304 release (sha256-checked). The generated sources are committed, so a
  build needs no Python.

## Known issues

- The clock shows `--:--`: the RTC has never been set. Something has to set it (phase 7, 9
  or 11).
- SD at 20 MHz: the long-run check of the CRC-error fix never reported a result, so it's
  still unconfirmed.
- 2026-11-12: the board dropped off USB three times during phases 4–6 and came back a few
  minutes later. Afterwards the PMIC reported no battery (it read 98 % and charging earlier),
  and the RTC had lost power (oscillator-stop flag set), so the board lost power completely.
  - The PMIC's power-off source register was empty, which fits a total power loss.
  - The battery turned out to be unplugged, so nothing carried the board through a USB
    dropout. The USB side (cable or port) is still the suspect for the drops.
  - When the battery was plugged back in, the gauge read 5 % with the cell at 4.17 V and
    charging. The gauge restarts from nothing after a total power loss and has to climb back;
    the voltage is the reading to trust until it does.
