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
#include "comms_monitor.h"
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
  for (uint8_t t = kRing; t <= kExt; ++t) {
    h.type = t;
    putHeader(h, f);
    ok = ok && parseHeader(f, kHeader + kTag + 8, p) && p.type == t && knownType(t);
  }
  f[1] = static_cast<uint8_t>(kVersion << 4);  // type 0
  ok = ok && !parseHeader(f, kHeader + kTag + 8, p) && kExt == 15 && kLastType == 15;
  c.check(ok, "the new types parse, 15 (ext) the last there can be; type 0 doesn't");

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


// The monitor's checks run in their own frames (noinline): the dev tools' task has 4 KB of stack,
// and LTO would add them all to selfTest()'s.
__attribute__((noinline)) void testMonitorPackets(Checker &c) {
  const crypto::GroupKeys k = crypto::derive(kSecret, false);
  std::unique_ptr<crypto::Sealer> s(new crypto::Sealer());
  s->setKey(k.key);
  // On the heap: the dev tools' task has a small stack.
  struct Bufs {
    uint8_t f[kMaxFrame], b[kMaxBody];
    StillHere st, back;
    MonitorAudio a, ab, full, x;
    History hist;
  };
  std::unique_ptr<Bufs> u(new Bufs());
  uint8_t *f = u->f;
  uint8_t *b = u->b;
  StillHere &st = u->st;
  StillHere &back = u->back;
  MonitorAudio &a = u->a;
  MonitorAudio &ab = u->ab;
  MonitorAudio &full = u->full;
  MonitorAudio &x = u->x;
  History &hist = u->hist;
  // A still-here from sender 11223344, boot 7, seq 3: sending, on USB, no battery, threshold
  // -30, peak -24, on 600 s, bar 5 at -18, pre-roll 25 frames, for 55667788 alone
  // (docs/comms-protocol.md has these bytes).
  Header h;
  h.type = kExt;
  h.ttl = 3;
  h.group = k.id;
  h.sender = 0x11223344;
  h.boot = 7;
  h.seq = 3;
  putHeader(h, f);
  st.flags = kStillSending | kStillUsb;
  st.battery = 0xFF;
  st.threshold = 30;
  st.peak = 24;
  st.onS = 600;
  st.bar = 5;
  st.barPeak = 18;
  st.listeners.n = 1;
  st.listeners.ids[0] = 0x55667788;
  size_t n = s->seal(f, putStillHere(f + kHeader, kMaxBody, 0, st));
  std::vector<uint8_t> want = unhex(
      "c71f0300e13e368e443322110700000003000000e74688b9939c604d7e97b0aa142e8a21bd52a8396506401237bb"
      "8bb8155b460b4a4194992ab6");
  c.check(n == want.size() && memcmp(f, want.data(), n) == 0,
          "a sealed still-here (type 15, ext 2) matches the reference bytes");
  size_t body = 0;
  bool ok = s->open(f, n, body);
  Reader r(f + kHeader, body);
  r.u32();
  ok = ok && r.u8() == kStillHere && readStillHere(r, back) && back.flags == st.flags &&
       back.threshold == 30 && back.peak == 24 && back.onS == 600 && back.bar == 5 &&
       back.barPeak == 18 && back.preFrames == 25 && back.listeners.n == 1 &&
       back.listeners.allows(0x55667788) && !back.listeners.allows(0x11111111) && r.left() == 0;
  c.check(ok, "the still-here reads back field by field, its listener list too");

  // Monitor audio, seq 4: to all, sound 2, frame 9, level -21, predictor -1234, index 40, 8 bytes.
  h.seq = 4;
  putHeader(h, f);
  a.sound = 2;
  a.frame = 9;
  a.level = 21;
  a.predictor = -1234;
  a.index = 40;
  a.bytes = 8;
  for (uint8_t i = 0; i < 8; ++i) a.data[i] = static_cast<uint8_t>(i + 1);
  n = s->seal(f, putMonitorAudio(f + kHeader, kMaxBody, 0, a));
  want = unhex(
      "c71f0300e13e368e443322110700000004000000a8c23f21808172d6dd8b0b0da60caee3a3b1e53cd7201641f9f7"
      "2080f0d7effe32e380e4eaf1e9");
  ok = n == want.size() && memcmp(f, want.data(), n) == 0;
  ok = ok && s->open(f, n, body);
  Reader ra(f + kHeader, body);
  ra.u32();
  ok = ok && ra.u8() == kMonitorAudio && readMonitorAudio(ra, ab) && ab.listeners.all() &&
       ab.sound == 2 && ab.frame == 9 && ab.level == 21 && ab.predictor == -1234 &&
       ab.index == 40 && ab.bytes == 8 && ab.data[7] == 8 && extFor(f + kHeader, body) == 0;
  c.check(ok, "a sealed monitor frame (ext 1) matches the reference bytes and reads back");

  // Sizes: a full frame to 8 listeners, a history of 60 bars, under 250; and who each is for.
  full.bytes = kFrameBytes;
  full.listeners.n = ListenerSet::kMax;
  for (size_t i = 0; i < ListenerSet::kMax; ++i) full.listeners.ids[i] = 0x100 + i;
  const size_t fullLen = putMonitorAudio(b, kMaxBody, 0, full);
  hist.to = 0x55667788;
  hist.count = MonitorHistory::kBars;
  const size_t histLen = putHistory(b, kMaxBody, 0, hist);
  ok = fullLen && kHeader + fullLen + kTag == 243 && kHeader + histLen + kTag == 108;
  full.listeners.n = 1;
  full.listeners.ids[0] = 0x55667788;
  ok = ok && extFor(b, putMonitorAudio(b, kMaxBody, 0, full)) == 0x55667788;
  ok = ok && extFor(b, putToRoom(b, kMaxBody, 0, kListening, 0x11223344, 0)) == 0x11223344 &&
       extFor(b, putToRoom(b, kMaxBody, 0, kHistoryAsk, 0x11223344, 0)) == 0x11223344 &&
       extFor(b, putHistory(b, kMaxBody, 0, hist)) == 0x55667788 &&
       extFor(b, putStillHere(b, kMaxBody, 0, st)) == 0 &&
       extFor(b, putMonitorOff(b, kMaxBody, 0, kOffStopped)) == 0;
  // A list longer than 8 is refused.
  b[0] = b[1] = b[2] = b[3] = 0;
  b[4] = kMonitorAudio;
  b[5] = 9;
  Reader bad(b + 5, 60);
  ok = ok && !readMonitorAudio(bad, x);
  c.check(ok, "monitor frames to 8 listeners are 243 bytes, a 2 h history 108; each for whom it "
              "says; a list of 9 is refused");
}

__attribute__((noinline)) void testGate(Checker &c) {
  // -30 dBFS threshold; 100 quiet frames, 10 over it, then quiet.
  std::unique_ptr<MonitorGate> g(new MonitorGate());
  g->setThreshold(-30);
  int sent = 0, firsts = 0;
  uint32_t firstAt = 0, lastAt = 0;
  for (uint32_t i = 0; i < 400; ++i) {
    const float db = i >= 100 && i < 110 ? -20.0f : -45.0f;
    const MonitorGate::Step s = g->frame(db);
    if (s.send) {
      if (!sent) firstAt = i;
      lastAt = i;
      ++sent;
    }
    if (s.first) ++firsts;
  }
  const uint32_t P = MonitorGate::kPreFrames, H = MonitorGate::kHangFrames;
  // The first frame sent is frame 75 (the pre-roll), sent at frame 100; the last, frame 109 + H,
  // sent P later.
  bool ok = sent == static_cast<int>(P + 10 + H) && firsts == 1 && firstAt == 100 &&
            lastAt - P == 109 + H && !g->sending();
  c.check(ok, "gate: a sound goes with 0.5 s before it and 2 s after, half a second late");

  g.reset(new MonitorGate());
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

  // Two loud frames 2 s apart are one sound; 3 s apart, two. A loud frame right after a reset
  // has only the pre-roll there is.
  g.reset(new MonitorGate());
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
    const MonitorGate::Step s = g->frame(i == 3 ? -10.0f : -50.0f);
    if (s.send && !sent) firstAt = i;
    sent += s.send;
  }
  c.check(ok && firstAt == P && sent == static_cast<int>(30 - P),
          "gate: sounds 2 s apart are one, 5 s apart two; after a pause the pre-roll starts there");
}

__attribute__((noinline)) void testHistory(Checker &c) {
  // The room: 130 minutes, louder each 2 minutes, the mic off for bars 20 and 21.
  std::unique_ptr<MonitorHistory> h(new MonitorHistory());
  for (uint32_t ms = 0; ms < 130 * 60000u; ms += 1000) {
    const uint32_t bar = ms / MonitorHistory::kBarMs;
    if (bar == 20 || bar == 21) continue;
    h->note(ms, -90.0f + bar + (ms % 7000 == 0 ? 5 : 0));
  }
  bool ok = h->top() == 64 && h->first() == 5 && h->at(4) == kNoLevel && h->at(5) == 80 &&
            h->at(64) == 21 && h->at(20) == kNoLevel && h->at(22) == 63 && h->at(65) == kNoLevel;
  c.check(ok, "history: 60 bars of 2 minutes, the loudest frame each, the oldest forgotten");

  // A listener: the history it asked for, then bars from still-heres, one skipped (no link).
  std::unique_ptr<MonitorHistory> l(new MonitorHistory());
  for (uint16_t b = h->first(); b <= h->top(); ++b) l->set(b, h->at(b));
  ok = l->top() == 64 && l->at(5) == 80 && l->at(20) == kNoLevel && l->at(64) == 21;
  l->set(66, 30);
  ok = ok && l->top() == 66 && l->at(65) == kNoLevel && l->at(7) == 78 && l->at(6) == kNoLevel;
  l->set(200, 10);
  c.check(ok && l->at(66) == kNoLevel && l->at(200) == 10 && levelByte(-96.5f) == 96 &&
              levelByte(3) == 0 && levelDb(kNoLevel) == -96,
          "history: a listener mirrors it; a gap clears the bars skipped");
}

__attribute__((noinline)) void testListen(Checker &c) {
  using L = ListenState;
  std::unique_ptr<L> s(new L());
  s->setThreshold(-30);
  s->start(0, 0);
  bool loud = false;
  // A soft sound (-25: over the threshold, not 12 dB over), then a loud one.
  for (uint32_t t = 1000; t < 3000; t += 20) s->audio(t, 1, -25, loud);
  bool ok = s->link() == L::Link::Sound && !loud;
  s->tick(3000 + L::kSoundEndMs);
  ok = ok && s->link() == L::Link::Quiet && s->events().size() == 1 && !s->events()[0].loud &&
       s->events()[0].endMs - s->events()[0].startMs == 1980;
  int becameLoud = 0;
  for (uint32_t t = 5000; t < 6000; t += 20) {
    s->audio(t, 2, t < 5500 ? -30.0f : -18.0f, loud);
    becameLoud += loud;
  }
  ok = ok && becameLoud == 1 && s->events().back().loud && s->events().back().peakDb == -18;
  c.check(ok, "listener: SOUND, then QUIET 0.6 s after; LOUD 12 dB over the threshold, once");

  // Still-heres every 2 s until 10 s; then nothing: NO LINK at 30 s, the alarm, ack, link back.
  for (uint32_t t = 6000; t <= 10000; t += 2000) s->heard(t);
  s->tick(29999);
  ok = s->link() == L::Link::Quiet && !s->alarm();
  s->tick(30000);
  ok = ok && s->link() == L::Link::NoLink && s->alarm() && s->sinceMs() == 10000;
  ok = ok && s->ack() && !s->alarm() && s->link() == L::Link::NoLink && !s->ack();
  s->heard(45000);
  const L::Event &e = s->events().back();
  ok = ok && s->link() == L::Link::Quiet && e.noLink && !e.open && e.endMs - e.startMs == 35000;
  s->tick(66000);  // lost again: the alarm sounds again
  ok = ok && s->alarm();
  c.check(ok, "listener: NO LINK 20 s after the last packet, its alarm until ack, the event 35 s");

  s->heard(67000);
  ok = s->off(68000) && s->link() == L::Link::Stopped && !s->alarm() && !s->on();
  bool dummy;
  ok = ok && !s->audio(69000, 3, -10, dummy) && !s->tick(200000) && s->link() == L::Link::Stopped;
  ListenerSet set;
  ok = ok && set.allows(42);
  set.n = 2;
  set.ids[0] = 7;
  set.ids[1] = 9;
  c.check(ok && set.allows(9) && !set.allows(42) && !set.allows(0),
          "listener: a clean off is STOPPED, no alarm; the allow-list lets in only those listed");
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
  testMonitorPackets(c);
  testGate(c);
  testHistory(c);
  testListen(c);
  out.printf("commstest: %s (%d failed) in %lu ms\n", c.failed ? "FAILED" : "all passed",
             c.failed, static_cast<unsigned long>(millis() - t0));
  return c.failed == 0;
}

}  // namespace comms

#endif  // INKDECK_COMMS
