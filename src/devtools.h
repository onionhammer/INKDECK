// Serial dev hooks, for driving and checking the UI without touching the board.
// Single-character commands on the USB console:
//   u / d    dial up / down          p / P   press / hold the wheel
//   U / D    dial up / down as a fast repeat (the dial held past 1.7 s: lists move a page)
//            All four are the physical wheel, so landscape screens get them swapped (ui/nav.h)
//   b / B    BOOT / hold BOOT        s       dump the framebuffer (tools/screenshot.py)
//   - / +    volume down / up 5 (no redraw; handy to silence the speaker before a test)
//   r        print the PMIC's configuration and status registers (read-only)
//   i        print one raw IMU reading (chip axes)
//   h        print free heap: internal RAM (largest block, lowest so far), DMA-capable, PSRAM
//   c        reset the RTC to 2000-01-01, as after a battery pull (the boot-time sync then
//            runs on the next boot)
//   o        press PWR: the Sleep screen, then power-off (an armed alarm wakes it)
//   z        standby for two wakes: the Clock face, off, redrawn by the next minute's RTC wake,
//            then the board stays on at the one after (so the USB console comes back)
//   k        close the book open in the Reader, as 15 idle minutes on battery do: its cover,
//            then power-off. The RTC boots the board a minute later (so the USB console comes
//            back), and that start opens the book again, under the Clock an RTC wake shows
// Line commands start with '!' and end with a newline:
//   !feed URL   add a podcast feed to SD:/Podcasts/feeds.txt (the Podcasts screen reads it when
//               it next opens)
//   !wxforget   drop the saved weather forecast (the Weather screen shows the room instead until
//               the next sync)
//   !wxmodels A,B,...   log the sky now (weather code, cloud layers) from each Open-Meteo model
//               named, e.g. "!wxmodels best_match,gfs_hrrr,ecmwf_ifs025"
//   !append PATH TEXT   add a line to a file on the card (PATH starts with /sdcard/), e.g.
//               "!append /sdcard/inkdeck.ini timezone = PST8PDT,M3.2.0,M11.1.0"
//   !b64 PATH DATA   append DATA, base64 (a multiple of 4 characters), to a file: binary test
//               files such as pictures, a line at a time
//   !cat PATH   print a file on the card
//   !rm PATH    delete a file on the card
//   !ls PATH    list a folder on the card, with file sizes
//   !cp SRC|DST copy a file, e.g. "!cp /sdcard/Books/A.epub|/ffat/A.epub"
//   !mkdir PATH make a folder
//               PATH may start with /ffat/ (the onboard FAT partition) as well as /sdcard/,
//               or /net/NAME/ for a network share while it's mounted (open it in Files first;
//               read-only), e.g. "!cp /net/Media/Books/A.epub|/sdcard/Books/A.epub"
//   !flow PATH|N   print the start of an EPUB's chapter N (spine item) as the Reader gets it,
//               without opening it in the Reader (its saved place stays put)
//   !unzip PATH[|ENTRY]   list the files in a zip on the card (an EPUB), or print one of them
//               ("!unzip /sdcard/Books/Some Book.epub|OEBPS/style.css")
//   !voiceloop  the voice paths without a network: 3 s of the mic into the capture ring, then
//               the same samples out through the stream ring; prints the bytes and the peak
//   !sdtest     write test patterns to the card from internal RAM, PSRAM and flash, read them
//               back and report wrong bytes (flash in one large write is known to fail)
//   !wom [N [MG]]   arm the IMU's wake on motion for N seconds (10) at a threshold of MG
//               (100 mg, as the picture viewer's nap does) and print each event; then the IMU
//               goes back to normal
#pragma once

namespace devtools {

void begin();

}  // namespace devtools
