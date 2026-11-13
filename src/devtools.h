// Serial dev hooks, for driving and checking the UI without touching the board.
// Single-character commands on the USB console:
//   u / d    dial up / down          p / P   press / hold the wheel
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
// Line commands start with '!' and end with a newline:
//   !feed URL   add a podcast feed to SD:/Podcasts/feeds.txt (the Podcasts screen reads it when
//               it next opens)
//   !wxforget   drop the saved weather forecast (the Weather screen shows the room instead until
//               the next sync)
//   !append PATH TEXT   add a line to a file on the card (PATH starts with /sdcard/), e.g.
//               "!append /sdcard/inkdeck.ini timezone = PST8PDT,M3.2.0,M11.1.0"
//   !cat PATH   print a file on the card
//   !rm PATH    delete a file on the card
//   !sdtest     write test patterns to the card from internal RAM, PSRAM and flash, read them
//               back and report wrong bytes (flash in one large write is known to fail)
#pragma once

namespace devtools {

void begin();

}  // namespace devtools
