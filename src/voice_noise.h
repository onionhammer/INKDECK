// Protocol 2's encryption (docs/voice-protocol.md, "Encryption"): the board's side of a Noise
// handshake, Noise_NNpsk0_25519_AESGCM_SHA256, keyed from voice_token, then a key per
// direction that seals every frame. On mbedTLS, which the firmware already carries for HTTPS:
// X25519, and AES-GCM and SHA-256 on the S3's AES and SHA units. The daemon's side is
// inkharness/voice-proto/src/crypto.rs, whose reference test runs these same steps against
// the snow crate.
//
// The token never leaves the board: it only derives the key. A daemon with another token
// can't open message 1, and someone without it can't make a message 2 that opens here.
#pragma once
#include <mbedtls/ecp.h>
#include <mbedtls/gcm.h>
#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

namespace noise {

constexpr size_t kKey = 32;  // keys, hashes and X25519 public keys alike
constexpr size_t kTag = 16;  // what sealing adds

// The key both sides derive from the token: HKDF-SHA256, salt "inkharness voice psk".
void psk(const std::string &token, uint8_t out[kKey]);

// One direction of the connection after the handshake. Frames open only in the order they
// were sealed: the nonce counts them.
class Cipher {
 public:
  Cipher() { mbedtls_gcm_init(&gcm_); }
  ~Cipher() { mbedtls_gcm_free(&gcm_); }
  Cipher(const Cipher &) = delete;
  Cipher &operator=(const Cipher &) = delete;

  // Seals `len` bytes at `data` in place and appends the tag: `data` needs len + kTag bytes.
  void seal(uint8_t *data, size_t len);
  // Opens `len` bytes (tag included) in place: the plaintext is the first len - kTag. False if
  // they were changed, replayed, reordered or sealed with another key.
  bool open(uint8_t *data, size_t len);
  void wipe();

 private:
  friend class Initiator;
  void setKey(const uint8_t key[kKey]);

  mbedtls_gcm_context gcm_;
  uint64_t n_ = 0;
  bool keyed_ = false;
};

// The board's half of the handshake: hello() makes message 1, finish() reads message 2.
class Initiator {
 public:
  // Keyed from the token (psk()); the key is wiped with the handshake.
  explicit Initiator(const std::string &token);
  ~Initiator();
  Initiator(const Initiator &) = delete;
  Initiator &operator=(const Initiator &) = delete;

  // Message 1: this connection's ephemeral key, then `payload` (the hello) sealed. Empty if
  // the key couldn't be made.
  std::vector<uint8_t> hello(const std::string &payload);
  // Message 2. True if it was made with the same key: `payload` is then the daemon's answer,
  // and `send` and `recv` hold the keys for the rest of the connection.
  bool finish(const uint8_t *msg, size_t len, std::string &payload, Cipher &send, Cipher &recv);

 private:
  void mixHash(const uint8_t *data, size_t len);
  void mixKey(const uint8_t *ikm, size_t len);
  void mixKeyAndHash(const uint8_t *ikm, size_t len);
  void encryptAndHash(const uint8_t *plain, size_t len, uint8_t *out);
  bool decryptAndHash(const uint8_t *sealed, size_t len, uint8_t *out);

  uint8_t ck_[kKey], h_[kKey], k_[kKey];
  uint64_t n_ = 0;
  mbedtls_ecp_group grp_;
  mbedtls_mpi e_;            // the ephemeral private key
  uint8_t ePub_[kKey] = {};  // and its public half
  bool ok_ = false;          // the ephemeral key was made
};

}  // namespace noise
