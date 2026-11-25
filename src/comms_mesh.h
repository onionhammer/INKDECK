// Relaying only when needed (docs/comms-protocol.md, "Relaying"). Every board says, every few
// seconds, which boards it hears straight over the air and how well (a links packet, ext type 3,
// in the link group: sealed with a public key, so boards of every group read it, and never
// relayed). A board passes a voice or message frame on only when a board it hears straight would
// otherwise miss it: one that hears neither the frame's origin nor the board whose radio sent the
// copy this board got, or says nothing (an older board, or its links lost). Everything else
// (presence, rings, the mic's still-here, small packets) floods as before.
//
// Plain C++ with no Arduino or ESP-IDF calls, like comms_wire.h: the self-test drives it with a
// made-up clock. Not thread-safe: comms.cpp keeps it under its mutex.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "comms_wire.h"

namespace comms {
namespace wire {

// Ext type 3 (comms_mic.h has 1 and 2; 3 to 7 are for relaying, 8 and up for later): links.
constexpr uint8_t kExtLinks = 3;
constexpr uint8_t kLinksRelays = 0x01;  // its board relays for others
constexpr uint8_t kLinksLong = 0x02;    // its board sends at the long-range rate
constexpr size_t kMaxLinks = 32;        // boards a links packet lists (5 bytes each)
struct LinkHeard {
  uint32_t id = 0;
  uint8_t pct = 0;  // of that board's packets lately, the share heard straight from it, 0 to 100
};
struct Links {
  uint8_t flags = 0;
  uint8_t count = 0;
  LinkHeard heard[kMaxLinks];
};
size_t putLinks(uint8_t *body, size_t cap, uint32_t sentAt, const Links &l);
bool readLinks(Reader &r, Links &l);  // r just past the ext type

// The boards this board hears straight over the air (any group's packets, hops 0), how well, and
// what each says it hears; and the rule a voice or message frame is relayed by.
class Neighbours {
 public:
  static constexpr size_t kBoards = 24;          // the one heard longest ago makes room
  static constexpr uint32_t kHeardMs = 20000;    // heard straight within this: a neighbour
  static constexpr uint32_t kListMs = 16000;     // its links count this long (sent every 5-6 s)
  static constexpr uint8_t kGoodPct = 80;        // listed at this share or more: heard well enough

  struct Neighbour {
    uint32_t id = 0;
    uint32_t boot = 0, first = 0, top = 0;  // its sequence numbers this boot heard straight
    uint64_t mask = 0;     // bit n: seq top - n came straight from it
    uint32_t heardMs = 0;  // its last packet straight from it; 0 never
    int rssi = 0;
    uint32_t listMs = 0;   // its links came then; 0 never
    uint8_t flags = 0;     // kLinks*, from its links
    uint8_t count = 0;
    LinkHeard heard[kMaxLinks];
    // Since its boot began here: its packets that came straight, those that didn't (gaps in its
    // sequence numbers), and the gaps by length: 1, 2-3, 4-15, 16 and more (for dump()).
    uint32_t got = 0, missed = 0;
    uint32_t runs[4] = {};
    // Of its last 64 sequence numbers (fewer while fewer have gone), the share that came straight
    // from it: 0 to 100.
    uint8_t pct() const;
  };

  // A packet straight from a board (hops 0), any group's or the link group's. now is never 0.
  void heard(uint32_t id, uint32_t boot, uint32_t seq, int rssi, uint32_t now);
  // A board's links came (straight from it: they're never relayed).
  void links(uint32_t id, const Links &l, uint32_t now);
  // This board's links: the boards heard straight within kHeardMs, with their shares (flags are
  // the caller's).
  void mine(Links &out, uint32_t now) const;
  // The rule, for a voice or message frame from `origin`, as this board got it from `from` (the
  // board whose radio sent that copy: origin itself at hops 0; 0 if unknown): true if a neighbour
  // (heard within kHeardMs; not origin, not from) may miss it: its links are missing or older than
  // kListMs, or list neither from nor origin at kGoodPct or more. forWhom gets the first such.
  bool needed(uint32_t origin, uint32_t from, uint32_t now, uint32_t *forWhom = nullptr) const;
  void clear();

  size_t size() const { return n_; }
  const Neighbour &at(size_t i) const { return e_[i]; }

 private:
  Neighbour *find(uint32_t id);
  const Neighbour *find(uint32_t id) const;
  static bool hears(const Neighbour &x, uint32_t id);
  Neighbour e_[kBoards];
  size_t n_ = 0;
};

}  // namespace wire
}  // namespace comms
