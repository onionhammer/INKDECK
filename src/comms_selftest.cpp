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
#include "comms_calls.h"
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

void testCallPackets(Checker &c) {
  const crypto::GroupKeys k = crypto::derive(kSecret, false);
  std::unique_ptr<crypto::Sealer> s(new crypto::Sealer());
  s->setKey(k.key);
  // A ring from sender 11223344, boot 7, seq 2: to all, ring 9, conversation A1B2C3D4, rung
  // 4200 ms (docs/comms-protocol.md has these bytes).
  uint8_t f[kMaxFrame];
  Header h;
  h.type = kRing;
  h.ttl = 3;
  h.group = k.id;
  h.sender = 0x11223344;
  h.boot = 7;
  h.seq = 2;
  putHeader(h, f);
  Writer w(f + kHeader, kMaxBody);
  w.u32(0);
  w.u32(0);
  w.u16(9);
  w.u32(0xA1B2C3D4);
  w.u32(4200);
  const size_t n = s->seal(f, w.size());
  const std::vector<uint8_t> want = unhex(
      "c7180300e13e368e44332211070000000200000095184362b8f980d1bcabd37b688b9e0f2160c1a57c8f182d6ad"
      "d0b5f6239176f7ac4");
  c.check(n == want.size() && memcmp(f, want.data(), n) == 0,
          "a sealed ring matches the reference bytes");
  Header p;
  size_t body = 0;
  bool ok = parseHeader(f, n, p) && p.type == kRing && s->open(f, n, body);
  Reader r(f + kHeader, body);
  r.u32();
  ok = ok && r.u32() == 0 && r.u16() == 9 && r.u32() == 0xA1B2C3D4 && r.u32() == 4200 && r.ok();
  c.check(ok, "the ring opens and reads back field by field");

  ok = true;
  for (uint8_t t = kRing; t <= kMessageAsk; ++t) {
    h.type = t;
    putHeader(h, f);
    ok = ok && parseHeader(f, kHeader + kTag + 8, p) && p.type == t && knownType(t);
  }
  h.type = 15;
  putHeader(h, f);
  ok = ok && parseHeader(f, kHeader + kTag + 8, p) && !knownType(15);
  f[1] = static_cast<uint8_t>(kVersion << 4);  // type 0
  ok = ok && !parseHeader(f, kHeader + kTag + 8, p);
  c.check(ok, "the new types parse; a later one parses (to be relayed), type 0 doesn't");

  // A presence with the conversation after the name, and one from before conversations.
  uint8_t b[96];
  Writer pw(b, sizeof b);
  pw.u32(0);
  pw.u8(3);
  pw.u8(87);
  pw.u8(7);
  pw.bytes("KITCHEN", 7);
  const size_t old = pw.size();
  pw.u32(0xA1B2C3D4);
  pw.u16(75);
  pw.u16(4);
  pw.u8(2);
  pw.u32(0x55667788);
  pw.u32(0x99AABBCC);
  Reader pr(b, pw.size());
  pr.u32();
  pr.u8();
  pr.u8();
  const uint8_t len = pr.u8();
  pr.bytes(len);
  ok = pr.left() >= 4 && pr.u32() == 0xA1B2C3D4 && pr.u16() == 75 && pr.u16() == 4 &&
       pr.u8() == 2 && pr.u32() == 0x55667788 && pr.u32() == 0x99AABBCC && pr.ok() &&
       pr.left() == 0;
  Reader po(b, old);
  po.u32();
  po.u8();
  po.u8();
  po.bytes(po.u8());
  c.check(ok && po.ok() && po.left() == 0,
          "presence: the conversation follows the name; an older board's ends there");
  c.check(kHeader + 4 + 4 + 4 + 2 + 2 + 1 + 1 + kFrameBytes + kTag == 214 &&
              kHeader + 4 + 4 + 4 + 1 + 2 + 2 + 2 + 195 + kTag == 250 &&
              kHeader + 4 + 4 + 2 + 2 + 2 + 1 + 1 + kFrameBytes + 5 + kTag == 217,
          "a message frame is 214 bytes, an ask mapping 1560 frames 250, voice 217");
}

void testMessages(Checker &c) {
  // A message of 100 frames, the last one short; every 7th lost on the air, some twice.
  constexpr uint16_t kN = 100;
  std::unique_ptr<MessageAssembly> a(new MessageAssembly());
  std::vector<uint8_t> sent(kN * kStoredFrame, 0);
  uint8_t data[kFrameBytes];
  auto frameOf = [&](uint16_t f, uint8_t &bytes) {
    bytes = f == kN - 1 ? 37 : kFrameBytes;
    for (size_t i = 0; i < kFrameBytes; ++i) data[i] = static_cast<uint8_t>(f * 31 + i * 7);
  };
  for (uint16_t f = 0; f < kN; ++f) {
    uint8_t bytes;
    frameOf(f, bytes);
    uint8_t *st = &sent[f * kStoredFrame];
    st[0] = static_cast<uint8_t>(f);
    st[1] = static_cast<uint8_t>(f >> 8);
    st[2] = static_cast<uint8_t>(f % 89);
    st[3] = bytes;
    memcpy(st + 4, data, bytes);
  }
  const uint32_t sum = messageSum(sent.data(), kN);
  auto add = [&](uint16_t f) {
    uint8_t bytes;
    frameOf(f, bytes);
    return a->add(f, static_cast<int16_t>(f), static_cast<uint8_t>(f % 89), data, bytes);
  };
  bool dupes = true;
  for (int f = kN - 1; f >= 0; --f) {  // backwards: out of order
    if (f % 7 == 3) continue;
    add(static_cast<uint16_t>(f));
    if (f % 10 == 0) dupes = dupes && !add(static_cast<uint16_t>(f));
  }
  c.check(dupes && a->have() == kN - 14, "message: copies of a frame are taken once");
  uint16_t starts[48], counts[48];
  size_t n = a->missing(starts, counts, 48);
  bool ok = n == 14 && !a->allIn();
  for (size_t i = 0; i < n; ++i) ok = ok && starts[i] % 7 == 3 && counts[i] == 1;
  c.check(ok, "message: the 14 frames missing are listed before the end comes");
  a->end(kN, sum);
  ok = a->ended() && a->total() == kN && !a->allIn() && a->missing(starts, counts, 48) == 14 &&
       a->missing(starts, counts, 5) == 5 && starts[0] == 3 && starts[4] == 31;
  c.check(ok, "message: after the end, the same 14, and an ask holds what fits");
  uint8_t map[195];
  uint16_t from = 0;
  size_t lost = 0;
  const size_t span = a->missingMap(from, map, 1560, lost);
  // Frames 3 (bit 0) and 10 (bit 7) missing, 4 to 9 and 11 (bit 8) in.
  ok = from == 3 && span == kN - 3 && lost == 14 && map[0] == 0x81 && (map[1] & 0x01) == 0;
  const size_t part = a->missingMap(from, map, 16, lost);
  c.check(ok && part == 16 && lost == 3, "message: the map of what's missing, from the first gap");
  ok = !add(0);  // a copy of a frame that's in, sent again for another board
  for (uint16_t f = 3; f < kN; f += 7) add(f);
  ok = ok && a->allIn() && a->sumOk() && a->audio().size() == sent.size() &&
       memcmp(a->audio().data(), sent.data(), sent.size()) == 0;
  c.check(ok, "message: asked for again, it's whole, byte for byte, checksum and all");

  std::unique_ptr<MessageAssembly> b(new MessageAssembly());
  b->end(kN, sum ^ 1);  // the end first, every frame missed
  n = b->missing(starts, counts, 48);
  ok = n == 1 && starts[0] == 0 && counts[0] == kN;
  for (uint16_t f = 0; f < kN; ++f) {
    uint8_t bytes;
    frameOf(f, bytes);
    b->add(f, static_cast<int16_t>(f), static_cast<uint8_t>(f % 89), data, bytes);
  }
  ok = ok && b->allIn() && !b->sumOk() && !b->add(kN, 0, 0, data, 1);
  c.check(ok, "message: all of it asked for from the end alone; a wrong checksum shows");
}

void testConversations(Checker &c) {
  using C = ConvState;
  constexpr uint32_t kSelf = 1, kA = 2, kB = 3;
  std::unique_ptr<C> v(new C());
  // A talks (its first call opens 500); this board played it, so it's in.
  bool ok = v->spoke(0, 500, kA, kSelf) && v->on() && !v->mine() && v->count() == 1;
  ok = ok && v->join(100, 0) && v->mine() && v->id() == 500;
  v->spoke(10000, 500, kA, kSelf);  // the last word at 10 s
  v->report(19000, kA, 500, 19, 9, nullptr, 0, kSelf);
  v->expire(69999);
  ok = ok && v->on();
  v->expire(70000);
  c.check(ok && !v->on() && !v->mine(), "conversation: over 60 s after the last word");
  v->report(70500, kA, 500, 70, 59, nullptr, 0, kSelf);  // A's clock a little behind: old news
  ok = !v->on();
  v->spoke(71000, 500, kA, kSelf);  // but A spoke in it before it ended there
  c.check(ok && v->on() && v->mine() && v->id() == 500,
          "conversation: a late report doesn't bring it back; a late word does");

  v.reset(new C());
  v->join(0, 600);
  const uint32_t listed[] = {kB, kSelf};
  v->report(1000, kA, 600, 1, 1, listed, 2, kSelf);
  ok = v->count() == 2 && v->isMember(kB) && v->member(0).id == kA && !v->member(0).listed;
  v->report(5000, kA, 600, 5, 1, nullptr, 0, kSelf);
  v->expire(13999);
  ok = ok && v->isMember(kB);
  v->expire(14000);  // B never said so itself: 13 s after A listed it, it's out
  ok = ok && !v->isMember(kB) && v->isMember(kA);
  v->expire(18000);
  c.check(ok && v->on() && v->count() == 0 && v->mine(),
          "conversation: members that stop saying so go after 13 s; this board alone keeps it");

  v.reset(new C());
  v->report(0, kA, 700, 0, 0, nullptr, 0, kSelf);  // seen, not in it
  ok = v->on() && !v->mine() && !v->left() && v->join(10, 0) && v->mine();
  ok = ok && v->leave(20) && !v->mine() && v->left() && v->on();
  v->report(4000, kA, 700, 4, 0, nullptr, 0, kSelf);
  ok = ok && v->left() && !v->mine();  // still out: its voice doesn't play here
  ok = ok && v->join(5000, 0) && v->mine() && !v->left();
  c.check(ok, "conversation: left stays out (still seen on) until joined again");

  v.reset(new C());
  v->report(0, kA, 800, 0, 0, nullptr, 0, kSelf);
  ok = v->reportNone(100, kA) && v->count() == 0;
  v->report(200, kB, 800, 0, 0, listed + 0, 0, kSelf);
  const uint32_t backA[] = {kA};
  v->report(300, kB, 800, 0, 0, backA, 1, kSelf);  // B still lists A: A said it's out
  ok = ok && !v->isMember(kA) && v->isMember(kB);
  v->reportNone(400, kB);
  v->expire(500);
  c.check(ok && !v->on(), "conversation: a board that says it's out isn't listed back in; "
                          "nobody in it: over");

  v.reset(new C());
  v->join(0, 900);
  v->report(1000, kA, 800, 30, 2, nullptr, 0, kSelf);  // another one, begun earlier, lower id
  ok = v->id() == 800 && v->mine() && v->isMember(kA) && 1000 - v->startMs() == 30000;
  v->report(2000, kB, 950, 1, 0, nullptr, 0, kSelf);  // a higher id: it merges into 800
  c.check(ok && v->id() == 800 && v->isMember(kB),
          "conversation: two that meet become the one with the lower id");
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
  testCallPackets(c);
  testMessages(c);
  testConversations(c);
  out.printf("commstest: %s (%d failed) in %lu ms\n", c.failed ? "FAILED" : "all passed",
             c.failed, static_cast<unsigned long>(millis() - t0));
  return c.failed == 0;
}

}  // namespace comms

#endif  // INKDECK_COMMS
