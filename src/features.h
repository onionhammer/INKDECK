// Features a build can leave out, set in platformio.ini's build_flags:
//   -DINKDECK_VOICE=1   the voice assistant (apps/voice.h, voice_client.h; the daemon is inkharness/):
//                       Home's VOICE, HOLD BOOT to talk from any screen, the voice sheet. 0 or
//                       unset: none of it is built, and HOLD BOOT only raises the player sheet
//                       while something plays (apps/hold_boot.h).
//   -DINKDECK_BLE=1     Bluetooth LE scans in Sensors (ble_survey.h, apps/ble_devices.h): the scan
//                       panel's BLE stop and the menu's BLUETOOTH LE. It keeps the BLE controller's
//                       memory from boot (about 26 KB of internal RAM, measured 2026-11-19) and adds
//                       about 230 KB of flash. 0 or unset: none of it is built, the core frees that
//                       memory at boot, and Sensors scans Wi-Fi only.
#pragma once

#ifndef INKDECK_VOICE
#define INKDECK_VOICE 0
#endif

#ifndef INKDECK_BLE
#define INKDECK_BLE 0
#endif
