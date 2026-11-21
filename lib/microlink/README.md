# MicroLink (vendored)

The Tailscale client behind the tailnet (`src/tailnet.h`), by CamM2325:
<https://github.com/CamM2325/microlink>, MIT (`LICENSE`). It bundles
[wireguard-lwip](https://github.com/smartalock/wireguard-lwip) (BSD-3-Clause,
`wireguard/LICENSE`) and an X25519 implementation (`x25519-license.txt`).

MicroLink speaks Tailscale's control protocol (ts2021: Noise over HTTP, register with an auth key,
the map of machines), finds paths with DISCO and STUN, relays through DERP when no direct path
works, enforces the tailnet's access rules on what comes in, and adds a WireGuard interface to
lwIP for the tailnet's 100.64.0.0/10. So the firmware's ordinary sockets reach other machines,
and its servers (Wi-Fi Drive, the updater) answer them. IPv4 inside the tunnel only, no MagicDNS
server (`src/tailnet.cpp` answers names from the map itself), no subnet routes or exit nodes in.

Copied from commit `1cb8d1c` (2026-11-18), component version 2.3.0: upstream's
`components/microlink/` as it is, with `include/` and `src/` here, its bundled
`components/wireguard_lwip/` as `wireguard/`, and its `Kconfig` for reference. Nothing in them is
changed. `library.json` builds what upstream's `CMakeLists.txt` builds, with warnings off (`-w`),
since they're upstream's to fix. The cellular, network-switch, web-config and OTA-rollback sources
compile to nothing: their Kconfig options are off.

Ours, in `port/`:

- `microlink_kconfig.h`: what MicroLink's Kconfig would put in `sdkconfig.h`. The Arduino core
  has ESP-IDF prebuilt, with no menuconfig, so `library.json` force-includes this ahead of every
  MicroLink source. Upstream's defaults, except a 10 s stop timeout (30 s), no device-name prefix
  or priority peer (`src/tailnet.cpp` passes both at run time), and no OTA rollback of its own
  (the firmware's `ota::confirmBoot()` decides). The DERP home region (9, Dallas) and the control
  server (Tailscale's) can be changed with `-D` build flags; the file says how, Headscale included.
- `ml_inkdeck.c`/`.h`: two calls into MicroLink's internals, to wake a peer's WireGuard session
  and ask whether it's up. Its own TCP API does the same before it connects, but keeps the socket.

Upstream builds the crypto (ChaCha20-Poly1305, BLAKE2s, X25519, the DISCO box) at `-O2`. Here it
is `-Os` like the rest. Voice audio is tens of KB/s, far below what it manages either way.

MicroLink keeps the board's machine, node and DISCO keys in NVS (namespace `microlink`, and peers
it has seen in `ml_peers`), unencrypted, like the firmware's other secrets. `src/tailnet.cpp`
erases them (`microlink_factory_reset()`) when the auth key changes.

Memory, measured 2026-11-21: about 123 KB of flash and 7 KB of internal RAM from boot (static
tables: `pending_probes` in `ml_wg_mgr.c`, the DISCO box's key cache in `nacl_box.c`, the
packet filter's flows), then four tasks with 42 KB of stack in internal RAM while the board is on
the tailnet, and a 512 KB buffer in PSRAM while it fetches the map of machines (the long poll's
buffers after that grow only as far as the tailnet's updates need). A build with `INKDECK_TAILNET=0`
(`src/features.h`) links none of it.

To update: clone upstream, copy `components/microlink/{include,src,Kconfig,LICENSE}` and
`components/microlink/components/wireguard_lwip/{src,LICENSE}` here again, compare upstream's
`CMakeLists.txt` with `library.json`'s source list and its `Kconfig` with
`port/microlink_kconfig.h`, change the version in `library.json` and the commit above, build, and
join a tailnet from the board.
