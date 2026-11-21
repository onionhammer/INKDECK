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
