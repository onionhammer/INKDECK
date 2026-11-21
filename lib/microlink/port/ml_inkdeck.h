// What src/tailnet.cpp needs from MicroLink's internals (microlink_internal.h): waking a peer's
// WireGuard session, as microlink_tcp_connect() does before its connect() but without the socket
// it makes (the firmware connects with ordinary lwIP sockets, which reach a 100.x address through
// MicroLink's WireGuard interface once the session is up), the DERP regions in play, and whether
// it's an exit node.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "microlink.h"

#ifdef __cplusplus
extern "C" {
#endif

// Asks the WireGuard manager for a handshake (and a CallMeMaybe) with this peer. vpn_ip in host
// byte order. Doesn't wait.
bool ml_inkdeck_wake_peer(microlink_t *ml, uint32_t vpn_ip);

// The WireGuard session with this peer is up.
bool ml_inkdeck_peer_is_up(microlink_t *ml, uint32_t vpn_ip);

// The DERP region MicroLink calls home at its next start (port/microlink_kconfig.h); 9 until set.
extern uint16_t ml_inkdeck_derp_region;
// False when a -DCONFIG_ML_DERP_REGION build flag fixes the region, so setting it does nothing.
bool ml_inkdeck_derp_auto(void);
// The region the running instance calls home; 0 before the control plane's first map.
uint16_t ml_inkdeck_home_region(microlink_t *ml);
// Forgets it, so the next start takes ml_inkdeck_derp_region (unless the control plane names
// one). Only while stopped: the instance keeps it across microlink_stop()/microlink_start().
void ml_inkdeck_forget_home_region(microlink_t *ml);
// The region a peer is homed in; 0 if it isn't a peer or the map didn't say.
uint16_t ml_inkdeck_peer_region(microlink_t *ml, uint32_t vpn_ip);
// The region most peers are homed in, counting online ones only if any are; 0 if none says.
uint16_t ml_inkdeck_common_region(microlink_t *ml);

// The private IPv4 addresses (10/8, 172.16/12, 192.168/16) among a peer's endpoints, as the map
// gave them: its LAN addresses, a Docker bridge's. Host byte order, at most `max`; the count.
int ml_inkdeck_peer_lan_addresses(microlink_t *ml, uint32_t vpn_ip, uint32_t *out, int max);

// The instance offers itself as an exit node: 0.0.0.0/0 was among its advertise_routes, and the
// WireGuard interface's NAPT came up (MicroLink drops the routes for good when it can't, rather
// than forward with tailnet source addresses).
bool ml_inkdeck_exit_node(microlink_t *ml);

#ifdef __cplusplus
}
#endif
