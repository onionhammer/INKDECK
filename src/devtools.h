// Serial dev hooks, for driving and checking the UI without touching the board.
// Single-character commands on the USB console:
//   u / d    dial up / down          p / P   press / hold the wheel
//   b / B    BOOT / hold BOOT        s       dump the framebuffer (tools/screenshot.py)
//   - / +    volume down / up 5 (no redraw; handy to silence the speaker before a test)
//   r        print the PMIC's configuration and status registers (read-only)
//   i        print one raw IMU reading (chip axes)
//   c        reset the RTC to 2000-01-01, as after a battery pull (the boot-time sync then
//            runs on the next boot)
#pragma once

namespace devtools {

void begin();

}  // namespace devtools
