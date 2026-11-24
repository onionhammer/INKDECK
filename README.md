# INKDECK/OS

Firmware for the [Waveshare ESP32-S3-ePaper-3.97](https://www.waveshare.com/esp32-s3-epaper-3.97.htm):
an e-reader, audio player and desk companion on an 800×480 1-bit e-paper panel. It's driven by a
3-way dial, the BOOT key and the PWR key. PlatformIO, Arduino core 3.3 on ESP-IDF 5.5.

<img src="docs/screenshots/home.png" width="240" alt="Home"> <img src="docs/screenshots/reader.png" width="240" alt="Reader"> <img src="docs/screenshots/level.png" width="400" alt="Level">
<img alt="368781D4-93A9-43E5-A802-835D6F896E2A_1_105_c" src="https://github.com/user-attachments/assets/9cf2d82b-b768-4e54-b666-16e1b94db101" width="486" alt="Enclosure" />

Screens as the panel draws them, saved over USB with `tools/screenshot.py`.

## Features

Home lists eleven modules, each with a live preview. Every screen uses one key language: ▲▼ to
move or adjust, ● for the main action, HOLD ● for the second one, BOOT for back (on Home, a
full refresh), and HOLD BOOT on any screen to talk to the agent (while music or a
podcast plays, it raises a player sheet instead). Landscape screens (Clock, Level) turn the board
so the wheel is on top; there, rolling it left is ▲ and right is ▼. Pictures turn with the
board instead, and the wheel keeps its portrait sense there.

| Module | What it does |
| --- | --- |
| **Reader** | EPUB 2/3 from microSD in Literata (three sizes), justified, with drop caps and line breaks at soft hyphens and dashes. Shows pictures (JPEG/PNG, dithered), the table of contents, and the cover as a page and on Home. Honours `display:none`. Resumes each book where you left it, even after a power loss. The **library** lists every book in `SD:/Books` (and its folders) under its author, authors by surname and each author's books by title, with how far each is read; titles and authors are read from each book once and cached on the card |
| **Podcasts** | RSS feeds over Wi-Fi plus folders of MP3s. Downloads resume after a reset. Playback resumes where you stopped, and played episodes are marked |
| **News** | RSS 2.0, RSS 1.0 and Atom feeds, listed one URL a line in `SD:/News/feeds.txt` (`src/apps/news.h`). Every feed's stories in one list, newest first, grouped by day; a new story (not opened, from the last three days) has a solid square and a bold headline, and a card under the list shows the selected one's headline and summary. A story reads like a note: the headline in capitals, the byline and its length, then the text in Literata a page at a time, paragraphs, headings, lists and quotes kept; ▼ past the last page opens the next story. A feed that sends only summaries (BBC News) gets the whole story from the page it links to, fetched when the story opens, with its progress in the strip, and kept on the card, so it opens offline afterwards. With inkharness set up, the daemon fetches the page and reads it with Mozilla's Readability (its `reader.rs`), pictures and captions included, dithered for e-paper; without it, or when it can't be reached, the board reads the page itself (`src/article.h`: paragraphs, headings and links; menus, bylines, captions, videos and related stories left out). Until it comes, or if it can't, the summary shows, with the story's address. ● on a story: LINKS lists the links in its text, and one opens the same way, as a page of its own (one level: its links don't open). HOLD ● on the list: save a story, show all, new or saved ones, sync now, the feeds (each with its new stories, `!` and the reason for one that fails), mark all read. Stories and their text are kept on the card (the newest 50 a feed, and saved ones), so News opens offline; it syncs when opened (if the last sync is half an hour old), hourly while open and on USB. PWR on News powers off behind its **off face**: the stories of the last 12 hours one at a time (headline and summary), with the time, the weather and a frog reading the paper. The RTC wakes the board every minute for the clock, every `news_face_every` minutes (5) for the next story, and once the cycle has shown them all, to fetch more, at most hourly. HOLD PWR opens the story on the face (`src/apps/news_edition.h`) |
| **Music** | MP3 and WAV from the card through the codec and speaker, with a waveform overview, queue and volume. Playback continues on any screen; while it plays, HOLD BOOT raises a player sheet over any screen (what's playing, pause / play, next, volume; BOOT closes it) |
| **Weather** | Open-Meteo forecast: now, 12 hours and 6 days, with a detail sheet per day. Synced hourly (every half hour on USB; while the Reader naps, at the next page turn) and cached for offline use |
| **Clock** | Landscape desk clock: calendar, sunrise and sunset, a weather strip, now-playing, and three alarms. An alarm can boot the board from power-off |
| **Recorder** | Voice memos from the onboard mic as 16 kHz WAV, with a live waveform, input meter and gain. Memos play back on the device with a volume meter (▲▼), and can be deleted |
| **Talk** | Hold BOOT on any screen and talk to an assistant on your network: a sheet rises over the lower half of the screen with what it heard and its reply; it speaks back, and can play music on speakers through Home Assistant. It can also use the board: find and open a book, read out the forecast, set an alarm, change the volume, open a screen, or say how the battery is (`src/apps/board_tools.h`), and search the web (SearXNG, Brave or Tavily). A screen it opens comes up once the reply has been said. **Long answers go on the screen**: for a recipe, the steps of a how-to or a list, it says a sentence or two and puts the whole thing up as a **note** (`src/apps/note.h`), paged like the Reader (▲▼, Literata in three sizes), with headings, bullets, numbered steps and checklists. Each note is kept as Markdown in `SD:/Notes/` (the onboard flash without a card), where Files opens it again; "show me the recipe again" brings back the last one where it was left, and it can read the note on screen to answer a follow-up ("double it"). The board streams the mic, encrypted with the token both ends hold, to [`inkharness serve`](inkharness/README.md), a Rust daemon that runs speech-to-text, a language model and text-to-speech (hosted or local, each swappable; a model that hears or speaks can stand in for the first or last), and plays the reply as it arrives. Set `agent_host` in `SD:/inkdeck.ini`: a machine on your network, or on your tailnet from any Wi-Fi (Tailnet, below). Protocol: [`docs/voice-protocol.md`](docs/voice-protocol.md). The same program, installed with one line, is also a chat in the terminal and an HTTP API, using what it finds in the folder it starts in (`AGENTS.md`, skills, `.mcp.json`). **Not yet tested on the board** |
| **Agent** | Home → AGENT (03) lists the sessions inkharness runs for you: jobs that keep working on the daemon while the board sleeps, grouped NEEDS YOU, RUNNING, SCHEDULED and DONE (`src/apps/agent.h`). A session shows its plan as a checklist, the steps it has taken, its cost, and its result (● puts it up as a note); HOLD ● pauses, resumes, stops or deletes it. **Schedules** ("every weekday at seven, brief me") show their time, days, prompt and last runs, and can wake the board, show the result as a note, or say it on USB; **watches** ("tell me when the fare to Lisbon drops under $700") show their readings as a sparkline; a session can also draw a **panel** of its own (stats, rows, a chart, a choice). When one asks you to choose, an **ask sheet** rises over whatever you're doing (once per question; BOOT: in an hour), with a chirp, `?1` in the status bar and `1 ASK` on Home: ▲▼, ● sends, HOLD BOOT answers in words. HOLD BOOT on the Agent starts a session, or talks into the one on screen. Anywhere else HOLD BOOT is a talk turn: the limited assistant above, which can't see or start sessions, but can file a job that waits on the list until you start it. The board doesn't hold Wi-Fi for this: the connection is live on the Agent's screens and on USB, and otherwise the board checks in every 5 to 30 minutes (every 30 while you read on battery, so the board can sleep between pages), or when inkharness asks. PWR on the Agent, or while something runs or waits, shows the **Agent face** (`src/apps/agent_face.h`) and powers off; the RTC wakes the board to check in, chirp and redraw it, and HOLD PWR opens the Agent. With `agent` in `INKDECK_PIN_ON`, the Agent opens behind the PIN, which is asked again after every start or PWR, the same digits unlock inkharness's own PIN (`pin_sha256`), and the sheet, Home and the face show counts only (`agent_face_details = on` shows everything on the face). Needs an inkharness with an `[agent]` table (issue #52). Tested on the board 2026-11-21 against the live inkharness: the list, the ask sheet answered by key, the face, and its RTC check-in |
| **Comms** | Home → COMMS (05): walkie-talkies with the other boards of your groups, board to board over the radio with no router (`src/apps/comms_ui.h`; the engine below). One group at a time: its boards and how each is reached (over the air with its signal, through which relay, or away) as a list and a small map, the channel and the state (QUIET, MIC ON, HEARING, LOST) with a level meter, and the group's log: one row a press (a mic session), rings and messages. **HOLD BOOT talks**, to the row selected (all of the group, or one board), and letting go stops; **● while BOOT is held latches the mic**: let go of BOOT and it stays on (how a room is heard) until the next BOOT or ● on these screens, which only stops it. While your mic is on nothing of comms plays here (half duplex, as on a walkie-talkie): a board talking meanwhile shows as `KITCHEN TALKING TOO`, and you hear it once you let go. While it's on, an inverted **MIC** shows in the status bar on every screen (outlined while the agent has the mic: it pauses and picks up again), on the desk face, and on the Agent and News faces live on USB; on battery PWR powers off, which switches it off. ● on the Comms screen **rings** the row selected (a ringing sheet counts the seconds and the boards ringing; from 5 s HOLD BOOT leaves a message instead; at 60 s NO ANSWER; answered, it becomes the call sheet, your mic still off), or JOINs or LEAVEs the group's conversation while one is on. HOLD ● opens the menu: a board's **mic screen** when the row selected has its mic on (ON, TALKING or LOST and for how long, its level, a two-hour chart of 2-minute bars, its power and reach, its LOSTs), PLAY LAST, MESSAGES (● plays one), RELAY FOR OTHERS, CALL CHIRP and GROUPS (to switch, and to set each one's way: AIR+NET, AIR, NET). A ring for this board raises a **ring sheet** over whatever you're doing: ● answers (your mic stays off), HOLD BOOT answers and talks, BOOT ignores it; a ring that ends unanswered shows MISSED, or LEFT A MESSAGE with ● to play it. When a conversation's voice first plays here, a **call sheet** rises once (not at every press): HOLD BOOT talks back, ▲▼ volume, HOLD ● leaves the conversation, BOOT closes; it closes 10 s after the last word and leaves your mic as it is. When a latched mic you hear goes silent without switching off, a **LOST sheet** rises with the alarm: ● silences it, and it says BACK when the mic is heard again; where the sheet can't rise (the lock, a face, the Recorder) the first key silences the alarm and the sheet comes up afterwards. A press that goes silent (out of range) is just cut off, without an alarm. A conversation you aren't in shows a speech bubble in the status bar. HOLD BOOT is the agent's everywhere else. BOOT stops PLAY LAST or a message first. Home's COMMS shows the mics on (`KITCHEN on · 12 min`, `yours on`, `KITCHEN lost`) and `1 NEW` while calls, missed rings, LOSTs or messages wait. The screens redraw only when what they show changes: a latched mic costs about one partial refresh a minute once nobody has pressed a key for 30 s. The screens hold the radio, so calls come in on battery while they're up; otherwise the board listens on USB, or while a mic is on. Tested with two boards 2026-11-24 |
| **Sensors** | Temperature and humidity with a 24 h sparkline, the IMU, PMIC readings, and a scan panel: the live I2C bus, the Wi-Fi networks in range or the Bluetooth LE devices around, the strongest seven of each. A CSV log to the card. Its menu opens the full scans. **Wi-Fi**: a 2.4 GHz channel chart and every network with its channel, width, security and generation; a network's detail follows its signal live (two minutes of history) and shows what the driver reports (ciphers, WPS, country, BSS colour, FTM) and the networks that share its channels. **Bluetooth LE**, in a build with `INKDECK_BLE=1` in `platformio.ini` (off by default: it keeps about 26 KB of internal RAM from boot and takes about 230 KB of flash; the S3 has no Bluetooth Classic): a proximity strip and every advertising device, named or decoded from its advert (Apple Continuity, Microsoft Swift Pair, iBeacon, Eddystone, Fast Pair, appearance, services, maker); a device's detail has its live signal, transmit power and path loss, advertising interval and raw advert bytes. Both details can beep once a second, higher as the signal gets stronger, to walk towards it |
| **Level** | Opened from Sensors' menu (SPIRIT LEVEL, which shows the live tilt), not from Home. Landscape bubble level and long-edge vial. Shows which side to raise and by how much, can be zeroed, and beeps when it's level |
| **Files** | Browses the microSD card and the onboard 9.9 MB FAT partition, and opens books, audio, pictures and text. With a PIN set, it can ask for it first unless you used it in the last minute (see Lock). A held dial pages through long folders. **HOLD ● deletes** the selected file or folder after asking; the sheet shows the file's size or how many items the folder holds, since they all go with it. Playback of a file being deleted stops first, and the Podcasts folder can't be deleted from while a download runs. **Pictures** (JPEG, PNG) show full screen, dithered, in portrait or landscape as the board is held (the IMU), and turn with it within a couple of seconds, on battery too; lying flat, they stay as they were (tilt decides, not the picture's shape). The wheel works
the same way round in both. Phone photos come out upright (EXIF orientation), and ▲▼ steps through the folder. ● hides the status and hint bars. JPEGs of any size stream from the card; PNGs up to 2400 px wide and 4 megapixels. **Text**: any file that reads as text (UTF-8, Windows-1252 or UTF-16) opens in the text reader, which scrolls instead of paging: ▲▼ scrolls by a line, a quarter or half a screen (menu), and keeps going while held. Literata or JetBrains Mono in three sizes; hard-wrapped prose (Project Gutenberg) is joined back into paragraphs. The place, size and font are kept per file. A file over 4 MB shows its first 4 MB (less if memory is short). A Markdown file (`.md`) in a folder called `Notes` opens as a note instead (see Voice) |
| **Network shares** | Files also browses shares on the local network or your tailnet (a NAS, a home server, a Windows PC), listed in `inkdeck.ini` (below): **SMB** (SMB2/3, user name and password) and **NFS** (v3). Each share is a row on the card's root after ONBOARD FLASH, tagged SMB or NFS. Opening one joins Wi-Fi and mounts it (read-only, so HOLD ● doesn't delete there), and its books, music, pictures and text open as they do from the card. **HOLD ● saves** the selected file to the card, into the folder its type belongs in: an MP3 to Music or Podcasts (a sheet asks which), a WAV to Music, a picture to Pictures, an EPUB to Books, any other text to Documents. Music keeps the folders it was in below the share's own `music` folder (else the folder it's in, the album), and a podcast lands in its show's folder (the one below the share's `podcasts` folder), where Podcasts lists it. A file of the same name and size already there isn't copied again; a different one gets a ` (2)`. The header shows how far it has got and the time left, BOOT stops it (nothing is left behind), and the board doesn't power off until it's done. Wi-Fi stays on while the share is open in Files or a file on it is (music playing, a book open), and goes off 30 s after |
| **System** | Battery and charger state, power rails, wake plan, Wi-Fi, the tailnet, memory and OTA slots, and the build running (version and git commit). Also NTP time sync, sleep, reboot, firmware updates over Wi-Fi, how often the screen does a full refresh, the PIN lock, and the card as a network drive (all below). The actions are in a sheet that ● opens |

**Power**
- PWR shows a sleep screen and powers the board off completely. The e-paper keeps its image,
  and the RTC wakes the board for the next alarm. The sleep screen shows the cover of the book
  you're reading, small, with its title and progress. With a book open, PWR closes it instead
  (below).
- After 10 idle minutes on battery, the board goes to standby: a clock face that the RTC
  redraws every minute, with the room temperature and humidity and the outside weather from
  the saved forecast. `INKDECK_STANDBY_WEATHER_HOURS` in `platformio.ini` has it fetch a new
  forecast every so many hours (0: never). Each wake light-sleeps through the panel refresh
  and runs at 80 MHz once the face is drawn.
- Reading (a text file or a picture) and music don't time out until the battery is low.
- A book left open on battery closes after 15 minutes without a key, or at once on PWR: the
  panel shows its cover across the whole screen with HOLD PWR TO RESUME, and the board
  powers off completely. PWR opens the book again at its page (so does an alarm's wake, under
  the Clock, and plugging in USB). A book without a cover picture gets its title in type. Not on
  USB, where the page stays (PWR shows the clock face), and the timeout not while music plays.
- While you read on battery, the board light-sleeps between page turns, with the panel's power
  off. A paused podcast or song doesn't keep it awake: pause switches the speaker amp, the codec
  and its supply off. The hourly weather sync waits for the next page turn. Under a picture, the
  IMU's wake on motion wakes it when you turn the board.
- Audio hardware (amp, codec, the codec's and mic's supply) is on only while something plays,
  rings or records. The PMIC outputs nothing uses stay off.
- On USB, PWR shows the clock face instead and the board stays on, and on the tailnet if it has
  a Tailscale key (Tailnet, below).
- PWR on News (or 10 idle minutes there on battery) powers off behind its off face, one story
  of the last 12 hours at a time with the clock and the weather. The RTC wakes the board every
  minute, as standby does: the clock redraws from what NVS keeps (a partial refresh, no card, no
  radio; about 0.8 s on), and every `news_face_every` minutes the next story comes up from the
  card (about 1.2 s). When the cycle has shown every story and the last fetch is an hour old,
  that wake joins Wi-Fi and fetches the feeds and the forecast (90 s at most); otherwise the
  cycle starts again. Below 15 % battery it stops: STOPPED AT over the time, no wakes but alarms.
  HOLD PWR (or PWR during a wake) opens News on the story the face showed. On USB the face stays
  up, live.

**Lock:** with a PIN set (System → PIN LOCK, or `inkdeck.ini` below), the board asks for it in
the places the build names in `INKDECK_PIN_ON` (`platformio.ini`, set to `files,agent`; all four
if it's unset):

- `power_on`: whenever the board starts and whenever it wakes from the USB clock face. If
  `lock_minutes` is set, it also asks after that many minutes without a key, and unlocking takes
  you back to the screen you were on.
- `settings`: on opening **System** from Home, unless you used it in the last minute. It asks
  again if you come back to System (from the updater, the desk face, a sheet) more than a minute
  after you last used it, or leave it on screen a minute without a key.
- `files`: the same for **Files**: coming back to it from a book, a picture or the desk face
  after a minute, or leaving it a minute without a key, brings the PIN up again. The Reader's
  library (only books in `SD:/Books`) and Music (`SD:/Music`, only that folder: BOOT stops at it,
  and there's no delete, onboard flash or network shares there) still open without it. Without
  such a folder, or for a book kept elsewhere, they open Files with the PIN.
- `agent`: the same for the **Agent**, except that every start and every PWR (the USB clock face
  included) asks again, however recently you used it. The digits also unlock inkharness when it
  has a PIN of its own.

Leave `power_on` out to ask only for System, Files and/or the Agent. Before any of them, the
wheels show under that module's tag with ● OPEN on the last one, and BOOT on the first wheel goes
back to Home. Unlocking the lock counts as opening all three. After three wrong PINs in a row, each try
waits: 30 s, then twice as long each time, up to 30 minutes. The count survives a restart (the
wait starts again), and the right PIN clears it. You enter the PIN like a combination padlock: turn the wheel to pick each digit, then
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
up since the last full one. BOOT on Home always does a full refresh.

**Wi-Fi drive:** System → WI-FI DRIVE puts the microSD card on the network as a drive, to copy
books, music and `inkdeck.ini` without taking the card out. The board joins Wi-Fi and serves the
card over WebDAV at `http://inkdeck.local` (the screen shows the IP too) until you leave the
screen, or after 10 minutes without a file read or written or a key pressed. On a Mac: Finder →
Go → Connect to Server, `http://inkdeck.local`, then Registered User (not Guest). On Windows: Map
network drive, the same address. Sign in as `inkdeck` with the password on the screen; the board
makes it up the first time and keeps it, so the computer can remember it. A Mac shows the drive's
free space as unknown: told it, macOS hangs 90 s while it mounts. The board keeps using the card
meanwhile, and nothing restarts. If the computer changed `inkdeck.ini` or `Podcasts`, leaving the
screen reads them again. It's slow, about 0.5 MB/s, so pull the card for big copies. BOOT stops
it, after asking if a copy is under way. If the build asks for the PIN before Files, WI-FI DRIVE
asks too, unless opening System already did. (The card as a USB drive was tried first and
dropped: macOS reads a FAT32 card's whole FAT before mounting it, and at USB full speed a big
card's takes longer than macOS waits.)

**Tailnet:** with a Tailscale auth key in `inkdeck.ini` (`[tailscale]`, below), the board joins
your tailnet whenever something needs a machine there, through
[MicroLink](lib/microlink/README.md), a Tailscale client for the ESP32 (`src/tailnet.h`). So
`agent_host` can be your computer's name on the tailnet (`macbook`, or
`macbook.tail1234.ts.net`) or its 100.x address, and the board reaches inkharness from any Wi-Fi
it can join, a phone's hotspot included. With a name, the board tries the LAN first: the
address your router's DNS gives the name, and the addresses the machine last gave the tailnet.
At home one answers within milliseconds and the board connects there without joining (voice
starts about 2.6 s after you press BOOT); elsewhere nothing answers within a second and it goes
through the tailnet. A 100.x address always goes through the tailnet. A share in Files works
the same way (`smb://erik@macbook/share`), and a podcast feed can be on a tailnet machine too. While Wi-Fi Drive or Update over Wi-Fi is open, the board answers on the
tailnet too, at `http://inkdeck` or its 100.x address (the screen shows it), so you can copy
books or push an update from elsewhere. A host counts as on the tailnet when it's a
100.64.0.0/10 address, ends in `.ts.net`, or has no dots (a machine's name there); the board
looks such names up in the tailnet's list of machines. On battery it leaves the tailnet a minute
after the last thing using it lets go. **On USB power it stays on the tailnet**, Wi-Fi with it, so
your other machines always find it; unplugged, it goes back to joining only when needed. System's
TAILNET row says whether it's on and at what address.
Make the key in Tailscale's admin console (Settings → Keys): reusable and not ephemeral, since
the board is offline most of the time and Tailscale removes an ephemeral machine soon after it
goes. Once the board shows up in Machines, turn off its key expiry, or it has to join again with
a new key when that runs out. A different key joins the board as a new machine, and
`tailscale_key = off` takes the key off it. The board relays through the same Tailscale relay
region (DERP) as `agent_host`'s machine, or as most of your machines: it learns that at its first
join after a start, joins again from there if it has to (about 4 s more, once), and keeps it. Joining
takes about 4 s after Wi-Fi, and voice over the tailnet starts about 6.5 s after you press BOOT
(measured with inkharness on a home server). While on the tailnet, MicroLink's four tasks take
42 KB of internal RAM, and fetching the list of machines takes 512 KB of PSRAM. Voice over the
tailnet is tested on the board; shares, feeds, Wi-Fi Drive and updates over it aren't yet.
**Exit node:** with `tailscale_exit_node = on`, the board offers itself as an exit node, so a
laptop or phone on the tailnet can send its internet traffic out through the board's Wi-Fi.
Approve it in the admin console (Machines → the board → Edit route settings → Use as exit
node); access rules other than the default must allow `autogroup:internet`. It's IPv4 only: the
core's lwIP can't forward IPv6, so a machine using it falls back to IPv4 for sites that have
both. It's there whenever the board is on the tailnet, so in practice on USB power, where the
board also keeps Wi-Fi's modem sleep off. Everything is encrypted in software on a 240 MHz core
over 2.4 GHz Wi-Fi, so it's much slower than a computer would be. System's TAILNET row ends in
`exit node` while it offers itself. The board advertises the route and forwards with NAT
(tested 2026-11-21: on USB it joined and turned it on, with the same 93 KB of internal RAM
free); traffic through it isn't tested yet.

**Comms** (issue #66; the engine, `src/comms.h`, under the screens above): boards talk to each
other directly, board to board over the radio with no router (ESP-NOW), in **groups**: each
`mesh_group` line in `inkdeck.ini` (up to 4) is a secret its boards share, which keys the group's
encryption (AES-GCM) and gives its id, whose first 4 hex digits (`7F3A`) the screens show so boards
can be checked against each other. The whole line is the invite: paste it on every board of the
group. Without a line, a board with an `agent_token` is in one group, HOME, with the other boards
of that token. **The mic:** a walkie-talkie's. You hold BOOT to talk (to all of the group, or to
one board) and let go; ● while holding latches it on, for a room, until you stop it. Nothing
another board sends switches it on, and it doesn't survive a restart. Everything the mic picks up
goes out while it's on, after the cue (the first 280 ms, the cue and the codec's start-up thump,
don't go): from the press to the first frame on the air takes about 0.39 s (0.31 s with the
speaker already on), and voice reaches the other board's speaker about 0.12 s after it was said.
It's half duplex: while your mic is on nothing of comms plays on your board, and what the others
said meanwhile plays once you let go (and is in the last 60 s heard). Several boards can talk at
once; each hears the others mixed (up to 4) when its own mic is off. Voice goes as IMA ADPCM,
16 kHz (64 kbps), 20 ms a packet. Every board of the group sees whose mics are on, latched or
held, for how long and their last 2 hours of levels; a latched mic that goes silent without
switching off is **LOST** after 10 s, with an alarm on the boards that heard it until you silence
it or it's back; a held one (a press) that goes silent is cut off after 5 s, without an alarm; one
that switches off after more than 5 minutes chirps once where it was heard. A board whose mic is
on doesn't ring or chirp. Voice doesn't play over the Recorder, an alarm or the agent, and stops
music, which plays on afterwards; those, and music you start, pause a mic, which picks up by itself
once they're done. The last 60 s heard are kept to play
again, with a log of mic sessions, rings and messages. **Conversations:** a group has one at a
time, on while a mic in it is on or from an answered ring, until 60 s after the last word; the
voice plays only on the boards in it (switching a mic on joins it or opens it, and every board
that hears that mic is in), every board of the group sees it's on and who is in it, and a board
can leave it (its voice stops playing there, and its mic goes off) and join again. **Ringing:** a
board rings all of a group or one board; the others ring (a phone's trill every few seconds) until
one answers, the dialer gives up, or 60 s pass, and the dialer sees how many are ringing. From 5 s
into a ring nobody answers, the dialer can **leave a message** instead (up to 60 s): the boards
ask again for every frame they missed until it's whole (a checksum says so), keep it on the card
(`SD:/.inkdeck/comms/`, in RAM without one) until played and for a day after, and play it only
when asked. The screens above show all of this (HOLD BOOT keys the mic). Boards pass each other's packets on (a **mesh**: flooding, up to three relays), also for
groups they aren't in, unread; voice and message frames only where a board would otherwise miss
them (each board says every few seconds which boards it hears straight, and how well), so boards
in one room don't double each other's voice on the air. Each board knows how the others are
reached: over the air with their signal, through which relay, or away. With **`mesh_range =
long`** a board sends at Espressif's long-range rate, 500 kbps instead of 1 Mbps: further, each
packet twice as long on the air; every board hears both, and the Comms screen shows `LONG RANGE`
beside the channel. A board listens on USB power, while a Comms screen
holds it, while its mic is on or it hears another board's; on battery otherwise the radio stays
off. It uses the Wi-Fi network's channel while joined, else `mesh_channel` (6): set that to your
Wi-Fi's channel so boards on and off it hear each other. `via` per group (`air`, `net` or `both`)
is there for the link through inkharness, which isn't built: `net` sends nothing yet. Packet format
and keys: [`docs/comms-protocol.md`](docs/comms-protocol.md). Measured 2026-11-23 on USB: a mic on,
or voice playing, costs about 10 KB of internal RAM (the speaker and the mic at once: 67 KB free
instead of 77 KB with the tailnet up); the image is 2.43 MB with the screens (`INKDECK_COMMS=0` leaves comms out).
The walkie-talkie was tested 2026-11-24 between two boards in different rooms (−76 to −78 dBm),
both ways, with test tones added to the mic (nobody was there to speak or listen): HOLD BOOT to
all, every frame sent seen on the other board, the end of the press included; 20 s held each way
(1.7% and 18.4% of frames lost on the air, each covered by the one before); the latch, stopped by
BOOT and by ● without either doing anything else; both boards talking at once (neither heard the
other until it let go); a message waiting while the mic was on; a latched mic vanishing (LOST, the
alarm, back) and a held one (cut off, no alarm); the call sheet once per conversation; ring,
answer, then talking both ways; a message after an unanswered ring; a sleeping board woken by a
ring. The open mic's tests (2026-11-23) covered no ringing on a board with its mic on, leaving,
and the alarm, the Recorder and the agent pausing a mic. Rings, conversations and messages were
tested 2026-11-22 the same way. The
long-range rate and relaying only when needed were tested 2026-11-23 the same way: a board on long
and one on normal hear each other; with the rule neither board relayed the other's voice, and at
long range with both mics on it lost 1.4 to 6% of frames where flooding lost 11 to 17%. Not
tested: anything by ear, three boards at once, on battery, range and current. Dev commands over
USB: `!comms`, `!commstest`, `!commsping`, `!commsmic [all|board] [latch]`, `!commsmicoff`,
`!commstalk N [board]`, `!commsplay`, `!commsring [board]`, `!commsringstop`,
`!commsanswer`, `!commsignore`, `!commsjoin`, `!commsleave`, `!commsmsg N`, `!commsplaymsg`,
`!commsgroup` (a test group without the card), `!commschannel`, `!commsrelay`, `!commsresend`, and
the test switches `!commstone S dB`,
`!commsvanish`, `!commsack`, `!commslong S`, `!commsbusy`, `!commsringvanish` and
`!commsdropmsg N`; `!commsrange normal|long|-` (the `mesh_range` line without the card),
`!commsloss S` (each board heard straight: its packets that came and didn't, and the gaps),
`!commsair S` (how much of the time the channel carries frames, by kind), and the test switches
`!commsdeaf ID` (as if that board were out of range), `!commsflood on|off` (relay every voice
frame, as before the rule) and `!commslrrx on|off` (the long-range protocol off); `!quiet` takes
30 dB off every sound for testing next to someone.

**Reachable on battery** (issue #66, phase 7; `mesh_reachable` in `inkdeck.ini`, off by default):
a board on battery that would power off (Sleep, the Clock face, a closed book, the Agent and News
faces, by itself or by PWR) keeps its face on the panel but stays in light sleep with the radio
off instead, waking every `mesh_reachable` seconds (2 to 30, `on` is 5) to ask the boards in range
whether anything waits for it, and listening 12 ms. A board that's awake answers if it knows of a
ring that includes it (rings flood, so any board on USB knows), someone talking to it now (a held mic; a latched one, a room, doesn't wake it), or
a message it sent for it in the last 5 minutes; the sleeper then comes up with its face, rings or
plays as usual (the sheets rise over the face), and goes back to sleep 30 s after it's over (60 s
after the last press while a mic it hears stays on), unless a key makes it an ordinary board that's on. Only PWR
wakes it otherwise (as a power-on), and what its face needs (the clock's minute, check-ins, News's
laps, alarms) works as before: the board restarts for those as the RTC alarm would boot it. Boards
that are awake show a sleeping one as `ASLEEP · WAKES WITHIN 10 S`, and ring it as any board. Below
15 % battery, or with the setting off, the board powers off as before. A sleeping board doesn't
sound LOST's alarm (it isn't listening): a board that must stays on USB. A board keeps the channel
it last heard the others on, checks it every 3 minutes with a poll the boards on USB answer even
with nothing, and sweeps the channels after three unanswered. `!reachlog` (and System's SLEEP row)
is a battery log for a night unplugged: polls, answers, wakes, time the radio was up, battery at the
start and now. Not built: boards on USB keeping a message for a sleeper after its sender's 5
minutes. Packets and rules: [`docs/comms-protocol.md`](docs/comms-protocol.md#reachable-on-battery).
Dev commands: `!commsreach N|on|off|-` (the `mesh_reachable` line without the card), `!reachtest
pretend|sleep M|off` (on USB, behave as on battery; the sleep a delay with the console alive, or M
minutes of real light sleep), `!reachlog [reset]` (with the missed rings), `!silent on|off`, `!reachchan N [misses]`, `!reachbench MODE N
SLEEPMS LISTENMS` (the radio around light sleep, timed). Measured 2026-11-23: a poll keeps the
board awake about 8 ms when an answer comes (Wi-Fi up in 2 ms, the answer 3 ms after the send) and
19 ms when none does. Tested on USB with `!reachtest pretend` (rings, voice and a message waking it,
PWR, the Clock face's minute, a channel sweep, a key making it an ordinary board); not on battery,
and its battery life is unmeasured: leave a board unplugged overnight and read `!reachlog`.
**System → ACTIONS → REACHABLE** sets it on the board (OFF, 10 S, 30 S, kept over the card's line;
`!commsreach -` hands it back to the card), and **SILENT** (off by default) stops the chirps the
board makes by itself: comms's ring, message, call and mic-off chirps and the agent's
chirps. Alarms (the Clock's, LOST's) still sound, and so does everything you start. A ring that
rings with SILENT on shows an inverted `RING` in the status bar where its sheet can't rise (the
lock, the desk face). With SILENT and REACHABLE both on, a ring nobody answered stays on the off
face (`MISSED RING · INKDECK-0000 RANG 18:02`, on all five faces, through the Clock face's minute
redraws) until a key, PWR or USB. Tested 2026-11-23 on USB the same way (screenshots of both rows
cycling and kept through a restart, a silent ring with no chirp on an awake board, on the desk
face and on a sleeper's Sleep and Clock faces, the mark kept through two minute redraws, cleared
by PWR and by a key).

**Settings:** Wi-Fi, time zone, location and the password for updates over Wi-Fi come from
`.env` at build time (template: `.env.example`). `inkdeck.ini` in the card's root overrides the
Wi-Fi network, time zone and location without a rebuild, sets the lock, lists network shares,
sets how long News's off face shows each story, puts the board on your tailnet and sets its comms groups, the rate it sends at and whether it stays reachable on battery (example: [`inkdeck.ini.example`](inkdeck.ini.example)). The board reads it at startup, and if
the card has none, it writes one with every line commented out. It can list up to 8 Wi-Fi
networks, each `ssid` line with its `password` under it: the board tries the one it last joined
first, then the others in the file's order, moving on as soon as one is out of range (about
3 s). **Passwords don't stay on the card**, since a card is easy to lift: the board keeps each
Wi-Fi password, each SMB share's and the Tailscale key in its flash (NVS) and rewrites their
lines without them. To
change one, write it in again. The flash isn't encrypted, so someone with the board and a USB
cable can read them, as they can the `.env` values built into the firmware.
`INKDECK_FAHRENHEIT` in `platformio.ini` switches between
US and metric units, and `INKDECK_AGENT` builds the agent in (`1`, as set) or leaves
it out (`0`): no AGENT on Home, and HOLD BOOT only raises the player sheet. `INKDECK_TAILNET`
does the same for the tailnet: `0` saves about 123 KB of flash and 7 KB of internal RAM, and
`INKDECK_COMMS` for comms (about 161 KB of flash).

```ini
[wifi]
ssid = HomeNet
password = secret                     ; the board keeps it and replaces the line with a note
ssid = Phone hotspot                  ; tried next if HomeNet is out of range
password = another-secret
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
[agent]
agent_host = macbook                  ; inkharness, here on the tailnet
agent_token = something-long
[news]
news_face_every = 5                   ; minutes per story on News's off face (1, 5, 10, 15)
[tailscale]
tailscale_key = tskey-auth-...        ; the board keeps it and replaces the line with a note
tailscale_name = inkdeck              ; its name on the tailnet
tailscale_exit_node = on              ; offer the board as an exit node (IPv4)
[comms]
mesh_group = 3f9c0e1b7a2d4c65?name=Family  ; a group: the secret, then options (name, via, hub)
mesh_name = KITCHEN                   ; this board's name on the others
mesh_channel = 6                      ; the radio's channel off Wi-Fi
mesh_range = long                     ; send at the long-range rate (500 kbps); normal: 1 Mbps
mesh_reachable = 10                   ; on battery, sleep polling every 10 s instead of powering off (System's REACHABLE overrides it)
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

If the USB port disappears, hold BOOT while power-cycling the board, then upload again. If the
port is there but the board doesn't answer (esptool: "No serial data received"),
`uv run tools/board_usb.py reset` makes the computer re-enumerate it, which brings it back.
`tools/screenshot.py` drives the UI over USB and saves the screen as a PNG (`--no-reset` to
leave the board as it is). `src/devtools.h` lists the serial dev commands.

The image has to fit a 3 MB slot, and a good deal of the build is about that: `platformio.ini`
and `scripts/lto.py` say what each flag saves, and [`AGENTS.md`](AGENTS.md)'s gotchas 9 to 14
what it asks of the code (fonts go through `ui/packed_fonts.h`, sorting through `sorting.h`).
Every link ends with `scripts/check_calls.py`, which fails the build if a call in the firmware's
own code doesn't land on a function: a check on the linker, which shrinks that code after
laying it out.

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
back to the computer for the image, so the firewall must let Python accept incoming connections.
espota's debug output prints the password, so don't paste upload logs anywhere public.

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
  caps wrap the corners, with the Sleep-screen frog and the case revision engraved on the back.
