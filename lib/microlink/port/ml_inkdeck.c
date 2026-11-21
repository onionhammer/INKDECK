#include "ml_inkdeck.h"

#include "microlink_internal.h"

bool ml_inkdeck_wake_peer(microlink_t *ml, uint32_t vpn_ip) {
    return ml && ml_wg_mgr_post_request(ml, ML_PEER_REQ_HANDSHAKE, vpn_ip) == ESP_OK;
}

bool ml_inkdeck_peer_is_up(microlink_t *ml, uint32_t vpn_ip) {
    return ml && ml_wg_mgr_peer_is_up(ml, vpn_ip);
}
