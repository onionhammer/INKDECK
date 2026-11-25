#include "comms_wire.h"

#include "features.h"

#if INKDECK_COMMS  // the whole file (features.h)

#include <string.h>

namespace comms {
namespace wire {

void put16(uint8_t *p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
}

void put32(uint8_t *p, uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}

uint16_t get16(const uint8_t *p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }

uint32_t get32(const uint8_t *p) {
  return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 |
         static_cast<uint32_t>(p[2]) << 16 | static_cast<uint32_t>(p[3]) << 24;
}

void putHeader(const Header &h, uint8_t *f) {
  f[0] = kMagic;
  f[1] = static_cast<uint8_t>(kVersion << 4 | (h.type & 0x0F));
  f[2] = static_cast<uint8_t>((h.hops & 0x0F) << 4 | (h.ttl & 0x0F));
  f[3] = 0;  // reserved: 0 in version 1, authenticated
  put32(f + 4, h.group);
  put32(f + 8, h.sender);
  put32(f + 12, h.boot);
  put32(f + 16, h.seq);
}

bool parseHeader(const uint8_t *f, size_t len, Header &out) {
  if (len < kHeader + kTag || len > kMaxFrame) return false;
  if (f[0] != kMagic || f[1] >> 4 != kVersion || f[3] != 0) return false;
  out.type = f[1] & 0x0F;
  if (out.type == 0) return false;
  out.ttl = f[2] & 0x0F;
  out.hops = f[2] >> 4;
  out.group = get32(f + 4);
  out.sender = get32(f + 8);
  out.boot = get32(f + 12);
  out.seq = get32(f + 16);
  return out.sender != 0;
}

void associatedData(const uint8_t *frame, uint8_t out[kHeader]) {
  memcpy(out, frame, kHeader);
  out[2] = 0;
}

void nonce(uint32_t sender, uint32_t boot, uint32_t seq, uint8_t out[kNonce]) {
  put32(out, sender);
  put32(out + 4, boot);
  put32(out + 8, seq);
}

bool forward(uint8_t *f) {
  const uint8_t ttl = f[2] & 0x0F;
  const uint8_t hops = f[2] >> 4;
  if (ttl == 0) return false;
  f[2] = static_cast<uint8_t>((hops < 15 ? hops + 1 : 15) << 4 | (ttl - 1));
  return true;
}

// --- Writer and Reader -----------------------------------------------------------------------

void Writer::u8(uint8_t v) { bytes(&v, 1); }

void Writer::u16(uint16_t v) {
  uint8_t b[2];
  put16(b, v);
  bytes(b, 2);
}

void Writer::u32(uint32_t v) {
  uint8_t b[4];
  put32(b, v);
  bytes(b, 4);
}

void Writer::bytes(const void *v, size_t n) {
  if (!ok_ || n > cap_ - n_) {
    ok_ = false;
    return;
  }
  memcpy(p_ + n_, v, n);
  n_ += n;
}

const uint8_t *Reader::bytes(size_t n) {
  if (!ok_ || n > len_ - n_) {
    ok_ = false;
    return nullptr;
  }
  const uint8_t *at = p_ + n_;
  n_ += n;
  return at;
}

uint8_t Reader::u8() {
  const uint8_t *b = bytes(1);
  return b ? b[0] : 0;
}

uint16_t Reader::u16() {
  const uint8_t *b = bytes(2);
  return b ? get16(b) : 0;
}

uint32_t Reader::u32() {
  const uint8_t *b = bytes(4);
  return b ? get32(b) : 0;
}

// --- SeenCache -------------------------------------------------------------------------------

bool SeenCache::seen(uint32_t group, uint32_t sender, uint32_t boot, uint32_t seq) const {
  for (size_t i = 0; i < count_; ++i) {
    const Key &k = keys_[i];
    if (k.seq == seq && k.sender == sender && k.boot == boot && k.group == group) return true;
  }
  return false;
}

void SeenCache::add(uint32_t group, uint32_t sender, uint32_t boot, uint32_t seq) {
  keys_[next_] = {group, sender, boot, seq};
  next_ = (next_ + 1) % kSize;
  if (count_ < kSize) ++count_;
}

void SeenCache::clear() { next_ = count_ = 0; }

// --- ReplayGuard -----------------------------------------------------------------------------

ReplayGuard::Verdict ReplayGuard::accept(uint32_t sender, uint32_t boot, uint32_t seq,
                                         bool fresh) {
  Entry *e = nullptr;
  Entry *oldest = &entries_[0];
  for (Entry &x : entries_) {
    if (x.sender == sender && x.used) {
      e = &x;
      break;
    }
    if (!x.used || (oldest->used && x.used < oldest->used)) oldest = &x;
  }
  ++tick_;
  if (!e) {
    if (!fresh) return Verdict::Stale;
    *oldest = Entry{sender, boot, seq, 1, tick_};
    return Verdict::Ok;
  }
  if (boot != e->boot) {
    if (boot < e->boot) return Verdict::OldBoot;
    if (!fresh) return Verdict::Stale;
    *e = Entry{sender, boot, seq, 1, tick_};
    return Verdict::Ok;
  }
  if (seq > e->top) {
    const uint32_t shift = seq - e->top;
    e->mask = shift >= 64 ? 1 : (e->mask << shift) | 1;
    e->top = seq;
  } else {
    const uint32_t back = e->top - seq;
    if (back >= 64) return Verdict::TooOld;
    const uint64_t bit = uint64_t{1} << back;
    if (e->mask & bit) return Verdict::Duplicate;
    e->mask |= bit;
  }
  e->used = tick_;
  return Verdict::Ok;
}

void ReplayGuard::clear() {
  for (Entry &e : entries_) e = Entry{};
  tick_ = 0;
}

}  // namespace wire
}  // namespace comms

#endif  // INKDECK_COMMS
