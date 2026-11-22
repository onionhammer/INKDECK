#include "comms_crypto.h"

#include "features.h"

#if INKDECK_COMMS  // the whole file (features.h)

#include <mbedtls/hkdf.h>
#include <mbedtls/md.h>
#include <mbedtls/platform_util.h>
#include <stdio.h>
#include <string.h>

#include "comms_wire.h"

namespace comms {
namespace crypto {
namespace {

// The salts and labels, exactly as docs/comms-protocol.md has them.
constexpr char kSalt[] = "inkdeck comms 1";
constexpr char kTokenSalt[] = "inkdeck comms 1 agent_token";
constexpr char kKeyInfo[] = "key";
constexpr char kIdInfo[] = "group id";
constexpr char kRendezvousInfo[] = "rendezvous id";

}  // namespace

bool hkdf(const uint8_t *salt, size_t saltLen, const uint8_t *ikm, size_t ikmLen,
          const char *info, uint8_t *out, size_t len) {
  if (len > kKey) return false;
  return mbedtls_hkdf(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), salt, saltLen, ikm, ikmLen,
                      reinterpret_cast<const uint8_t *>(info), strlen(info), out, len) == 0;
}

GroupKeys derive(const std::string &secret, bool fromToken) {
  GroupKeys k;
  const char *salt = fromToken ? kTokenSalt : kSalt;
  const auto *s = reinterpret_cast<const uint8_t *>(salt);
  const size_t sl = strlen(salt);
  const auto *ikm = reinterpret_cast<const uint8_t *>(secret.data());
  uint8_t id[4];
  hkdf(s, sl, ikm, secret.size(), kKeyInfo, k.key, kKey);
  hkdf(s, sl, ikm, secret.size(), kIdInfo, id, sizeof id);
  hkdf(s, sl, ikm, secret.size(), kRendezvousInfo, k.rendezvous, kRendezvous);
  // Big-endian, so the tag reads as the bytes do: id 7F 3A ... shows as 7F3A.
  k.id = static_cast<uint32_t>(id[0]) << 24 | static_cast<uint32_t>(id[1]) << 16 |
         static_cast<uint32_t>(id[2]) << 8 | id[3];
  return k;
}

void tag(uint32_t groupId, char out[5]) {
  snprintf(out, 5, "%04X", static_cast<unsigned>(groupId >> 16));
}

bool Sealer::setKey(const uint8_t key[kKey]) {
  mbedtls_gcm_free(&gcm_);
  mbedtls_gcm_init(&gcm_);
  keyed_ = mbedtls_gcm_setkey(&gcm_, MBEDTLS_CIPHER_ID_AES, key, kKey * 8) == 0;
  return keyed_;
}

size_t Sealer::seal(uint8_t *frame, size_t bodyLen) {
  using namespace wire;
  if (!keyed_ || bodyLen > kMaxBody) return 0;
  uint8_t iv[kNonce], aad[kHeader];
  nonce(get32(frame + 8), get32(frame + 12), get32(frame + 16), iv);
  associatedData(frame, aad);
  uint8_t *body = frame + kHeader;
  if (mbedtls_gcm_crypt_and_tag(&gcm_, MBEDTLS_GCM_ENCRYPT, bodyLen, iv, sizeof iv, aad,
                                sizeof aad, body, body, kTag, body + bodyLen) != 0) {
    return 0;
  }
  return kHeader + bodyLen + kTag;
}

bool Sealer::open(uint8_t *frame, size_t len, size_t &bodyLen) {
  using namespace wire;
  if (!keyed_ || len < kHeader + kTag || len > kMaxFrame) return false;
  bodyLen = len - kHeader - kTag;
  uint8_t iv[kNonce], aad[kHeader];
  nonce(get32(frame + 8), get32(frame + 12), get32(frame + 16), iv);
  associatedData(frame, aad);
  uint8_t *body = frame + kHeader;
  return mbedtls_gcm_auth_decrypt(&gcm_, bodyLen, iv, sizeof iv, aad, sizeof aad,
                                  body + bodyLen, kTag, body, body) == 0;
}

}  // namespace crypto
}  // namespace comms

#endif  // INKDECK_COMMS
