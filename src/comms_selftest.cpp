// comms::selfTest() (!commstest): the parts of comms that need no radio, each check printed PASS
// or FAIL. The expected bytes come from a separate implementation (Python's hmac, hashlib and
// cryptography), so they also pin docs/comms-protocol.md: another implementation that makes
// the same frame reads this board's packets.
#include "comms.h"

#if INKDECK_COMMS  // the whole file (features.h)

#include <Arduino.h>
#include <math.h>
#include <string.h>

#include <initializer_list>
#include <memory>
#include <utility>
#include <vector>

#include "adpcm.h"
#include "comms_calls.h"
#include "comms_crypto.h"
#include "comms_mesh.h"
#include "comms_mic.h"
#include "comms_reach.h"
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
  c.check(kFrameBytes == 160 &&
              kHeader + 4 + 4 + 4 + 2 + 2 + 2 + 1 + 1 + 2 + 1 + 1 + kFrameBytes + kTag == 220,
          "a voice packet is 220 bytes (ESP-NOW v1 takes 250)");
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
  for (uint8_t t = kRing; t <= kExt; ++t) {
    h.type = t;
    putHeader(h, f);
    ok = ok && parseHeader(f, kHeader + kTag + 8, p) && p.type == t && knownType(t);
  }
  f[1] = static_cast<uint8_t>(kVersion << 4);  // type 0
  ok = ok && !parseHeader(f, kHeader + kTag + 8, p) && kExt == 15 && kLastType == 15;
  ok = ok && knownType(kMic) && knownType(kVoice) && knownType(kMicOff) && !knownType(7);
  c.check(ok, "the types parse, 15 (ext) the last there can be, 7 unused; type 0 doesn't");

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
              kHeader + 4 + 4 + 4 + 1 + 2 + 2 + 2 + 195 + kTag == 250,
          "a message frame is 214 bytes, an ask mapping 1560 frames 250");
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


// The mic's checks run in their own frames (noinline): the dev tools' task has 4 KB of stack,
// and LTO would add them all to selfTest()'s.
__attribute__((noinline)) void testMicPackets(Checker &c) {
  const crypto::GroupKeys k = crypto::derive(kSecret, false);
  std::unique_ptr<crypto::Sealer> s(new crypto::Sealer());
  s->setKey(k.key);
  // On the heap: the dev tools' task has a small stack.
  struct Bufs {
    uint8_t f[kMaxFrame], b[kMaxBody];
    MicStill st, back;
    Voice v, vb, full;
    MicOff off, offb;
    History hist;
  };
  std::unique_ptr<Bufs> u(new Bufs());
  uint8_t *f = u->f;
  uint8_t *b = u->b;
  // A still-here (type 2) from sender 11223344, boot 7, seq 3: sending, on USB, no battery,
  // threshold -30, peak -24, session 5, on 600 s, bar 5 at -18, pre-roll 8 frames, to all, in
  // conversation A1B2C3D4 (docs/comms-protocol.md has these bytes).
  Header h;
  h.type = kMic;
  h.ttl = 3;
  h.group = k.id;
  h.sender = 0x11223344;
  h.boot = 7;
  h.seq = 3;
  putHeader(h, f);
  MicStill &st = u->st;
  st.flags = kMicSending | kMicUsb;
  st.battery = 0xFF;
  st.threshold = 30;
  st.peak = 24;
  st.session = 5;
  st.onS = 600;
  st.bar = 5;
  st.barPeak = 18;
  st.preFrames = 8;
  st.target = 0;
  st.conv = 0xA1B2C3D4;
  size_t n = s->seal(f, putMicStill(f + kHeader, kMaxBody, 0, st));
  std::vector<uint8_t> want = unhex(
      "c7120300e13e368e443322110700000003000000e74688b99260814b63cfeaa8142b8f33b65b204e035360b158"
      "2729fcbda72e8993accf2e6124db819103");
  c.check(n == want.size() && memcmp(f, want.data(), n) == 0,
          "a sealed still-here (type 2) matches the reference bytes");
  size_t body = 0;
  bool ok = s->open(f, n, body);
  Reader r(f + kHeader, body);
  r.u32();
  MicStill &back = u->back;
  ok = ok && readMicStill(r, back) && back.flags == st.flags && back.threshold == 30 &&
       back.peak == 24 && back.session == 5 && back.onS == 600 && back.bar == 5 &&
       back.barPeak == 18 && back.preFrames == 8 && back.target == 0 &&
       back.conv == 0xA1B2C3D4 && r.left() == 0;
  c.check(ok, "the still-here reads back field by field");

  // Voice (type 3), seq 4: to all, conversation A1B2C3D4, session 5, sound 2, frame 9, level -21,
  // opening, predictor -1234, index 40, 8 bytes.
  h.type = kVoice;
  h.seq = 4;
  putHeader(h, f);
  Voice &v = u->v;
  v.conv = 0xA1B2C3D4;
  v.session = 5;
  v.sound = 2;
  v.frame = 9;
  v.level = 21;
  v.flags = kVoiceOpening;
  v.predictor = -1234;
  v.index = 40;
  v.bytes = 8;
  for (uint8_t i = 0; i < 8; ++i) v.data[i] = static_cast<uint8_t>(i + 1);
  n = s->seal(f, putVoice(f + kHeader, kMaxBody, 0, v));
  want = unhex(
      "c7130300e13e368e443322110700000004000000a8c23f21818170d60048ac825824a4e2a8b2f438ffdc362dda"
      "38f9995ef899d129751713f97265f70f5fe1782ab0339a");
  ok = n == want.size() && memcmp(f, want.data(), n) == 0;
  ok = ok && s->open(f, n, body);
  Reader rv(f + kHeader, body);
  rv.u32();
  Voice &vb = u->vb;
  ok = ok && readVoice(rv, vb) && vb.target == 0 && vb.conv == 0xA1B2C3D4 && vb.session == 5 &&
       vb.sound == 2 && vb.frame == 9 && vb.level == 21 && vb.flags == kVoiceOpening &&
       vb.predictor == -1234 && vb.index == 40 && vb.bytes == 8 && vb.data[7] == 8;
  c.check(ok, "a sealed voice frame (type 3) matches the reference bytes and reads back");

  // Mic off (type 4), seq 5: session 5, switched off, on 600 s.
  h.type = kMicOff;
  h.seq = 5;
  putHeader(h, f);
  u->off.session = 5;
  u->off.reason = kOffSwitched;
  u->off.onS = 600;
  n = s->seal(f, putMicOff(f + kHeader, kMaxBody, 0, u->off));
  want = unhex(
      "c7140300e13e368e4433221107000000050000003ddb4c9843cacf766c0a6f4f4f5c99a236200cdcbdbf1bb3fd"
      "7296");
  ok = n == want.size() && memcmp(f, want.data(), n) == 0 && s->open(f, n, body);
  Reader ro(f + kHeader, body);
  ro.u32();
  ok = ok && readMicOff(ro, u->offb) && u->offb.session == 5 && u->offb.reason == kOffSwitched &&
       u->offb.onS == 600;
  c.check(ok, "a sealed mic off (type 4) matches the reference bytes and reads back");

  // Sizes: a full voice frame, a still-here, a history of 60 bars, under 250; who each is for.
  Voice &full = u->full;
  full.bytes = kFrameBytes;
  full.target = 0x55667788;
  const size_t fullLen = putVoice(b, kMaxBody, 0, full);
  ok = fullLen && kHeader + fullLen + kTag == 220 && get32(b + 4) == 0x55667788;
  History &hist = u->hist;
  hist.to = 0x55667788;
  hist.count = MicHistory::kBars;
  const size_t histLen = putHistory(b, kMaxBody, 0, hist);
  ok = ok && kHeader + histLen + kTag == 110 &&
       kHeader + putMicStill(b, kMaxBody, 0, st) + kTag == 62;
  ok = ok && extFor(b, putHistory(b, kMaxBody, 0, hist)) == 0x55667788 &&
       extFor(b, putHistoryAsk(b, kMaxBody, 0, 0x11223344)) == 0x11223344;
  // A frame of more than 160 bytes, or a step index past 88, is refused.
  b[0] = b[1] = b[2] = b[3] = 0;
  const size_t len = putVoice(b, kMaxBody, 0, full);
  b[4 + 4 + 4 + 2 + 2 + 2 + 1 + 1 + 2] = 89;  // the step index
  Reader bad(b + 4, len - 4);
  ok = ok && !readVoice(bad, u->vb);
  c.check(ok, "voice is 220 bytes full, a still-here 62, a 2 h history 110; each for whom it says; "
              "a bad step index is refused");
}

__attribute__((noinline)) void testGate(Checker &c) {
  // -30 dBFS threshold; 100 quiet frames, 10 over it, then quiet.
  std::unique_ptr<MicGate> g(new MicGate());
  g->setThreshold(-30);
  int sent = 0, firsts = 0;
  uint32_t firstAt = 0, lastAt = 0;
  for (uint32_t i = 0; i < 400; ++i) {
    const float db = i >= 100 && i < 110 ? -20.0f : -45.0f;
    const MicGate::Step s = g->frame(db);
    if (s.send) {
      if (!sent) firstAt = i;
      lastAt = i;
      ++sent;
    }
    if (s.first) ++firsts;
  }
  const uint32_t P = MicGate::kPreFrames, H = MicGate::kHangFrames;
  // The first frame sent is frame 100 - P (the pre-roll), sent at frame 100; the last, frame
  // 109 + H, sent P later.
  bool ok = sent == static_cast<int>(P + 10 + H) && firsts == 1 && firstAt == 100 &&
            lastAt - P == 109 + H && !g->sending() && P * kFrameMs == 160;
  c.check(ok, "gate: a sound goes with 160 ms before it and 2 s after, 160 ms late");

  g.reset(new MicGate());
  g->setThreshold(-30);
  sent = 0;
  for (uint32_t i = 0; i < 300; ++i) sent += g->frame(i % 50 == 0 ? -30.5f : -60.0f).send;
  ok = sent == 0;
  g->reset();
  g->setThreshold(-60);
  sent = 0;
  for (uint32_t i = 0; i < 300; ++i) sent += g->frame(-59.0f).send;
  ok = ok && sent == static_cast<int>(300 - P);
  c.check(ok, "gate: below the threshold nothing goes; at -60 a -59 dBFS room goes all the time");

  // Two loud frames 2 s apart are one sound; 5 s apart, two. A loud frame right after a reset
  // has only the pre-roll there is.
  g.reset(new MicGate());
  g->setThreshold(-30);
  firsts = 0;
  for (uint32_t i = 0; i < 600; ++i) {
    const bool loud = i == 100 || i == 200 || i == 450;
    firsts += g->frame(loud ? -10.0f : -50.0f).first;
  }
  ok = firsts == 2;
  g->reset();
  sent = 0;
  for (uint32_t i = 0; i < 30; ++i) {
    const MicGate::Step s = g->frame(i == 3 ? -10.0f : -50.0f);
    if (s.send && !sent) firstAt = i;
    sent += s.send;
  }
  c.check(ok && firstAt == P && sent == static_cast<int>(30 - P),
          "gate: sounds 2 s apart are one, 5 s apart two; after a pause the pre-roll starts there");
}

__attribute__((noinline)) void testEchoGate(Checker &c) {
  // The speaker plays from frame 50 to 150, putting up to -20 dBFS on the mic (with the
  // margin): the echo alone (-24 at the mic) opens nothing.
  std::unique_ptr<MicGate> g(new MicGate());
  g->setThreshold(-30);
  int sent = 0;
  for (uint32_t i = 0; i < 300; ++i) {
    const bool playing = i >= 50 && i < 150;
    sent += g->frame(playing ? -24.0f : -50.0f, playing ? -20.0f : -96.0f).send;
  }
  bool ok = sent == 0;
  // Someone speaks over it at -10 (frames 100 to 109): it goes, and the frames around it that
  // the echo could explain go as silence; after the speaker stops, the hang's frames go as they
  // are.
  g.reset(new MicGate());
  g->setThreshold(-30);
  int loud = 0, muted = 0, plain = 0, firsts = 0;
  for (uint32_t i = 0; i < 400; ++i) {
    const bool playing = i >= 50 && i < 150;
    const bool talk = i >= 100 && i < 110;
    const float db = talk ? -10.0f : playing ? -24.0f : -50.0f;
    const MicGate::Step s = g->frame(db, playing ? -20.0f : -96.0f);
    if (!s.send) continue;
    firsts += s.first;
    const uint32_t f = i - MicGate::kPreFrames;  // the frame that goes
    if (f >= 100 && f < 110) {
      loud += !s.muted;
    } else if (f < 150) {
      muted += s.muted;
    } else {
      plain += !s.muted;
    }
  }
  const uint32_t P = MicGate::kPreFrames, H = MicGate::kHangFrames;
  // The pre-roll (P before 100) and the hang's part while the speaker played (110 to 149) muted;
  // the hang after it (150 to 109 + H) as it is.
  ok = ok && firsts == 1 && loud == 10 && muted == static_cast<int>(P + 40) &&
       plain == static_cast<int>(109 + H - 150 + 1);
  c.check(ok, "echo gate: the speaker's echo alone opens nothing; a voice over it goes, the "
              "echo around it as silence");
}

__attribute__((noinline)) void testGateDrain(Checker &c) {
  // Loud frames 100 to 109, the speaker's echo on 110 and 111, the mic off after frame 111: the
  // 8 frames held back (104 to 111) go, the two the echo explains as silence, none a new sound.
  const uint32_t P = MicGate::kPreFrames, H = MicGate::kHangFrames;
  std::unique_ptr<MicGate> g(new MicGate());
  g->setThreshold(-30);
  int sent = 0;
  for (uint32_t i = 0; i < 112; ++i) {
    const bool echo = i >= 110;
    sent += g->frame(i >= 100 && i < 110 ? -20.0f : echo ? -40.0f : -50.0f,
                     echo ? -35.0f : -96.0f).send;
  }
  int drained = 0, muted = 0, firsts = 0;
  for (uint32_t k = 0; k < P; ++k) {
    const MicGate::Step s = g->drain();
    if (!s.send) break;
    ++drained;
    muted += s.muted;
    firsts += s.first;
  }
  // Sent before the off: frames 100 - P to 103, at frames 100 to 111.
  bool ok = sent == 12 && drained == static_cast<int>(P) && muted == 2 && firsts == 0;
  // Nothing going out: nothing to drain.
  g.reset(new MicGate());
  g->setThreshold(-30);
  for (uint32_t i = 0; i < 50; ++i) g->frame(-50.0f);
  ok = ok && !g->drain().send;
  // Off near the hang's end (a loud frame at 10, the mic off after frame 12 + H): only the frames
  // the hang still covers, up to 10 + H.
  g.reset(new MicGate());
  g->setThreshold(-30);
  for (uint32_t i = 0; i < 13 + H; ++i) g->frame(i == 10 ? -20.0f : -50.0f);
  drained = 0;
  for (uint32_t k = 0; k < P && g->drain().send; ++k) ++drained;
  c.check(ok && drained == static_cast<int>(P - 2),
          "gate drain: a mic off mid-sound sends the 160 ms held back, echo as silence; nothing "
          "when quiet, only what the hang covers at its end");
}

__attribute__((noinline)) void testHistory(Checker &c) {
  // A mic on 130 minutes, louder each 2 minutes, paused for bars 20 and 21.
  std::unique_ptr<MicHistory> h(new MicHistory());
  for (uint32_t ms = 0; ms < 130 * 60000u; ms += 1000) {
    const uint32_t bar = ms / MicHistory::kBarMs;
    if (bar == 20 || bar == 21) continue;
    h->note(ms, -90.0f + bar + (ms % 7000 == 0 ? 5 : 0));
  }
  bool ok = h->top() == 64 && h->first() == 5 && h->at(4) == kNoLevel && h->at(5) == 80 &&
            h->at(64) == 21 && h->at(20) == kNoLevel && h->at(22) == 63 && h->at(65) == kNoLevel;
  c.check(ok, "history: 60 bars of 2 minutes, the loudest frame each, the oldest forgotten");

  // A board that hears it: the history it asked for, then bars from still-heres, one skipped.
  std::unique_ptr<MicHistory> l(new MicHistory());
  for (uint16_t b = h->first(); b <= h->top(); ++b) l->set(b, h->at(b));
  ok = l->top() == 64 && l->at(5) == 80 && l->at(20) == kNoLevel && l->at(64) == 21;
  l->set(66, 30);
  ok = ok && l->top() == 66 && l->at(65) == kNoLevel && l->at(7) == 78 && l->at(6) == kNoLevel;
  l->set(200, 10);
  c.check(ok && l->at(66) == kNoLevel && l->at(200) == 10 && levelByte(-96.5f) == 96 &&
              levelByte(3) == 0 && levelDb(kNoLevel) == -96,
          "history: a board that hears the mic mirrors it; a gap clears the bars skipped");
}

__attribute__((noinline)) void testHeard(Checker &c) {
  using L = HeardMic;
  std::unique_ptr<L> s(new L());
  s->setThreshold(-30);
  // Session 5, on 4 s when first heard.
  L::Change ch = s->on(1000, 5, 4000);
  bool ok = ch.began && !ch.ended && s->link() == L::Link::Quiet && s->startMs() == 0 - 3000u;
  bool loud = false;
  // A soft sound (-25: over the threshold, not 12 dB over), then a loud one.
  for (uint32_t t = 1000; t < 3000; t += 20) s->audio(t, 1, -25, loud);
  ok = ok && s->link() == L::Link::Sound && !loud;
  s->tick(3000 + L::kSoundEndMs, true);
  ok = ok && s->link() == L::Link::Quiet && s->events().size() == 1 && !s->events()[0].loud &&
       s->events()[0].endMs - s->events()[0].startMs == 1980;
  int becameLoud = 0;
  for (uint32_t t = 5000; t < 6000; t += 20) {
    s->audio(t, 2, t < 5500 ? -30.0f : -18.0f, loud);
    becameLoud += loud;
  }
  ok = ok && becameLoud == 1 && s->events().back().loud && s->events().back().peakDb == -18;
  c.check(ok, "heard mic: SOUND, then QUIET 0.6 s after; LOUD 12 dB over its threshold, once");

  // Still-heres every 2 s until 10 s; then nothing: LOST at 30 s, the alarm, ack, the link back.
  for (uint32_t t = 6000; t <= 10000; t += 2000) ok = ok && !s->on(t, 5, t + 3000).began;
  s->tick(29999, true);
  ok = ok && s->link() == L::Link::Quiet && !s->alarm();
  s->tick(30000, true);
  ok = ok && s->link() == L::Link::Lost && s->alarm() && s->sinceMs() == 10000;
  ok = ok && s->ack() && !s->alarm() && s->link() == L::Link::Lost && !s->ack();
  ch = s->on(45000, 5, 48000);
  const L::Event &e = s->events().back();
  ok = ok && ch.back && !ch.began && s->link() == L::Link::Quiet && e.lost && !e.open &&
       e.endMs - e.startMs == 35000;
  s->tick(66000, false);  // lost again, this board not hearing it now: no alarm
  ok = ok && s->link() == L::Link::Lost && !s->alarm();
  c.check(ok, "heard mic: LOST 20 s after the last packet, its alarm until ack, the outage 35 s; "
              "no alarm where it isn't heard");

  // Sessions: back, then its off: session 5 ended, 70 s long. Session 6 begins; 7 comes without
  // 6's off: 6 ends, 7 begins. An off for another session changes nothing.
  s->on(67000, 5, 70000);
  ch = s->off(68000, 5);
  ok = ch.ended && ch.endedSession == 5 && ch.endedMs == 71000 && s->link() == L::Link::Off &&
       !s->alarm();
  ok = ok && !s->audio(69000, 3, -10, loud) && !s->tick(200000, true) &&
       s->link() == L::Link::Off;
  ch = s->on(300000, 6, 0);
  ok = ok && ch.began && !ch.ended && s->session() == 6;
  ch = s->on(310000, 7, 1000);
  ok = ok && ch.ended && ch.endedSession == 6 && ch.endedMs == 10000 && ch.began &&
       s->session() == 7 && !s->off(320000, 6).ended && s->on();
  c.check(ok, "heard mic: a session a log entry, on to off; a new one without the off ends the "
              "one before; a clean off is no alarm");
}

__attribute__((noinline)) void testMixer(Checker &c) {
  std::unique_ptr<Mixer> m(new Mixer());
  struct Bufs {
    int16_t tone[3][kFrameSamples];
    Mixer::Frame f[3];
    int16_t out[kFrameSamples];
    int16_t dec[kFrameSamples];
  };
  std::unique_ptr<Bufs> u(new Bufs());
  // Three boards, each a tone at -20 dBFS (3277 peak), 20 frames each, frames arriving in
  // turn every 20 ms, one of B's lost on the air.
  for (int s = 0; s < 3; ++s) {
    for (size_t i = 0; i < kFrameSamples; ++i) {
      u->tone[s][i] = static_cast<int16_t>(
          3277 * sinf(2 * static_cast<float>(M_PI) * (300 + 400 * s) * i / kVoiceRate));
    }
  }
  int most = 0, rounds = 0, lostSeen = 0;
  adpcm::State enc[3];
  uint32_t now = 1000;
  for (uint16_t fr = 0; fr < 20; ++fr, now += 20) {
    for (int s = 0; s < 3; ++s) {
      Mixer::Frame &f = u->f[s];
      f.frame = fr;
      f.predictor = enc[s].predictor;
      f.index = enc[s].index;
      f.bytes = kFrameBytes;
      adpcm::encode(enc[s], u->tone[s], kFrameSamples, f.data);
      if (s == 1 && fr == 10) continue;  // lost on the air
      m->add(0xA0 + s, 7, f, now);
    }
    const int n = m->pull(u->out, now);
    most = std::max(most, n);
    if (n) ++rounds;
    size_t k = 0;
    const Mixer::Played *p = m->played(k);
    for (size_t i = 0; i < k; ++i) lostSeen += p[i].lost;
  }
  bool ok = most == 3 && m->sources() == 3 && lostSeen == 1;
  // Late: a frame already played is dropped; one still waiting, a copy.
  ok = ok && m->add(0xA1, 7, u->f[1], now) == Mixer::Took::Copy;
  u->f[1].frame = 5;
  ok = ok && m->add(0xA1, 7, u->f[1], now) == Mixer::Took::Late;
  // A fourth board fits, a fifth doesn't.
  u->f[0].frame = 0;
  ok = ok && m->add(0xA3, 9, u->f[0], now) == Mixer::Took::Ok &&
       m->add(0xA4, 9, u->f[0], now) == Mixer::Took::NoRoom;
  c.check(ok, "mixer: three boards at once, a frame lost on the air covered, a late one "
              "dropped; four sources, the fifth waits");

  // The sum: two boards in one pull are their frames decoded and added, sample for sample.
  m.reset(new Mixer());
  adpcm::State e1, e2;
  Mixer::Frame &a = u->f[0];
  Mixer::Frame &b = u->f[1];
  int err = 0, pulls = 0;
  for (uint16_t fr = 0; fr < 4; ++fr) {
    a.frame = b.frame = fr;
    a.bytes = b.bytes = kFrameBytes;
    a.predictor = e1.predictor;
    a.index = e1.index;
    adpcm::encode(e1, u->tone[0], kFrameSamples, a.data);
    b.predictor = e2.predictor;
    b.index = e2.index;
    adpcm::encode(e2, u->tone[2], kFrameSamples, b.data);
    m->add(1, 1, a, 5000 + fr * 20);
    m->add(2, 1, b, 5000 + fr * 20);
    if (fr == 0) continue;  // two frames in before it plays: frame 0 plays at fr 1
  }
  // Frames 0..3 of each are in; the pulls play them in order. Frame 0 again, to compare.
  adpcm::State d1, d2;
  e1 = adpcm::State();
  e2 = adpcm::State();
  for (uint16_t fr = 0; fr < 4; ++fr) {
    adpcm::State s1 = e1, s2 = e2;
    uint8_t p1[kFrameBytes], p2[kFrameBytes];
    adpcm::encode(e1, u->tone[0], kFrameSamples, p1);
    adpcm::encode(e2, u->tone[2], kFrameSamples, p2);
    adpcm::decode(s1, p1, kFrameBytes, u->dec);
    int16_t other[kFrameSamples];
    adpcm::decode(s2, p2, kFrameBytes, other);
    pulls += m->pull(u->out, 5080 + fr * 20) == 2;
    for (size_t i = 0; i < kFrameSamples; ++i) {
      err = std::max(err, abs(u->out[i] - (u->dec[i] + other[i])));
    }
  }
  (void)d1;
  (void)d2;
  err = pulls == 4 ? err : 99999;
  // Loud: three boards near full scale don't wrap round.
  m.reset(new Mixer());
  for (int s = 0; s < 3; ++s) {
    for (size_t i = 0; i < kFrameSamples; ++i) u->tone[s][i] = (i / 8) % 2 ? 30000 : -30000;
    adpcm::State e;
    for (uint16_t fr = 0; fr < 3; ++fr) {
      Mixer::Frame &f = u->f[s];
      f.frame = fr;
      f.predictor = e.predictor;
      f.index = e.index;
      f.bytes = kFrameBytes;
      adpcm::encode(e, u->tone[s], kFrameSamples, f.data);
      m->add(0x10 + s, 1, f, 7000);
    }
  }
  int peak = 0;
  bool wrapped = false;
  for (int k = 0; k < 3; ++k) {
    m->pull(u->out, 7000 + k * 20);
    for (size_t i = 8; i < kFrameSamples; ++i) {
      peak = std::max(peak, abs(static_cast<int>(u->out[i])));
      // Every board's square wave has the same sign here: the sum mustn't flip it.
      if ((i / 8) % 2 && u->out[i] < 0) wrapped = true;
    }
  }
  // Idle: a board with nothing for 2 s is gone.
  m->pull(u->out, 7000 + 3 * 20 + Mixer::kIdleMs);
  m->pull(u->out, 7000 + 4 * 20 + Mixer::kIdleMs);
  c.check(err == 0 && peak > 30000 && peak <= 32767 && !wrapped && m->sources() == 0,
          "mixer: two at once in one pull; three near full scale held under it; gone after 2 s");
}

void testLinks(Checker &c) {
  // The link group: its key and id, and a links packet, as Python's cryptography makes them.
  const crypto::GroupKeys k = crypto::linkKeys();
  std::unique_ptr<crypto::Sealer> s(new crypto::Sealer());
  s->setKey(k.key);
  bool ok = memcmp(k.key, unhex("25f4f013d525701f32742171b454d3ecbd8786bb6da2a59ebc0b37be22b1576b")
                              .data(),
                   32) == 0 &&
            k.id == 0x57c9ed26u;
  uint8_t f[kMaxFrame];
  Header h;
  h.type = kExt;
  h.ttl = 0;
  h.group = k.id;
  h.sender = 0x11223344;
  h.boot = 7;
  h.seq = 6;
  putHeader(h, f);
  std::unique_ptr<Links> l(new Links());
  l->flags = kLinksRelays;
  l->count = 2;
  l->heard[0].id = 0x55667788;
  l->heard[0].pct = 97;
  l->heard[1].id = 0x99aabbcc;
  l->heard[1].pct = 64;
  const size_t body = putLinks(f + kHeader, kMaxBody, 0, *l);
  const size_t n = s->seal(f, body);
  const std::vector<uint8_t> want = unhex(
      "c71f000026edc957443322110700000006000000bf4af235269e1c3c6f1fb0bde60c7219243837f43cb76b8bd8"
      "c0fffdffd68006b1");
  ok = ok && n == want.size() && memcmp(f, want.data(), n) == 0;
  size_t bl = 0;
  std::unique_ptr<Links> back(new Links());
  ok = ok && s->open(f, n, bl);
  Reader r(f + kHeader, bl);
  r.u32();
  ok = ok && r.u8() == kExtLinks && readLinks(r, *back) && back->flags == kLinksRelays &&
       back->count == 2 && back->heard[0].id == 0x55667788 && back->heard[0].pct == 97 &&
       back->heard[1].id == 0x99aabbcc && back->heard[1].pct == 64;
  c.check(ok, "links: the link group's key and id; a sealed links packet (ext 3) matches the "
              "reference bytes and reads back");
  l->count = kMaxLinks;
  uint8_t big[kMaxBody];
  c.check(putLinks(big, sizeof big, 0, *l) == 7 + kMaxLinks * 5 &&
              kHeader + 7 + kMaxLinks * 5 + kTag == 203,
          "links: 32 boards listed is 203 bytes on the air");
}

void testRelayRule(Checker &c) {
  using N = Neighbours;
  constexpr uint32_t kMe = 0x01, kA = 0x0A, kB = 0x0B, kC = 0x0C;
  std::unique_ptr<N> nb(new N());
  std::unique_ptr<Links> l(new Links());
  auto say = [&](uint32_t who, std::initializer_list<std::pair<uint32_t, uint8_t>> heard,
                 uint32_t at) {
    l->count = 0;
    for (const auto &p : heard) {
      l->heard[l->count].id = p.first;
      l->heard[l->count].pct = p.second;
      ++l->count;
    }
    nb->links(who, *l, at);
  };
  uint32_t t = 1000, who = 0;
  // This board and A hear each other; nobody else is about.
  nb->heard(kA, 1, 1, -60, t);
  bool ok = !nb->needed(kA, kA, t);  // A's voice, straight from A: links or none, nobody needs it
  say(kA, {{kMe, 100}}, t);
  ok = ok && !nb->needed(kA, kA, t);
  c.check(ok, "relay: two boards that hear each other don't relay each other's voice");

  // C comes: heard here, no links from it yet: A's voice is relayed for it (today's flooding).
  nb->heard(kC, 1, 1, -70, t);
  ok = nb->needed(kA, kA, t, &who) && who == kC;
  // Its links list this board, not A: still relayed for it; and C's voice for A, which lists
  // only this board.
  say(kC, {{kMe, 95}}, t);
  ok = ok && nb->needed(kA, kA, t, &who) && who == kC && nb->needed(kC, kC, t, &who) && who == kA;
  c.check(ok, "relay: no links means relay; a third board one of them doesn't hear gets it "
              "relayed, both ways");

  // C hears A at 90%: not relayed; at 60% (under kGoodPct): relayed.
  say(kC, {{kMe, 95}, {kA, 90}}, t);
  ok = !nb->needed(kA, kA, t);
  say(kC, {{kMe, 95}, {kA, 60}}, t);
  ok = ok && nb->needed(kA, kA, t);
  c.check(ok, "relay: a board that hears the sender well isn't relayed for; one that hears it "
              "under 80% is");

  // A copy relayed by B (origin A, out of this board's range): C, heard here, needs it only if
  // it hears neither B (that copy) nor A (the first).
  std::unique_ptr<N> keep(nb.release());
  nb.reset(new N());
  nb->heard(kB, 1, 1, -60, t);
  nb->heard(kC, 1, 1, -60, t);
  say(kB, {{kMe, 100}, {kC, 100}}, t);
  say(kC, {{kMe, 100}, {kB, 100}}, t);
  ok = !nb->needed(kA, kB, t);
  say(kC, {{kMe, 100}, {kA, 100}}, t);
  ok = ok && !nb->needed(kA, kB, t);
  say(kC, {{kMe, 100}}, t);
  ok = ok && nb->needed(kA, kB, t, &who) && who == kC && nb->needed(kA, 0, t);
  c.check(ok, "relay: a relayed copy goes on only for a board that hears neither that relay nor "
              "the origin (an unknown relay: the origin alone)");

  // Lists expire: C's links older than kListMs count as none (relayed for); C not heard for
  // kHeardMs isn't a neighbour (not relayed for).
  say(kC, {{kMe, 100}, {kB, 100}}, t);
  ok = !nb->needed(kA, kB, t + N::kListMs - 1);
  nb->heard(kB, 1, 2, -60, t + N::kListMs);
  nb->heard(kC, 1, 2, -60, t + N::kListMs);
  say(kB, {{kMe, 100}, {kC, 100}}, t + N::kListMs);
  ok = ok && nb->needed(kA, kB, t + N::kListMs, &who) && who == kC;
  const uint32_t later = t + N::kListMs + N::kHeardMs;
  nb->heard(kB, 1, 3, -60, later);
  say(kB, {{kMe, 100}}, later);
  ok = ok && !nb->needed(kA, kB, later);
  c.check(ok, "relay: links older than 16 s count as none; a board not heard for 20 s isn't "
              "relayed for");

  // The share heard: of 64 packets, every 4th lost: 75%; a new boot counts afresh; mine() lists
  // the boards heard within kHeardMs, with theirs.
  nb.reset(new N());
  for (uint32_t q = 1; q <= 65; ++q) {  // the window: 2 to 65, 16 of them lost
    if (q % 4) nb->heard(kA, 3, q, -60, t + q);
  }
  for (uint32_t q = 10; q <= 20; ++q) nb->heard(kB, 5, q, -60, t + 100);
  ok = nb->size() == 2 && nb->at(0).pct() == 75 && nb->at(1).pct() == 100;
  nb->heard(kA, 4, 1, -60, t + 200);
  ok = ok && nb->at(0).pct() == 100;
  nb->mine(*l, t + 100 + N::kHeardMs - 1);
  ok = ok && l->count == 2;
  nb->mine(*l, t + 100 + N::kHeardMs);
  ok = ok && l->count == 1 && l->heard[0].id == kA;
  c.check(ok, "links: the share of a board's packets heard straight (75% with every 4th lost), "
              "afresh at its next boot; this board's links list who it heard in the last 20 s");
}

// Reachable on battery: the poll and its answer (docs/comms-protocol.md has these bytes), the
// sleeper's channel check, the hold after a wake, the rings seen and what a poll is told.
void testReachPackets(Checker &c) {
  const crypto::GroupKeys k = crypto::derive(kSecret, false);
  std::unique_ptr<crypto::Sealer> s(new crypto::Sealer());
  s->setKey(k.key);
  auto seal = [&](uint32_t seq, const uint8_t *body, size_t n, uint8_t *f) {
    Header h;
    h.type = kExt;
    h.ttl = 0;
    h.group = k.id;
    h.sender = 0x11223344;
    h.boot = 7;
    h.seq = seq;
    putHeader(h, f);
    memcpy(f + kHeader, body, n);
    return s->seal(f, n);
  };
  Poll p;
  p.flags = kPollEcho;
  p.battery = 87;
  p.intervalS = 5;
  p.nameLen = 7;
  memcpy(p.name, "KITCHEN", 7);
  uint8_t body[64], f[kMaxFrame];
  size_t n = putPoll(body, sizeof body, 0, p);
  n = seal(6, body, n, f);
  std::vector<uint8_t> want = unhex(
      "c71f0000e13e368e443322110700000006000000ba7508f5bb634115d8976b9dfb518dfe82b0aa36a4596ae029c"
      "81bbf82a42dd2dc4ad8cd21");
  c.check(n == want.size() && memcmp(f, want.data(), n) == 0,
          "reach: a sealed poll matches the reference bytes");
  size_t len = 0;
  bool ok = s->open(f, n, len);
  Reader r(f + kHeader, len);
  r.u32();
  Poll q;
  ok = ok && r.u8() == kExtPoll && readPoll(r, q) && q.flags == kPollEcho && q.battery == 87 &&
       q.intervalS == 5 && q.nameLen == 7 && memcmp(q.name, "KITCHEN", 7) == 0 &&
       reachExtFor(f + kHeader, len) == 0;
  c.check(ok, "reach: the poll reads back, for all (not one board's alone)");

  Answer a;
  a.to = 0x99AABBCC;
  a.what = kWaitRing;
  a.channel = 6;
  a.flags = 0x01;
  a.dialer = 0x11223344;
  a.ring = 9;
  n = putAnswer(body, sizeof body, 0, a);
  n = seal(7, body, n, f);
  want = unhex(
      "c71f0000e13e368e443322110700000007000000de01bdf96a08528e1d7874d08729caf35fc9bb4e4dbf3a72b58"
      "a79b6b9fe7ee7a37fade383044b5b2c494fa1d1a7841f");
  c.check(n == want.size() && memcmp(f, want.data(), n) == 0,
          "reach: a sealed answer matches the reference bytes");
  ok = s->open(f, n, len);
  Reader r2(f + kHeader, len);
  r2.u32();
  Answer b;
  ok = ok && r2.u8() == kExtAnswer && readAnswer(r2, b) && b.to == 0x99AABBCC &&
       b.what == kWaitRing && b.channel == 6 && b.dialer == 0x11223344 && b.ring == 9 &&
       reachExtFor(f + kHeader, len) == 0x99AABBCC;
  c.check(ok, "reach: the answer reads back, for the sleeper alone");
  Reader shortR(body, 12);
  shortR.u32();
  shortR.u8();
  c.check(!readAnswer(shortR, b), "reach: a short answer is refused");
}

void testSleeper(Checker &c) {
  SleepPlan p;
  Sleeper::begin(p, 6, 5);
  // The first poll checks the channel; then one every 3 minutes (36 polls at 5 s).
  Sleeper::Next n = Sleeper::next(p, 5);
  bool ok = n.flags == kPollEcho && !n.sweep && p.channel == 6;
  Sleeper::result(p, 5, n, true);
  int echoes = 0;
  for (int i = 0; i < 72; ++i) {
    n = Sleeper::next(p, 5);
    if (n.flags & kPollEcho) ++echoes;
    Sleeper::result(p, 5, n, (n.flags & kPollEcho) != 0);
  }
  ok = ok && echoes == 2 && p.misses == 0;
  c.check(ok, "reach: the sleeper checks the channel at first, then every 3 minutes");
  // Three echoes unanswered in a row: a sweep.
  int polls = 0;
  bool swept = false;
  while (polls < 200 && !swept) {
    n = Sleeper::next(p, 5);
    ++polls;
    if (n.sweep) {
      swept = true;
      break;
    }
    Sleeper::result(p, 5, n, false);
  }
  ok = swept && p.misses == 3 && (n.flags & kPollSweep) && polls > 36 * 2 && polls <= 36 * 3 + 1;
  c.check(ok, "reach: three echoes unanswered in a row sweep the channels");
  Sleeper::sweepResult(p, 5, 11);
  n = Sleeper::next(p, 5);
  c.check(p.channel == 11 && p.misses == 0 && !n.sweep, "reach: a sweep that finds boards moves");
  // A sweep that finds nobody: the channel stays, the next sweep waits half an hour.
  for (int i = 0; i < 200 && !Sleeper::next(p, 5).sweep; ++i) {
    n = Sleeper::next(p, 5);
    Sleeper::result(p, 5, n, false);
  }
  Sleeper::sweepResult(p, 5, 0);
  int until = 0;
  for (; until < 1000; ++until) {
    n = Sleeper::next(p, 5);
    if (n.sweep) break;
    Sleeper::result(p, 5, n, false);
  }
  c.check(p.channel == 11 && until >= 359 && until <= 361,
          "reach: a sweep that finds nobody keeps the channel and waits 30 minutes");
  // Any answer means the boards are on this channel (an answer with something waiting too).
  p.misses = 2;
  n = Sleeper::next(p, 5);
  Sleeper::result(p, 5, n, true);
  c.check(p.misses == 0, "reach: any answer resets the misses");
}

void testWoken(Checker &c) {
  using V = WokenHold::Verdict;
  WokenHold h;
  WokenHold::Seen s;
  // Woken, nothing comes: back to sleep after 15 s.
  h.begin(1000, kWaitRing);
  bool ok = h.step(5000, s) == V::Stay && h.step(15999, s) == V::Stay;
  ok = ok && h.step(16000, s) == V::Sleep && !h.active();
  c.check(ok, "reach: woken for nothing that comes, back to sleep after 15 s");
  // A ring for 20 s: 30 s after it ends.
  h.begin(0, kWaitRing);
  s.ringing = true;
  ok = h.step(1000, s) == V::Stay && h.step(20000, s) == V::Stay;
  s.ringing = false;
  ok = ok && h.step(49999, s) == V::Stay && h.step(50000, s) == V::Sleep;
  c.check(ok, "reach: after a ring, back to sleep 30 s after it ends");
  // A key: on.
  h.begin(0, kWaitRing);
  s.ringing = true;
  h.step(1000, s);
  s.keyed = true;
  ok = h.step(2000, s) == V::On && !h.active();
  s = WokenHold::Seen();
  c.check(ok, "reach: a key makes it an ordinary board that's on");
  // Sound from a mic that goes off: 30 s; from a mic that stays on, quiet: 60 s.
  h.begin(0, kWaitVoice);
  s.sound = true;
  s.micOn = true;
  h.step(3000, s);
  s.sound = false;
  s.micOn = false;
  ok = h.step(32999, s) == V::Stay && h.step(33000, s) == V::Sleep;
  h.begin(0, kWaitVoice);
  s.sound = true;
  s.micOn = true;
  h.step(3000, s);
  s.sound = false;
  ok = ok && h.step(33000, s) == V::Stay && h.step(62999, s) == V::Stay &&
       h.step(63000, s) == V::Sleep;
  s = WokenHold::Seen();
  c.check(ok, "reach: 30 s after a sound, 60 s while its mic stays on (a quiet room)");
  // A transfer keeps it up until 30 s after it ends.
  h.begin(0, kWaitMessage);
  s.transfer = true;
  ok = h.step(40000, s) == V::Stay;
  s.transfer = false;
  ok = ok && h.step(69999, s) == V::Stay && h.step(70000, s) == V::Sleep;
  c.check(ok, "reach: a message coming in keeps it up, then 30 s");
}

void testWaiting(Checker &c) {
  RingWatch w;
  RingWatch::Seen seen;
  const uint32_t sleeper = 0xABCD0001, other = 0xABCD0002, dialer = 0xABCD0003;
  // A ring to all of group 0, 2 s in.
  w.heard(0, dialer, 9, 0, 2000, 10000);
  bool ok = w.ringFor(0, sleeper, 10500, seen) && seen.dialer == dialer && seen.ring == 9 &&
            !w.ringFor(1, sleeper, 10500, seen) && !w.ringFor(0, dialer, 10500, seen);
  // No packet for 3.5 s: over. Its packets again: on.
  ok = ok && !w.ringFor(0, sleeper, 13500, seen);
  w.heard(0, dialer, 9, 0, 6000, 14000);
  ok = ok && w.ringFor(0, sleeper, 14100, seen);
  // An answer ends it; a ring to another board isn't for the sleeper.
  w.over(dialer, 9);
  ok = ok && !w.ringFor(0, sleeper, 14200, seen);
  w.heard(0, dialer, 10, other, 0, 15000);
  ok = ok && !w.ringFor(0, sleeper, 15100, seen) && w.ringFor(0, other, 15100, seen);
  // 60 s after it began, a ring is over whatever comes.
  w.heard(0, dialer, 10, other, 59000, 74000);
  ok = ok && w.ringFor(0, other, 74500, seen) && !w.ringFor(0, other, 75100, seen);
  c.check(ok, "reach: rings seen go by, end at an answer, 3.5 s silence or 60 s");

  WaitingNow n;
  ok = waitingFor(n, 0, sleeper, 5, 100000) == 0;
  n.ringOn = true;
  ok = ok && waitingFor(n, 0, sleeper, 5, 100000) == kWaitRing;
  n = WaitingNow();
  // Voice: a mic to all that opened the conversation, with sound within 5 s + 2 s.
  WaitingNow::MicNow &m = n.mics[n.micCount++];
  m.group = 0;
  m.board = other;
  m.opened = true;
  m.soundMs = 100000;
  ok = ok && waitingFor(n, 0, sleeper, 5, 107000) == kWaitVoice &&
       waitingFor(n, 0, sleeper, 5, 107001) == 0 && waitingFor(n, 1, sleeper, 5, 101000) == 0;
  // Not one that joined a conversation already on (the sleeper wouldn't play it), nor the
  // sleeper's own, nor one to another board; one to the sleeper alone, yes.
  m.opened = false;
  ok = ok && waitingFor(n, 0, sleeper, 5, 101000) == 0;
  m.opened = true;
  m.board = sleeper;
  ok = ok && waitingFor(n, 0, sleeper, 5, 101000) == 0;
  m.board = other;
  m.target = dialer;
  ok = ok && waitingFor(n, 0, sleeper, 5, 101000) == 0;
  m.target = sleeper;
  m.opened = false;
  ok = ok && waitingFor(n, 0, sleeper, 5, 101000) == kWaitVoice;
  c.check(ok, "reach: voice waits for a sleeper that would hear it, sounding within the interval");
  // A message: to all or to it, until it says it has it whole.
  n = WaitingNow();
  n.messageOn = true;
  n.msgGroup = 0;
  n.msgSender = other;
  n.msgTo = 0;
  ok = waitingFor(n, 0, sleeper, 5, 0) == kWaitMessage;
  n.msgTo = dialer;
  ok = ok && waitingFor(n, 0, sleeper, 5, 0) == 0;
  n.msgTo = sleeper;
  n.deliveredTo[n.delivered++] = sleeper;
  ok = ok && waitingFor(n, 0, sleeper, 5, 0) == 0;
  c.check(ok, "reach: a message waits for a sleeper it's for, until it has it whole");
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
  testMicPackets(c);
  testGate(c);
  testEchoGate(c);
  testGateDrain(c);
  testHistory(c);
  testHeard(c);
  testMixer(c);
  testLinks(c);
  testRelayRule(c);
  testReachPackets(c);
  testSleeper(c);
  testWoken(c);
  testWaiting(c);
  out.printf("commstest: %s (%d failed) in %lu ms\n", c.failed ? "FAILED" : "all passed",
             c.failed, static_cast<unsigned long>(millis() - t0));
  return c.failed == 0;
}

}  // namespace comms

#endif  // INKDECK_COMMS
