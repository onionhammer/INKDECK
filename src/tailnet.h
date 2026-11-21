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
//     (webdav.h, ota.h);
//   - USB power (begin()): plugged in, the board stays on the tailnet, Wi-Fi with it, so the
//     other machines always find it; on battery it joins only for the above.
//
// A host is on the tailnet (routes()) when it's a Tailscale address (100.64.0.0/10), a MagicDNS
// name (*.ts.net) or a name without dots, which is how the tailnet names its machines. The voice
// client and shares try such a host on the LAN first (connectLan(): at home it answers there in
// milliseconds, with no join) and hold the tailnet only when nothing does. While
// the board is on it, getaddrinfo() answers those names from the tailnet's list of machines
// (an lwIP hook, tailnet.cpp), so code that resolves a name needs nothing more. A name the list
// doesn't have goes to DNS as usual.
//
// inkdeck.ini's [tailscale] sets it up (settings.h): an auth key, which the board keeps in NVS
// and takes off the card, and the board's name on the tailnet. MicroLink keeps the board's own
// keys (machine, node, disco) in NVS too ("microlink"), so it stays the same machine across
// power-offs; a different auth key joins it again as a new machine.
//
// tailscale_exit_node = on makes the board an exit node: it advertises 0.0.0.0/0, and once that's
// approved in the admin console, machines that pick it send their internet traffic through it,
// out of its Wi-Fi with its LAN address as the source (lwIP's NAPT, port/microlink_kconfig.h).
// IPv4 only: the core's lwIP can't forward IPv6, so a machine's IPv6 through it goes nowhere and
// it falls back to IPv4. The tailnet's access rules decide where it forwards to, as for what
// reaches the board. It's an exit node while it's on the tailnet: all the time on USB power
// (begin()), where it also keeps Wi-Fi's modem sleep off, and on battery only while something
// holds it.
#pragma once
#include <stdint.h>

#include <string>

namespace tailnet {

bool configured();  // the board has an auth key

// Starts the watcher that holds the tailnet while the board is on USB power and lets it go when
// it's unplugged. After settings::loadCard(); does nothing without an auth key.
void begin();

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

// At home a tailnet machine is usually on the same LAN, and quicker to reach there: no join.
// connectLan() tries, all at once, the address the LAN's own DNS gives the name (a router often
// knows its machines by name; the tailnet's names stay out of it), and the private IPv4 addresses
// the machine gave the tailnet the last time reach() met it (kept in NVS by host: its LAN ones,
// a Docker bridge's). It returns the first socket that connects to `port` (blocking, no options
// set), with the address in `address` if given, or -1: nothing answered in timeoutMs, or there
// was nothing to try. On another network nothing answers, and the caller goes through the
// tailnet. Needs Wi-Fi up but no hold(). Blocks (the DNS lookup too): call from a background
// task. Whoever answers on the LAN isn't vouched for by the tailnet, so use it for links that
// authenticate the other end themselves (the voice token, SMB's password).
int connectLan(const std::string &host, uint16_t port, uint32_t timeoutMs,
               std::string *address = nullptr);

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
// "on · inkdeck · 100.101.102.103" (with " · exit node" when it offers itself as one),
// "KEY REFUSED · inkdeck" (the last join's failure). Safe from any task.
std::string describe();

}  // namespace tailnet
