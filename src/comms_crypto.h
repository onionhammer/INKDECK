// A comms group's keys and the sealing of its packets (docs/comms-protocol.md, "Keys"), on
// mbedTLS, which the firmware carries for HTTPS: HKDF-SHA256 from the group's secret, and
// AES-GCM (the S3's AES unit) on every packet.
//
// One secret gives three things, each under a label of its own: the 256-bit key, the group id
// that heads every packet (so a board picks the key without trying them all, and its first 4 hex
// digits show on screen to check boards against each other), and a rendezvous id for daemons to
// meet at a hub by (not used yet). Neither id tells anything about the key.
//
// The default group, made from agent_token when the card has no mesh_group line, uses another
// salt: its key is not the one the agent link uses, and no mesh_group line gives it.
#pragma once
#include <mbedtls/gcm.h>
#include <stddef.h>
#include <stdint.h>

#include <string>

namespace comms {
namespace crypto {

constexpr size_t kKey = 32;
constexpr size_t kRendezvous = 16;

struct GroupKeys {
  uint8_t key[kKey] = {};
  uint32_t id = 0;  // the packet header's group id: the first 4 bytes of its HKDF output
  uint8_t rendezvous[kRendezvous] = {};
};

// HKDF-SHA256 (RFC 5869), extract then expand, one 32-byte block: out gets `len` (up to 32) bytes.
bool hkdf(const uint8_t *salt, size_t saltLen, const uint8_t *ikm, size_t ikmLen,
          const char *info, uint8_t *out, size_t len);

// A group's keys from its secret: a mesh_group line's (fromToken false) or agent_token's.
GroupKeys derive(const std::string &secret, bool fromToken);
// The link group's (comms_mesh.h: the links packets every board reads): from a public secret
// under a salt of its own, so no mesh_group line or agent_token gives it.
GroupKeys linkKeys();

// "7F3A": the group id's first 4 hex digits, as the screens show it.
void tag(uint32_t groupId, char out[5]);

// AES-GCM under one group's key. The frame is the header (comms_wire.h) then the body; the
// nonce comes from the header's sender, boot and seq, and the header (ttl and hops left out) is
// authenticated. Not thread-safe: one task uses each.
class Sealer {
 public:
  Sealer() { mbedtls_gcm_init(&gcm_); }
  ~Sealer() { mbedtls_gcm_free(&gcm_); }
  Sealer(const Sealer &) = delete;
  Sealer &operator=(const Sealer &) = delete;

  bool setKey(const uint8_t key[kKey]);
  bool keyed() const { return keyed_; }
  // Encrypts the body after the header in place and appends the tag. Returns the frame's
  // length (header, body, tag), or 0 if it would be over wire::kMaxFrame.
  size_t seal(uint8_t *frame, size_t bodyLen);
  // Opens a whole frame in place; the body is then at frame + wire::kHeader, `bodyLen` long.
  // False if it was changed or sealed with another key.
  bool open(uint8_t *frame, size_t len, size_t &bodyLen);

 private:
  mbedtls_gcm_context gcm_;
  bool keyed_ = false;
};

}  // namespace crypto
}  // namespace comms
