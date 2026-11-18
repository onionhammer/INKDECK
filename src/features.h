// Features a build can leave out, set in platformio.ini's build_flags:
//   -DINKDECK_VOICE=1   the voice assistant (apps/voice.h, voice_client.h; the daemon is inkharness/):
//                       Home's VOICE, HOLD BOOT to talk from any screen, the voice sheet. 0 or
//                       unset: none of it is built, and HOLD BOOT only raises the player sheet
//                       while something plays (apps/hold_boot.h).
#pragma once

#ifndef INKDECK_VOICE
#define INKDECK_VOICE 0
#endif
