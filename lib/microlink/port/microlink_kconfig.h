// What MicroLink's Kconfig would add to sdkconfig.h in an ESP-IDF build. The Arduino core comes
// with ESP-IDF prebuilt, so there is no menuconfig: library.json includes this ahead of every
// MicroLink source (-include). The values are upstream's defaults (Kconfig, next to this folder)
// unless a comment says otherwise. The ones in #ifndef can be overridden with a -D in
// platformio.ini's build_flags, which reaches libraries too.
#pragma once
#include <stdint.h>

// --- Peers and buffers ----------------------------------------------------------------------

#ifndef CONFIG_ML_MAX_PEERS
#define CONFIG_ML_MAX_PEERS 16  // WireGuard peers at once; the least recently used one goes
#endif
#ifndef CONFIG_ML_NVS_MAX_PEERS
#define CONFIG_ML_NVS_MAX_PEERS 64  // peers cached in NVS for a quicker start
#endif
// The largest map response taken from the control plane (HTTP/2 frames, then their JSON), about
// 3 KB a machine: the first fetch allocates the first in PSRAM for its length, and the long poll's
// buffers grow up to these.
#ifndef CONFIG_ML_H2_BUFFER_SIZE_KB
#define CONFIG_ML_H2_BUFFER_SIZE_KB 512
#endif
#ifndef CONFIG_ML_JSON_BUFFER_SIZE_KB
#define CONFIG_ML_JSON_BUFFER_SIZE_KB 512
#endif
#ifndef CONFIG_ML_FILTER_MAX_ENTRIES
#define CONFIG_ML_FILTER_MAX_ENTRIES 4096
#endif
#define CONFIG_ML_PRIORITY_PEER_IP ""  // tailnet.cpp passes one at run time instead

// --- Relays, control plane ------------------------------------------------------------------

// The DERP relay region the board calls home: it advertises it (PreferredDERP) and keeps its
// one relay connection there, and MicroLink relays to every peer through it. A peer homed in
// another region never hears a relayed packet, so this must be the region the board's peers use.
// Kconfig makes it a constant (upstream's default: 9, Dallas); here it's a variable, which
// src/tailnet.cpp sets before each start: the voice host's region, else the one most machines
// use, learned at the first join and kept in NVS (ml_inkdeck.h). -DCONFIG_ML_DERP_REGION=N fixes
// it at N instead (https://login.tailscale.com/derpmap/default has the numbers).
#ifndef CONFIG_ML_DERP_REGION
extern uint16_t ml_inkdeck_derp_region;
#define CONFIG_ML_DERP_REGION ml_inkdeck_derp_region
#define ML_INKDECK_DERP_AUTO 1
#endif
#define CONFIG_ML_DERP_VERIFY_CERT 1  // the core has the certificate bundle

// Tailscale's control plane, over port 80 (Noise inside, as tailscaled does). A Headscale
// server listening on port 80 works with both of these as build flags:
// -DCONFIG_ML_CTRL_HOST=\"headscale.example.com\" and -DCONFIG_ML_CTRL_NOISE_PUBKEY_HEX=\"...\"
// (the 64 hex digits after mkey: in https://<server>/key?v=131).
#ifndef CONFIG_ML_CTRL_HOST
#define CONFIG_ML_CTRL_HOST "controlplane.tailscale.com"
#endif
#ifndef CONFIG_ML_CTRL_NOISE_PUBKEY_HEX
#define CONFIG_ML_CTRL_NOISE_PUBKEY_HEX ""
#endif

// --- Behaviour ------------------------------------------------------------------------------

#define CONFIG_ML_PACKET_FILTER_ENFORCE 1  // the tailnet's access rules decide what reaches us
#define CONFIG_ML_UDP_RX_TASK_PRIO 7
#define CONFIG_ML_STOP_TIMEOUT_MS 10000  // upstream: 30000. Its tasks stop within a second or two
#define CONFIG_ML_DEVICE_NAME ""         // tailnet.cpp always names the board

// Left out (undefined, as Kconfig leaves an option that's off): CONFIG_ML_ZERO_COPY_WG,
// CONFIG_ML_SUBNET_ROUTER, CONFIG_ML_ROUTER_IPV6, CONFIG_ML_ENABLE_CELLULAR,
// CONFIG_ML_ENABLE_NET_SWITCH, CONFIG_ML_ENABLE_CONFIG_HTTPD, CONFIG_ML_CTRL_TLS,
// CONFIG_ML_CTRL_STREAMING_MAP_FETCH, and CONFIG_ML_OTA_ROLLBACK: the firmware keeps or rolls
// back an update itself (ota::confirmBoot()), and MicroLink's would mark it valid on its own.
