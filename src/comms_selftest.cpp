// comms::selfTest() (!commstest): the parts of comms that need no radio, each check printed PASS
// or FAIL. The expected bytes come from a separate implementation (Python's hmac, hashlib and
// cryptography), so they also pin docs/comms-protocol.md: another implementation that makes
// the same frame reads this board's packets.
#include "comms.h"

#if INKDECK_COMMS  // the whole file (features.h)

#include <Arduino.h>
#include <math.h>
#include <string.h>

#include <memory>
#include <vector>

#include "adpcm.h"
#include "comms_crypto.h"
#include "comms_wire.h"

namespace comms {
namespace {

using namespace wire;

std::vector<uint8_t> unhex(const char *s) {
  std::vector<uint8_t> out;
  for (; s[0] && s[1]; s += 2) {
    auto nib = [](char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; };
    out.push_back(static_cast<uint8_t>(nib(s[0]) << 4 | nib(s[1])));
  }
  return out;
}

struct Checker {
  Print &out;
  int failed = 0;
  void check(bool ok, const char *what) {
    out.printf("commstest: %s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failed;
  }
};

constexpr char kSecret[] = "correct horse battery staple";

void testHkdf(Checker &c) {
  // RFC 5869, test case 1: the first 32 bytes of its OKM.
  const std::vector<uint8_t> ikm(22, 0x0b);
  uint8_t salt[13];
  for (int i = 0; i < 13; ++i) salt[i] = static_cast<uint8_t>(i);
  const char info[] = "\xf0\xf1\xf2\xf3\xf4\xf5\xf6\xf7\xf8\xf9";
  uint8_t okm[32];
  const bool ran = crypto::hkdf(salt, sizeof salt, ikm.data(), ikm.size(), info, okm, 32);
  const std::vector<uint8_t> want =
      unhex("3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf");
  c.check(ran && memcmp(okm, want.data(), 32) == 0, "HKDF-SHA256 matches RFC 5869 case 1");

  const crypto::GroupKeys k = crypto::derive(kSecret, false);
  c.check(memcmp(k.key, unhex("304cb698315df95320f1c497cdc6a392940c46af8009bb2883438fa2568baf2f")
                            .data(),
                 32) == 0 &&
              k.id == 0x8e363ee1u &&
              memcmp(k.rendezvous, unhex("9ad83e8a1a71e3ed30be224b4403b778").data(), 16) == 0,
          "group key, id and rendezvous id from a secret");
  char tag[5];
  crypto::tag(k.id, tag);
  c.check(strcmp(tag, "8E36") == 0, "the tag shown is the id's first 4 hex digits");
  const crypto::GroupKeys t = crypto::derive(kSecret, true);
  c.check(t.id == 0x098b2a7bu && memcmp(t.key, k.key, 32) != 0,
          "agent_token's group uses another salt");
}

void testGcm(Checker &c) {
  const crypto::GroupKeys k = crypto::derive(kSecret, false);
  std::unique_ptr<crypto::Sealer> s(new crypto::Sealer());
  std::unique_ptr<crypto::Sealer> other(new crypto::Sealer());
  s->setKey(k.key);
  other->setKey(crypto::derive("another secret entirely", false).key);
  // A presence from sender 11223344, boot 7, seq 1: KITCHEN, flags 3, battery 87.
  uint8_t f[kMaxFrame];
  Header h;
  h.type = kPresence;
  h.ttl = 3;
  h.group = k.id;
  h.sender = 0x11223344;
  h.boot = 7;
  h.seq = 1;
  putHeader(h, f);
  Writer w(f + kHeader, kMaxBody);
  w.u32(0);
  w.u8(3);
  w.u8(87);
  w.u8(7);
  w.bytes("KITCHEN", 7);
  const size_t n = s->seal(f, w.size());
  const std::vector<uint8_t> want = unhex(
      "c7110300e13e368e443322110700000001000000496daff1a20747187f7b647e498edf8dee5b8ed3d6177c50cce"
      "f8ca429a1");
  c.check(n == want.size() && memcmp(f, want.data(), n) == 0,
          "a sealed presence matches the reference bytes");

  uint8_t g[kMaxFrame];
  size_t body = 0;
  memcpy(g, f, n);
  c.check(s->open(g, n, body) && body == 14 && memcmp(g + kHeader + 7, "KITCHEN", 7) == 0,
          "it opens again, body intact");
  memcpy(g, f, n);
  forward(g);
  c.check(s->open(g, n, body), "it still opens after a relay counts its hops");
  memcpy(g, f, n);
  g[kHeader + 3] ^= 1;
  c.check(!s->open(g, n, body), "a changed body doesn't open");
  memcpy(g, f, n);
  g[5] ^= 1;  // the group id: in the associated data, not the nonce
  c.check(!s->open(g, n, body), "a changed header doesn't open");
  memcpy(g, f, n);
  c.check(!other->open(g, n, body), "another group's key doesn't open it");
}

void testReplay(Checker &c) {
  std::unique_ptr<ReplayGuard> g(new ReplayGuard());
  using V = ReplayGuard::Verdict;
  bool ok = g->accept(7, 5, 1, false) == V::Stale && g->accept(7, 5, 1, true) == V::Ok &&
            g->accept(7, 5, 2, true) == V::Ok;
  c.check(ok, "the first packet from a board must be fresh");
  c.check(g->accept(7, 5, 2, true) == V::Duplicate, "a copy is dropped");
  ok = g->accept(7, 5, 70, true) == V::Ok && g->accept(7, 5, 5, true) == V::TooOld &&
       g->accept(7, 5, 69, true) == V::Ok && g->accept(7, 5, 69, true) == V::Duplicate &&
       g->accept(7, 5, 7, true) == V::Ok;
  c.check(ok, "the 64-packet window: late ones in it taken once, older ones dropped");
  ok = g->accept(7, 4, 100, true) == V::OldBoot && g->accept(7, 6, 1, false) == V::Stale &&
       g->accept(7, 6, 1, true) == V::Ok && g->accept(7, 5, 71, true) == V::OldBoot;
  c.check(ok, "an earlier boot is dropped; a later one starts afresh");
}

void testAdpcm(Checker &c) {
  // A second of speech-like sound: three tones under a 3 Hz envelope, 50 packets of 20 ms, each
  // decoded on its own from the state it carries.
  constexpr size_t kN = kVoiceRate;
  std::vector<int16_t> in(kN), out(kN);
  for (size_t i = 0; i < kN; ++i) {
    const float t = static_cast<float>(i) / kVoiceRate;
    const float env = 0.5f + 0.5f * sinf(2 * static_cast<float>(M_PI) * 3 * t);
    const float x = 0.6f * sinf(2 * static_cast<float>(M_PI) * 300 * t) +
                    0.3f * sinf(2 * static_cast<float>(M_PI) * 1100 * t) +
                    0.1f * sinf(2 * static_cast<float>(M_PI) * 3400 * t);
    in[i] = static_cast<int16_t>(10000 * env * x);
  }
  adpcm::State enc;
  std::vector<adpcm::State> starts;
  std::vector<uint8_t> packed(kN / 2);
  for (size_t f = 0; f < kN / kFrameSamples; ++f) {
    starts.push_back(enc);
    adpcm::encode(enc, &in[f * kFrameSamples], kFrameSamples, &packed[f * kFrameBytes]);
  }
  for (size_t f = 0; f < kN / kFrameSamples; f += 2) {  // every other packet first: out of order
    adpcm::State st = starts[f];
    adpcm::decode(st, &packed[f * kFrameBytes], kFrameBytes, &out[f * kFrameSamples]);
  }
  for (size_t f = 1; f < kN / kFrameSamples; f += 2) {
    adpcm::State st = starts[f];
    adpcm::decode(st, &packed[f * kFrameBytes], kFrameBytes, &out[f * kFrameSamples]);
  }
  double err = 0, sig = 0;
  for (size_t i = 0; i < kN; ++i) {
    const double e = static_cast<double>(in[i]) - out[i];
    err += e * e;
    sig += static_cast<double>(in[i]) * in[i];
  }
  const double snr = 10 * log10(sig / err);
  char what[80];
  snprintf(what, sizeof what, "ADPCM round trip, packets decoded on their own: SNR %.1f dB (>= 20)",
           snr);
  c.check(snr >= 20, what);
  c.check(kFrameBytes == 160 && kHeader + 4 + 4 + 2 + 2 + 2 + 1 + 1 + kFrameBytes + kTag == 212,
          "a voice packet is 212 bytes (ESP-NOW v1 takes 250)");
}

void testPacket(Checker &c) {
  uint8_t f[kMaxFrame] = {};
  Header h;
  h.type = kVoice;
  h.ttl = kTtl;
  h.group = 0x8e363ee1;
  h.sender = 0xDEADBEEF;
  h.boot = 9;
  h.seq = 77;
  putHeader(h, f);
  Header p;
  bool ok = parseHeader(f, 212, p) && p.type == kVoice && p.ttl == 3 && p.hops == 0 &&
            p.group == h.group && p.sender == h.sender && p.boot == 9 && p.seq == 77;
  c.check(ok, "header written and read back");
  ok = forward(f) && parseHeader(f, 212, p) && p.ttl == 2 && p.hops == 1;
  c.check(ok, "a relay takes one from ttl and adds one to hops");
  f[2] = 0x30;  // ttl 0, 3 hops
  c.check(!forward(f), "ttl 0 isn't relayed");
  putHeader(h, f);
  ok = !parseHeader(f, kHeader + kTag - 1, p) && !parseHeader(f, kMaxFrame + 1, p);
  f[0] = 0x00;
  ok = ok && !parseHeader(f, 212, p);
  f[0] = kMagic;
  f[1] = 2 << 4 | kVoice;  // version 2
  ok = ok && !parseHeader(f, 212, p);
  c.check(ok, "short, long, foreign and later-version frames are refused");
  uint8_t b[8];
  Writer w(b, sizeof b);
  w.u32(1);
  w.u32(2);
  w.u8(3);
  Reader r(b, 4);
  const uint32_t one = r.u32();
  r.u16();
  c.check(!w.ok() && one == 1 && !r.ok(), "a body can't be written or read past its end");
}

void testSeen(Checker &c) {
  std::unique_ptr<SeenCache> s(new SeenCache());
  for (uint32_t i = 0; i < 300; ++i) s->add(1, 2, 3, i);
  const bool ok = s->seen(1, 2, 3, 299) && s->seen(1, 2, 3, 300 - SeenCache::kSize) &&
                  !s->seen(1, 2, 3, 299 - SeenCache::kSize) && !s->seen(1, 2, 4, 299) &&
                  !s->seen(9, 2, 3, 299);
  c.check(ok, "seen-cache: the last 256 kept, by group, sender, boot and seq");
}

}  // namespace

bool selfTest(Print &out) {
  Checker c{out};
  const uint32_t t0 = millis();
  testHkdf(c);
  testGcm(c);
  testReplay(c);
  testAdpcm(c);
  testPacket(c);
  testSeen(c);
  out.printf("commstest: %s (%d failed) in %lu ms\n", c.failed ? "FAILED" : "all passed",
             c.failed, static_cast<unsigned long>(millis() - t0));
  return c.failed == 0;
}

}  // namespace comms

#endif  // INKDECK_COMMS
