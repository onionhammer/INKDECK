#include "comms_mesh.h"

#include "features.h"

#if INKDECK_COMMS  // the whole file (features.h)

namespace comms {
namespace wire {

size_t putLinks(uint8_t *body, size_t cap, uint32_t sentAt, const Links &l) {
  Writer w(body, cap);
  w.u32(sentAt);
  w.u8(kExtLinks);
  w.u8(l.flags);
  const uint8_t n = l.count < kMaxLinks ? l.count : kMaxLinks;
  w.u8(n);
  for (uint8_t i = 0; i < n; ++i) {
    w.u32(l.heard[i].id);
    w.u8(l.heard[i].pct);
  }
  return w.ok() ? w.size() : 0;
}

bool readLinks(Reader &r, Links &l) {
  l.flags = r.u8();
  const uint8_t n = r.u8();
  l.count = 0;
  for (uint8_t i = 0; i < n && r.ok(); ++i) {
    const uint32_t id = r.u32();
    const uint8_t pct = r.u8();
    if (!r.ok() || l.count >= kMaxLinks) break;
    l.heard[l.count].id = id;
    l.heard[l.count].pct = pct > 100 ? 100 : pct;
    ++l.count;
  }
  return r.ok();
}

uint8_t Neighbours::Neighbour::pct() const {
  if (!heardMs) return 0;
  const uint32_t span = top - first + 1 < 64 ? top - first + 1 : 64;
  const uint64_t m = span >= 64 ? mask : mask & ((1ull << span) - 1);
  return static_cast<uint8_t>(__builtin_popcountll(m) * 100 / span);
}

Neighbours::Neighbour *Neighbours::find(uint32_t id) {
  for (size_t i = 0; i < n_; ++i) {
    if (e_[i].id == id) return &e_[i];
  }
  return nullptr;
}

const Neighbours::Neighbour *Neighbours::find(uint32_t id) const {
  for (size_t i = 0; i < n_; ++i) {
    if (e_[i].id == id) return &e_[i];
  }
  return nullptr;
}

void Neighbours::heard(uint32_t id, uint32_t boot, uint32_t seq, int rssi, uint32_t now) {
  Neighbour *e = find(id);
  if (!e) {
    if (n_ < kBoards) {
      e = &e_[n_++];
    } else {
      e = &e_[0];
      for (size_t i = 1; i < n_; ++i) {
        if (now - e_[i].heardMs > now - e->heardMs) e = &e_[i];
      }
    }
    *e = Neighbour();
    e->id = id;
  }
  e->heardMs = now;
  e->rssi = rssi;
  if (!e->boot || boot > e->boot) {  // its first packet, or a new boot: count afresh
    e->boot = boot;
    e->first = e->top = seq;
    e->mask = 1;
    e->got = 1;
    e->missed = 0;
    for (uint32_t &r : e->runs) r = 0;
    return;
  }
  if (boot < e->boot) return;  // an earlier boot's, late
  ++e->got;
  if (seq > e->top) {
    const uint32_t shift = seq - e->top;
    if (shift > 1) {
      const uint32_t gap = shift - 1;
      e->missed += gap;
      ++e->runs[gap == 1 ? 0 : gap <= 3 ? 1 : gap <= 15 ? 2 : 3];
    }
    e->mask = shift >= 64 ? 0 : e->mask << shift;
    e->mask |= 1;
    e->top = seq;
  } else {
    const uint32_t back = e->top - seq;
    if (back < 64) e->mask |= 1ull << back;
    if (back && e->missed) --e->missed;  // late, not lost (the gap it was counted in stays)
    if (seq < e->first) e->first = seq;
  }
}

void Neighbours::links(uint32_t id, const Links &l, uint32_t now) {
  Neighbour *e = find(id);
  if (!e) return;  // heard() came first, for the same packet
  e->listMs = now;
  e->flags = l.flags;
  e->count = l.count;
  for (uint8_t i = 0; i < l.count; ++i) e->heard[i] = l.heard[i];
}

void Neighbours::mine(Links &out, uint32_t now) const {
  out.count = 0;
  for (size_t i = 0; i < n_ && out.count < kMaxLinks; ++i) {
    const Neighbour &e = e_[i];
    if (!e.heardMs || now - e.heardMs >= kHeardMs) continue;
    out.heard[out.count].id = e.id;
    out.heard[out.count].pct = e.pct();
    ++out.count;
  }
}

bool Neighbours::hears(const Neighbour &x, uint32_t id) {
  if (!id) return false;
  for (uint8_t i = 0; i < x.count; ++i) {
    if (x.heard[i].id == id) return x.heard[i].pct >= kGoodPct;
  }
  return false;
}

bool Neighbours::needed(uint32_t origin, uint32_t from, uint32_t now, uint32_t *forWhom) const {
  for (size_t i = 0; i < n_; ++i) {
    const Neighbour &x = e_[i];
    if (!x.heardMs || now - x.heardMs >= kHeardMs) continue;  // not a neighbour now
    if (x.id == origin || x.id == from) continue;             // it has it
    const bool listed = x.listMs && now - x.listMs < kListMs;
    if (listed && (hears(x, from) || hears(x, origin))) continue;  // it heard that copy
    if (forWhom) *forWhom = x.id;
    return true;
  }
  return false;
}

void Neighbours::clear() { n_ = 0; }

}  // namespace wire
}  // namespace comms

#endif  // INKDECK_COMMS
