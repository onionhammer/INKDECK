# INKDECK/OS

Firmware for the [Waveshare ESP32-S3-ePaper-3.97](https://www.waveshare.com/esp32-s3-epaper-3.97.htm):
an e-reader, audio player and desk companion on an 800×480 1-bit e-paper panel. It's driven by a
3-way dial, the BOOT key and the PWR key. PlatformIO, Arduino core 3.3 on ESP-IDF 5.5.

<img src="docs/screenshots/home.png" width="240" alt="Home"> <img src="docs/screenshots/reader.png" width="240" alt="Reader"> <img src="docs/screenshots/level.png" width="400" alt="Level">

Screens as the panel draws them, saved over USB with `tools/screenshot.py`.

## Features

Home lists ten modules, each with a live preview. Every screen uses one key language: ▲▼ to
move or adjust, ● for the main action, HOLD ● for the second one, BOOT for back, HOLD BOOT for
Home. Landscape screens (Clock, Level, pictures held that way) turn the board so the wheel is on top;
there, rolling it left is ▲ and right is ▼.

| Module | What it does |
| --- | --- |
| **Reader** | EPUB 2/3 from microSD in Literata (three sizes), justified, with drop caps and line breaks at soft hyphens and dashes. Shows pictures (JPEG/PNG, dithered), the table of contents, and the cover as a page and on Home. Honours `display:none`. Resumes each book where you left it, even after a power loss |
| **Podcasts** | RSS feeds over Wi-Fi plus folders of MP3s. Downloads resume after a reset. Playback resumes where you stopped, and played episodes are marked |
| **Music** | MP3 and WAV from the card through the codec and speaker, with a waveform overview, queue and volume. Playback continues on any screen; HOLD BOOT jumps back to the player |
| **Weather** | Open-Meteo forecast: now, 12 hours and 6 days, with a detail sheet per day. Synced hourly (every half hour on USB) and cached for offline use |
| **Clock** | Landscape desk clock: calendar, sunrise and sunset, a weather strip, now-playing, and three alarms. An alarm can boot the board from power-off |
| **Recorder** | Voice memos from the onboard mic as 16 kHz WAV, with a live waveform, input meter and gain. Memos play back on the device with a volume meter (▲▼), and can be deleted |
| **Sensors** | Temperature and humidity with a 24 h sparkline, the IMU, PMIC readings, a live I2C bus scan, and a CSV log to the card |
| **Level** | Landscape bubble level and long-edge vial. Shows which side to raise and by how much, can be zeroed, and beeps when it's level |
| **Files** | Browses the microSD card and the onboard 9.9 MB FAT partition, and opens books, audio, pictures and text. With a PIN set, it can ask for it first unless you used it in the last minute (see Lock). A held dial pages through long folders. **HOLD ● deletes** the selected file or folder after asking; the sheet shows the file's size or how many items the folder holds, since they all go with it. Playback of a file being deleted stops first, and the Podcasts folder can't be deleted from while a download runs. **Pictures** (JPEG, PNG) show full screen, dithered, in portrait or landscape as the board is held (the IMU), and turn with it within a couple of seconds, on battery too; lying flat, in landscape when they're wider than tall. Phone photos come out upright (EXIF orientation), and ▲▼ steps through the folder. ● hides the status and hint bars. JPEGs of any size stream from the card; PNGs up to 2400 px wide and 4 megapixels. **Text**: any file that reads as text (UTF-8, Windows-1252 or UTF-16) opens in the text reader, which scrolls instead of paging: ▲▼ scrolls by a line, a quarter or half a screen (menu), and keeps going while held. Literata or JetBrains Mono in three sizes; hard-wrapped prose (Project Gutenberg) is joined back into paragraphs. The place, size and font are kept per file. A file over 4 MB shows its first 4 MB (less if memory is short) |
| **Network shares** | Files also browses shares on the local network (a NAS, a home server, a Windows PC), listed in `inkdeck.ini` (below): **SMB** (SMB2/3, user name and password) and **NFS** (v3). Each share is a row on the card's root after ONBOARD FLASH, tagged SMB or NFS. Opening one joins Wi-Fi and mounts it (read-only, so HOLD ● doesn't delete there), and its books, music, pictures and text open as they do from the card. Wi-Fi stays on while the share is open in Files or a file on it is (music playing, a book open), and goes off 30 s after |
| **System** | Battery and charger state, power rails, wake plan, Wi-Fi, memory and OTA slots, and the build running (version and git commit). Also NTP time sync, sleep, reboot, firmware updates over Wi-Fi, how often the screen does a full refresh, and the PIN lock (all below) |

**Power**
- PWR shows a sleep screen and powers the board off completely. The e-paper keeps its image,
  and the RTC wakes the board for the next alarm.
- After 10 idle minutes on battery, the board goes to standby: a clock face that the RTC
  redraws every minute, with the room temperature and humidity and the outside weather from
  the saved forecast. `INKDECK_STANDBY_WEATHER_HOURS` in `platformio.ini` has it fetch a new
  forecast every so many hours (0: never). Each wake light-sleeps through the panel refresh
  and runs at 80 MHz once the face is drawn.
- Reading (a book, a text file or a picture) and music don't time out until the battery is low.
- While you read on battery, the board light-sleeps between page turns. Under a picture, the IMU's wake on motion wakes it when you turn the board.
- On USB, PWR shows the clock face instead and the board stays on.

**Lock:** with a PIN set (System → PIN LOCK, or `inkdeck.ini` below), the board asks for it in
the places the build names in `INKDECK_PIN_ON` (`platformio.ini`, set to `files`; all three
if it's unset):

- `power_on`: whenever the board starts and whenever it wakes from the USB clock face. If
  `lock_minutes` is set, it also asks after that many minutes without a key, and unlocking takes
  you back to the screen you were on.
- `settings`: on opening **System** from Home, unless you used it in the last minute.
- `files`: on opening **Files** from Home, the same way. Reader and Music open their folders
  without it.

Leave `power_on` out to ask only for System and/or Files. Before System or Files, the wheels show
under that module's tag with ● OPEN on the last one, and BOOT on the first wheel goes back to
Home. You enter the PIN like a combination padlock: turn the wheel to pick each digit, then
press to move to the next. BOOT goes back a digit. Alarms still ring over the lock, and
stopping or snoozing one needs no PIN. In System, PIN LOCK sets a PIN of 4 to 8 digits (HOLD ●
after the last one, then the same again to confirm). Once one is set, it offers CHANGE PIN and
CLEAR PIN, and both ask for the current PIN first. The board keeps the PIN in its own flash and
replaces a `pin =` line on the card with a note. A new `pin =` line, or `pin = off`, takes
effect the next time you enter the current PIN. This is a casual lock: anyone with a USB cable
can reflash the board.

**Display:** screen changes use the fast partial refresh. A full refresh (the black-white flash)
clears the ghosting that partials leave, once they have flipped two screens' worth of pixels
or after 30 of them. System's AUTO FULL REFRESH sets that to 2, 5, 10 or 20 screens, or OFF,
and the setting is kept. The serial log reports each refresh and how much change has built
up since the last full one. HOLD BOOT always does a full refresh.

**Settings:** Wi-Fi, time zone, location and the password for updates over Wi-Fi come from
`.env` at build time (template: `.env.example`). `inkdeck.ini` in the card's root overrides the
Wi-Fi network, time zone and location without a rebuild, sets the lock and lists network shares
(example: [`inkdeck.ini.example`](inkdeck.ini.example)). The board reads it at startup, and if
the card has none, it writes one with every line commented out. **Passwords don't stay on the
card**, since a card is easy to lift: the board keeps the Wi-Fi password and each SMB share's in
its flash (NVS) and rewrites their lines without them. To change one, write it in again. The
flash isn't encrypted, so someone with the board and a USB cable can read them, as they can the
`.env` values built into the firmware. `INKDECK_FAHRENHEIT` in `platformio.ini` switches between
US and metric units.

```ini
[wifi]
ssid = HomeNet
password = secret                     ; the board keeps it and replaces the line with a note
[clock]
timezone = GMT0BST,M3.5.0/1,M10.5.0   ; a POSIX TZ string, daylight saving rule included
[location]
latitude = 51.48                      ; decimal degrees, north and east positive
longitude = 0.00
[network]
smb = smb://erik:secret@192.168.1.20/share/media?name=Media  ; the board keeps the password
nfs = nfs://nas.local/volume1/books   ; up to 4 shares in all
[lock]
pin = 2468                            ; 4 to 8 digits; pin = off removes it
lock_minutes = 5                      ; also lock after 5 minutes without a key
```

Each `smb =` or `nfs =` line is a share Files lists:

- **SMB:** `smb://[domain;]user:password@server[:port]/share[/folder]`, SMB2 or SMB3 through
  [libsmb2](lib/libsmb2/README.md), signed if the server asks. Write `@ : / ? ; %` in a password
  as `%40 %3A %2F %3F %3B %25`. A folder after the share opens Files there.
- **NFS:** `nfs://server/export` or `server:/export`, NFS v3 over TCP with no password. Options
  `uid` and `gid` (who the server takes the board for, default 0), `nfsport` and `mountport`
  (else the server's portmapper says). The server must export the folder to the board's address.

Both take `name` (what Files calls the share) after a `?`. Use the server's IP address if it's on
another subnet: `.local` names don't cross routers. Options and limits:
[`src/shares.h`](src/shares.h).

## Build and flash

The PlatformIO CLI is at `~/.platformio/penv/Scripts/pio.exe` (or use the VS Code extension).

```sh
cp .env.example .env          # optional: Wi-Fi, time zone, location
pio run -t upload             # build and flash over the board's USB-C
pio device monitor            # serial console, 115200
```

If the USB port disappears, hold BOOT while power-cycling the board, then upload again.
`tools/screenshot.py` drives the UI over USB and saves the screen as a PNG. `src/devtools.h`
lists the serial dev commands.

### Updating over Wi-Fi

Once a build with Wi-Fi and an `OTA_PASSWORD` in `.env` is on the board (flash it over USB the
first time), later builds can go over the network:

1. On the board: System → UPDATE OVER WI-FI. It joins the network and listens for 10 minutes
   (each attempt restarts the window), showing its address.
2. On the computer, from the same network:

   ```sh
   pio run -t upload --upload-port inkdeck.local    # or the IP the board shows
   ```

PlatformIO sends a network upload with espota, and `scripts/gen_secrets.py` passes it the
`OTA_PASSWORD` from `.env` (the firmware keeps only its SHA-256). espota has the board connect
back to the computer for the image, so the firewall must let Python accept incoming connections
(macOS asks the first time). espota's debug output prints the password, so don't paste upload
logs anywhere public.

The image goes into the idle OTA slot. Only a complete image that passes its checks switches
the boot slot, so cutting the power or Wi-Fi midway leaves the running firmware in place. The
board then restarts into the new build. The System screen's context line shows the build's git
commit (with `+` for uncommitted changes), so you can see it took. A new build stays on
probation until it gets through `setup()`. If it crashes or resets before then, the bootloader
goes back to the previous one, and System's OTA row says `ROLLED BACK FROM app1`. A USB upload
still works as before and always boots the slot it writes (`app0`).

## More

- [`AGENTS.md`](AGENTS.md): hardware, power architecture, driver notes and gotchas.
- [`docs/ui-design.md`](docs/ui-design.md): the UI spec, one entry per screen, and a link to the
  design canvas.
- [`docs/`](docs/README.md): schematic, datasheets, mechanical files and the vendor wiki.
- [`enclosure/`](enclosure/README.md): a 3D-printable back tray and a snap-on front bezel whose
  caps wrap the corners, with the Sleep-screen frog engraved on the back.
