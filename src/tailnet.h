// The board on your tailnet (Tailscale, issue #56), through MicroLink (lib/microlink/README.md):
// a Tailscale client for the ESP32 that joins with an auth key and adds a WireGuard interface to
// lwIP. So the firmware's ordinary sockets reach 100.x addresses, and the other machines on the
// tailnet reach the board's servers.
//
// On demand, like Wi-Fi (wlan.h): hold() joins Wi-Fi and the tailnet (the control plane, the
// list of machines, a relay: a few seconds), release() lets go, and the board leaves kLingerMs
// after the last user. What joins it:
//   - the voice daemon, when voice_host is on the tailnet (voice_client.h): inkharness on a
//     machine anywhere, the board on any Wi-Fi;
//   - a share in Files whose server is (shares.h);
//   - an HTTP fetch from a host that is (netget.h): a feed on a home server;
//   - Wi-Fi Drive and Update over Wi-Fi while they're open, so the board answers there too
//     (webdav.h, ota.h).
//
// A host is on the tailnet (routes()) when it's a Tailscale address (100.64.0.0/10), a MagicDNS
// name (*.ts.net) or a name without dots, which is how the tailnet names its machines. While
// the board is on it, getaddrinfo() answers those names from the tailnet's list of machines
// (an lwIP hook, tailnet.cpp), so code that resolves a name needs nothing more. A name the list
// doesn't have goes to DNS as usual.
//
// inkdeck.ini's [tailscale] sets it up (settings.h): an auth key, which the board keeps in NVS
// and takes off the card, and the board's name on the tailnet. MicroLink keeps the board's own
// keys (machine, node, disco) in NVS too ("microlink"), so it stays the same machine across
// power-offs; a different auth key joins it again as a new machine.
#pragma once
#include <stdint.h>

#include <string>

namespace tailnet {

bool configured();  // the board has an auth key

bool routes(const std::string &host);  // reached through the tailnet (above); false unless configured()

// Joins Wi-Fi and the tailnet, or waits for a join under way; true once the board is on it,
// with its address and the list of machines. Each true is matched by a release(). On failure,
// error says why in a few words (TAILSCALE KEY REFUSED). Blocks: call from a background task.
bool hold(uint32_t timeoutMs, std::string &error);
void release();

// hold() and release() for a server that wants to be reachable on the tailnet but mustn't wait
// for it (Wi-Fi Drive, the updater): join() starts a hold in a task of its own and returns at
// once; leave() lets it go, or has the join let go as soon as it's done. One try: if it fails,
// describe() says why. Nothing happens without configured().
void join();
void leave();

// While held: brings up the WireGuard session with the machine `host` names (a tailnet name or
// address), waiting until it's up or timeoutMs passes. A connect() before that loses its first
// packets and waits for TCP to send them again. False if the host isn't on the tailnet's list
// or the session didn't come up in time (connecting may still work).
bool reach(const std::string &host, uint32_t timeoutMs);

// The board's address on the tailnet ("100.101.102.103") while it's on it, else "". Safe from
// any task.
std::string address();
std::string name();  // what it asks to be called (tailscale_name, inkdeck by default)

// For the System screen's TAILNET row: "not set", "off · inkdeck", "joining · inkdeck",
// "on · inkdeck · 100.101.102.103", "KEY REFUSED · inkdeck" (the last join's failure). Safe
// from any task.
std::string describe();

}  // namespace tailnet
