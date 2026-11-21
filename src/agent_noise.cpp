#include "features.h"

#if INKDECK_AGENT  // the whole file (features.h)

#include "agent_noise.h"

#include <esp_random.h>
#include <mbedtls/ecdh.h>
#include <mbedtls/md.h>
#include <mbedtls/platform_util.h>
#include <mbedtls/sha256.h>
#include <string.h>

namespace noise {
namespace {

// The protocol's full name and the prologue, exactly as crypto.rs has them.
constexpr char kName[] = "Noise_NNpsk0_25519_AESGCM_SHA256";
constexpr char kPrologue[] = "inkharness voice 2";
constexpr char kPskSalt[] = "inkharness voice psk";
static_assert(sizeof kName - 1 <= kKey, "a longer name is hashed into h, not copied");

int fillRandom(void *, unsigned char *buf, size_t len) {
  esp_fill_random(buf, len);  // Wi-Fi is up while connecting: the RNG is a true one then
  return 0;
}

// HMAC-SHA256 over the parts, one after the other.
void hmac(const uint8_t *key, size_t keyLen, const uint8_t *a, size_t aLen, const uint8_t *b,
          size_t bLen, uint8_t out[kKey]) {
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
  mbedtls_md_hmac_starts(&ctx, key, keyLen);
  if (aLen) mbedtls_md_hmac_update(&ctx, a, aLen);
  if (bLen) mbedtls_md_hmac_update(&ctx, b, bLen);
  mbedtls_md_hmac_finish(&ctx, out);
  mbedtls_md_free(&ctx);
}

// Noise's HKDF: up to three outputs from the chaining key and the input (out3 may be null).
// out1 may be the chaining key itself.
void hkdf(const uint8_t ck[kKey], const uint8_t *ikm, size_t ikmLen, uint8_t out1[kKey],
          uint8_t out2[kKey], uint8_t out3[kKey]) {
  uint8_t temp[kKey];
  hmac(ck, kKey, ikm, ikmLen, nullptr, 0, temp);
  const uint8_t one = 1, two = 2, three = 3;
  hmac(temp, kKey, &one, 1, nullptr, 0, out1);
  hmac(temp, kKey, out1, kKey, &two, 1, out2);
  if (out3) hmac(temp, kKey, out2, kKey, &three, 1, out3);
  mbedtls_platform_zeroize(temp, sizeof temp);
}

// AESGCM's nonce: 4 zero bytes, then the counter big-endian.
void nonce(uint64_t n, uint8_t out[12]) {
  memset(out, 0, 4);
  for (int i = 0; i < 8; ++i) out[11 - i] = static_cast<uint8_t>(n >> (8 * i));
}

}  // namespace

void psk(const std::string &token, uint8_t out[kKey]) {
  // HKDF-SHA256: extract with the salt, then one block of expand with no info.
  uint8_t prk[kKey];
  hmac(reinterpret_cast<const uint8_t *>(kPskSalt), sizeof kPskSalt - 1,
       reinterpret_cast<const uint8_t *>(token.data()), token.size(), nullptr, 0, prk);
  const uint8_t one = 1;
  hmac(prk, kKey, &one, 1, nullptr, 0, out);
  mbedtls_platform_zeroize(prk, sizeof prk);
}

// --- Cipher ----------------------------------------------------------------------------------

void Cipher::setKey(const uint8_t key[kKey]) {
  mbedtls_gcm_free(&gcm_);
  mbedtls_gcm_init(&gcm_);
  keyed_ = mbedtls_gcm_setkey(&gcm_, MBEDTLS_CIPHER_ID_AES, key, kKey * 8) == 0;
  n_ = 0;
}

void Cipher::seal(uint8_t *data, size_t len) {
  uint8_t iv[12];
  nonce(n_++, iv);
  mbedtls_gcm_crypt_and_tag(&gcm_, MBEDTLS_GCM_ENCRYPT, len, iv, sizeof iv, nullptr, 0, data,
                            data, kTag, data + len);
}

bool Cipher::open(uint8_t *data, size_t len) {
  if (!keyed_ || len < kTag) return false;
  uint8_t iv[12];
  nonce(n_, iv);
  if (mbedtls_gcm_auth_decrypt(&gcm_, len - kTag, iv, sizeof iv, nullptr, 0, data + len - kTag,
                               kTag, data, data) != 0) {
    return false;  // the counter stays: nothing after this opens either, and the link drops
  }
  ++n_;
  return true;
}

void Cipher::wipe() {
  mbedtls_gcm_free(&gcm_);  // zeroes the key schedule
  mbedtls_gcm_init(&gcm_);
  keyed_ = false;
  n_ = 0;
}

// --- Handshake -------------------------------------------------------------------------------

Initiator::Initiator(const std::string &token) {
  // InitializeSymmetric: the name, padded with zeros, is h; ck starts as h.
  memset(h_, 0, kKey);
  memcpy(h_, kName, sizeof kName - 1);
  memcpy(ck_, h_, kKey);
  memset(k_, 0, kKey);
  mixHash(reinterpret_cast<const uint8_t *>(kPrologue), sizeof kPrologue - 1);
  // NNpsk0's first token: the key from the token goes in before anything is sent.
  uint8_t key[kKey];
  psk(token, key);
  mixKeyAndHash(key, kKey);
  mbedtls_platform_zeroize(key, sizeof key);
  // This connection's ephemeral key.
  mbedtls_ecp_group_init(&grp_);
  mbedtls_mpi_init(&e_);
  mbedtls_ecp_point pub;
  mbedtls_ecp_point_init(&pub);
  size_t len = 0;
  ok_ = mbedtls_ecp_group_load(&grp_, MBEDTLS_ECP_DP_CURVE25519) == 0 &&
        mbedtls_ecp_gen_keypair(&grp_, &e_, &pub, fillRandom, nullptr) == 0 &&
        mbedtls_ecp_point_write_binary(&grp_, &pub, MBEDTLS_ECP_PF_UNCOMPRESSED, &len, ePub_,
                                       kKey) == 0 &&
        len == kKey;  // X25519's public key: the u-coordinate, little-endian
  mbedtls_ecp_point_free(&pub);
}

Initiator::~Initiator() {
  mbedtls_platform_zeroize(ck_, sizeof ck_);
  mbedtls_platform_zeroize(k_, sizeof k_);
  mbedtls_mpi_free(&e_);  // zeroes it
  mbedtls_ecp_group_free(&grp_);
}

void Initiator::mixHash(const uint8_t *data, size_t len) {
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  mbedtls_sha256_update(&ctx, h_, kKey);
  mbedtls_sha256_update(&ctx, data, len);
  mbedtls_sha256_finish(&ctx, h_);
  mbedtls_sha256_free(&ctx);
}

void Initiator::mixKey(const uint8_t *ikm, size_t len) {
  hkdf(ck_, ikm, len, ck_, k_, nullptr);
  n_ = 0;
}

void Initiator::mixKeyAndHash(const uint8_t *ikm, size_t len) {
  uint8_t h[kKey];
  hkdf(ck_, ikm, len, ck_, h, k_);
  mixHash(h, kKey);
  n_ = 0;
}

// The handshake's AEAD: k and its counter, with h as the associated data.
void Initiator::encryptAndHash(const uint8_t *plain, size_t len, uint8_t *out) {
  mbedtls_gcm_context gcm;
  mbedtls_gcm_init(&gcm);
  mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, k_, kKey * 8);
  uint8_t iv[12];
  nonce(n_++, iv);
  mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, len, iv, sizeof iv, h_, kKey, plain, out,
                            kTag, out + len);
  mbedtls_gcm_free(&gcm);
  mixHash(out, len + kTag);
}

bool Initiator::decryptAndHash(const uint8_t *sealed, size_t len, uint8_t *out) {
  if (len < kTag) return false;
  mbedtls_gcm_context gcm;
  mbedtls_gcm_init(&gcm);
  mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, k_, kKey * 8);
  uint8_t iv[12];
  nonce(n_, iv);
  const bool ok = mbedtls_gcm_auth_decrypt(&gcm, len - kTag, iv, sizeof iv, h_, kKey,
                                           sealed + len - kTag, kTag, sealed, out) == 0;
  mbedtls_gcm_free(&gcm);
  if (!ok) return false;
  ++n_;
  mixHash(sealed, len);
  return true;
}

std::vector<uint8_t> Initiator::hello(const std::string &payload) {
  if (!ok_) return {};
  // e: sent in the clear, and (in a psk pattern) mixed into the key as well as the hash.
  std::vector<uint8_t> msg(kKey + payload.size() + kTag);
  memcpy(msg.data(), ePub_, kKey);
  mixHash(ePub_, kKey);
  mixKey(ePub_, kKey);
  encryptAndHash(reinterpret_cast<const uint8_t *>(payload.data()), payload.size(),
                 msg.data() + kKey);
  return msg;
}

bool Initiator::finish(const uint8_t *msg, size_t len, std::string &payload, Cipher &send,
                       Cipher &recv) {
  if (!ok_ || len < kKey + kTag) return false;
  // e: the daemon's ephemeral key, then ee, X25519 of the two.
  mixHash(msg, kKey);
  mixKey(msg, kKey);
  mbedtls_ecp_point re;
  mbedtls_ecp_point_init(&re);
  mbedtls_mpi shared;
  mbedtls_mpi_init(&shared);
  uint8_t ee[kKey];
  // compute_shared refuses a low-order point (a zero result)
  const bool dh = mbedtls_ecp_point_read_binary(&grp_, &re, msg, kKey) == 0 &&
                  mbedtls_ecdh_compute_shared(&grp_, &shared, &re, &e_, fillRandom, nullptr) == 0 &&
                  mbedtls_mpi_write_binary_le(&shared, ee, kKey) == 0;
  mbedtls_ecp_point_free(&re);
  mbedtls_mpi_free(&shared);
  if (!dh) return false;
  mixKey(ee, kKey);
  mbedtls_platform_zeroize(ee, sizeof ee);
  // The payload: the daemon's answer.
  const size_t sealedLen = len - kKey;
  payload.assign(sealedLen - kTag, '\0');
  if (!decryptAndHash(msg + kKey, sealedLen, reinterpret_cast<uint8_t *>(&payload[0]))) {
    payload.clear();
    return false;
  }
  // Split: the first key is for what the board sends, the second for what it receives.
  uint8_t k1[kKey], k2[kKey];
  hkdf(ck_, nullptr, 0, k1, k2, nullptr);
  send.setKey(k1);
  recv.setKey(k2);
  mbedtls_platform_zeroize(k1, sizeof k1);
  mbedtls_platform_zeroize(k2, sizeof k2);
  return send.keyed_ && recv.keyed_;
}

}  // namespace noise

#endif  // INKDECK_AGENT
