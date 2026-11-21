// Two calls from MicroLink's internals (microlink_internal.h), for src/tailnet.cpp: what
// microlink_tcp_connect() does before its connect(), without the socket it makes. The firmware
// connects with ordinary lwIP sockets, which reach a 100.x address through MicroLink's
// WireGuard interface once the session with that peer is up.
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

#ifdef __cplusplus
}
#endif
