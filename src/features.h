// Features a build can leave out, set in platformio.ini's build_flags:
//   -DINKDECK_AGENT=1   the agent (agent_client.h, apps/agent.h, apps/agent_sheet.h; the daemon is
//                       inkharness/): Home's AGENT, HOLD BOOT to talk from any screen, the agent
//                       sheet. 0 or unset: none of it is built, and HOLD BOOT only raises the
//                       player sheet while something plays, or talks to the boards on the Comms
//                       screens (apps/hold_boot.h). The old name, -DINKDECK_VOICE, still works.
//   -DINKDECK_BLE=1     Bluetooth LE scans in Sensors (ble_survey.h, apps/ble_devices.h): the scan
//                       panel's BLE stop and the menu's BLUETOOTH LE. It keeps the BLE controller's
//                       memory from boot (about 26 KB of internal RAM, measured 2026-11-19) and adds
//                       about 230 KB of flash. 0 or unset: none of it is built, the core frees that
//                       memory at boot, and Sensors scans Wi-Fi only.
//   -DINKDECK_TAILNET=1 the board on your tailnet (tailnet.h, Tailscale through lib/microlink):
//                       agent_host, shares and feeds on tailnet machines, Wi-Fi Drive and the
//                       updater answering there. MicroLink adds about 123 KB of flash and keeps
//                       about 7 KB of internal RAM from boot (its static tables; measured
//                       2026-11-21), and 42 KB more while the board is on the tailnet. 0 or unset:
//                       none of it is built, and System's TAILNET row says so.
//   -DINKDECK_COMMS=1   COMMS (comms.h, issue #66): boards talking to each other over ESP-NOW,
//                       open mics in groups from the card's mesh_group lines (a board's mic on
//                       or off, several at once, mixed), relaying for each other as a mesh.
//                       Home's COMMS, its screens and the call sheet (apps/comms_ui.h), where
//                       HOLD BOOT switches the mic on while held, for now. It listens on USB
//                       power, while a Comms screen holds it, while this board's mic is on, or
//                       while it's in a conversation where another board's mic is on. 0 or unset:
//                       none of it is built, and the dev commands !comms... are left out.
#pragma once

#if !defined(INKDECK_AGENT) && defined(INKDECK_VOICE)
#define INKDECK_AGENT INKDECK_VOICE  // its name before the Agent
#endif
#ifndef INKDECK_AGENT
#define INKDECK_AGENT 0
#endif

#ifndef INKDECK_BLE
#define INKDECK_BLE 0
#endif

#ifndef INKDECK_TAILNET
#define INKDECK_TAILNET 0
#endif

#ifndef INKDECK_COMMS
#define INKDECK_COMMS 0
#endif
