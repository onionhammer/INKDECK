#include "ml_inkdeck.h"

#include "microlink_internal.h"

uint16_t ml_inkdeck_derp_region = 9;  // upstream's default, until tailnet.cpp knows better

bool ml_inkdeck_wake_peer(microlink_t *ml, uint32_t vpn_ip) {
    return ml && ml_wg_mgr_post_request(ml, ML_PEER_REQ_HANDSHAKE, vpn_ip) == ESP_OK;
}

bool ml_inkdeck_peer_is_up(microlink_t *ml, uint32_t vpn_ip) {
    return ml && ml_wg_mgr_peer_is_up(ml, vpn_ip);
}

bool ml_inkdeck_derp_auto(void) {
#ifdef ML_INKDECK_DERP_AUTO
    return true;
#else
    return false;
#endif
}

uint16_t ml_inkdeck_home_region(microlink_t *ml) {
    return ml ? ml->derp_home_region : 0;
}

void ml_inkdeck_forget_home_region(microlink_t *ml) {
    if (ml) ml->derp_home_region = 0;
}

uint16_t ml_inkdeck_peer_region(microlink_t *ml, uint32_t vpn_ip) {
    if (!ml || !vpn_ip) return 0;
    for (int i = 0; i < ml->peer_count; i++) {
        if (ml->peers[i].active && ml->peers[i].vpn_ip == vpn_ip) return ml->peers[i].derp_region;
    }
    return 0;
}

uint16_t ml_inkdeck_common_region(microlink_t *ml) {
    if (!ml) return 0;
    bool any_online = false;
    for (int i = 0; i < ml->peer_count; i++) {
        if (ml->peers[i].active && ml->peers[i].online && ml->peers[i].derp_region) any_online = true;
    }
    uint16_t best = 0;
    int best_count = 0;
    for (int i = 0; i < ml->peer_count; i++) {
        const ml_peer_t *p = &ml->peers[i];
        if (!p->active || !p->derp_region || (any_online && !p->online)) continue;
        int count = 0;
        for (int j = 0; j < ml->peer_count; j++) {
            const ml_peer_t *q = &ml->peers[j];
            if (q->active && q->derp_region == p->derp_region && (!any_online || q->online)) count++;
        }
        if (count > best_count) {
            best = p->derp_region;
            best_count = count;
        }
    }
    return best;
}

static bool is_private_v4(uint32_t ip) {
    return (ip >> 24) == 10 || (ip >> 20) == 0xAC1 || (ip >> 16) == 0xC0A8;
}

int ml_inkdeck_peer_lan_addresses(microlink_t *ml, uint32_t vpn_ip, uint32_t *out, int max) {
    if (!ml || !vpn_ip || !out) return 0;
    int n = 0;
    for (int i = 0; i < ml->peer_count; i++) {
        const ml_peer_t *p = &ml->peers[i];
        if (!p->active || p->vpn_ip != vpn_ip) continue;
        for (int e = 0; e < p->endpoint_count && n < max; e++) {
            const uint32_t ip = p->endpoints[e].ip;
            if (p->endpoints[e].is_ipv6 || !is_private_v4(ip)) continue;
            bool seen = false;
            for (int k = 0; k < n; k++) seen = seen || out[k] == ip;
            if (!seen) out[n++] = ip;
        }
        break;
    }
    return n;
}

bool ml_inkdeck_exit_node(microlink_t *ml) {
    return ml && ml_routes_is_exit_node(ml->routes, ml->route_count);
}
