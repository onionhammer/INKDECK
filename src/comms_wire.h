// The comms packet format (docs/comms-protocol.md has it byte by byte), and the two tables that
// decide whether a packet is new: the seen-cache, which the mesh relays by, and the replay
// window, which the board trusts by. Conversations and message transfers are in comms_calls.h,
// the baby monitor (type 15's packets) in comms_monitor.h.
//
// Plain C++ with no Arduino or ESP-IDF calls, so the self-test (comms::selfTest()) checks it on
// the board and it builds on a computer too. Not thread-safe: comms.cpp's task owns the tables.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace comms {
namespace wire {

constexpr uint8_t kMagic = 0xC7;  // the first byte of every INKDECK comms frame
constexpr uint8_t kVersion = 1;
constexpr size_t kHeader = 20;
constexpr size_t kTag = 16;        // AES-GCM's tag, after the body
constexpr size_t kMaxFrame = 250;  // ESP-NOW v1's limit, which v2 boards also take
constexpr size_t kMaxBody = kMaxFrame - kHeader - kTag;
constexpr size_t kNonce = 12;
constexpr uint8_t kTtl = 3;  // relays a packet may pass through (so 4 hops over the air)

enum Type : uint8_t {
  kPresence = 1,    // "here": name, flags, battery; a hello asks the others to answer; the
                    // conversation the board is in
  kVoiceStart = 2,  // a call begins (the voice packets carry the same, so it may be lost)
  kVoice = 3,       // 20 ms of IMA ADPCM, self-contained
  kVoiceEnd = 4,    // a call ended, with how many frames it had
  kPing = 5,        // !commsping
  kPong = 6,        // the answer, to the pinger
  kHeard = 7,       // a call played here: the caller counts who heard it
  kRing = 8,        // a dialer rings all of a group or one board of it, about once a second
  kRinging = 9,     // a board rings for it (or has stopped: ignored): the dialer counts them
  kAnswer = 10,     // a ring answered: it floods, so every board it rang stops
  kRingEnd = 11,    // the dialer gave up, rang 60 s with no answer, or leaves a message
  kMessage = 12,    // a numbered 20 ms frame of a message (not live: it's sent to be complete)
  kMessageEnd = 13, // a message's frame count and checksum, or that it was dropped
  kMessageAsk = 14, // a board asks for the frames of a message it missed, or has them all
  kExt = 15,        // the last 4-bit type: the body's byte after sentAt says which (the baby
                    // monitor's packets, comms_monitor.h; later features add theirs there)
};
constexpr uint8_t kLastType = kExt;  // the types this firmware handles: all 15 there can be
// A type this firmware handles. An ext type it doesn't (a later version's) still opens and is
// relayed, as a later type was before type 15 took the last number.
inline bool knownType(uint8_t t) { return t >= kPresence && t <= kLastType; }

// The cleartext header. Everything but ttl and hops is authenticated (it's the AES-GCM
// associated data), so a relay can count down ttl and count up hops without the key.
struct Header {
  uint8_t type = 0;
  uint8_t ttl = 0;   // relays left, 0 to 15
  uint8_t hops = 0;  // relays passed, 0 to 15
  uint32_t group = 0;
  uint32_t sender = 0;
  uint32_t boot = 0;  // the sender's boot number: with seq, the nonce
  uint32_t seq = 0;   // the sender's packet counter this boot
};

void put16(uint8_t *p, uint16_t v);
void put32(uint8_t *p, uint32_t v);
uint16_t get16(const uint8_t *p);
uint32_t get32(const uint8_t *p);

void putHeader(const Header &h, uint8_t *frame);
// False if it isn't a comms frame this firmware reads: too short or long, another magic or
// version, type 0. A type it doesn't handle (knownType()) still parses, so it can be relayed.
bool parseHeader(const uint8_t *frame, size_t len, Header &out);
// The associated data: the header as sent, with the ttl/hops byte zeroed.
void associatedData(const uint8_t *frame, uint8_t out[kHeader]);
// sender, boot, seq: 12 bytes, little-endian. Never repeats while a board keeps its id (comms.cpp
// makes boot a counter kept beside the id).
void nonce(uint32_t sender, uint32_t boot, uint32_t seq, uint8_t out[kNonce]);
// What a relay changes before sending a frame on. False if its ttl is already 0.
bool forward(uint8_t *frame);

// Writing and reading a body, byte by byte, little-endian. Reading past the end gives zeros and
// sets !ok(), so a short body can't be read beyond.
class Writer {
 public:
  Writer(uint8_t *buf, size_t cap) : p_(buf), cap_(cap) {}
  void u8(uint8_t v);
  void u16(uint16_t v);
  void u32(uint32_t v);
  void bytes(const void *v, size_t n);
  size_t size() const { return n_; }
  bool ok() const { return ok_; }

 private:
  uint8_t *p_;
  size_t cap_, n_ = 0;
  bool ok_ = true;
};

class Reader {
 public:
  Reader(const uint8_t *buf, size_t len) : p_(buf), len_(len) {}
  uint8_t u8();
  uint16_t u16();
  uint32_t u32();
  const uint8_t *bytes(size_t n);  // nullptr past the end
  size_t left() const { return len_ - n_; }
  bool ok() const { return ok_; }

 private:
  const uint8_t *p_;
  size_t len_, n_ = 0;
  bool ok_ = true;
};

// Packets already taken, by (group, sender, boot, seq): the last kSize of them. The mesh floods,
// so each packet arrives once from every relay in range; only the first is handled and passed
// on. A packet leaves the cache kSize packets later, seconds after its copies have come.
class SeenCache {
 public:
  static constexpr size_t kSize = 256;
  bool seen(uint32_t group, uint32_t sender, uint32_t boot, uint32_t seq) const;
  void add(uint32_t group, uint32_t sender, uint32_t boot, uint32_t seq);
  void clear();

 private:
  struct Key {
    uint32_t group, sender, boot, seq;
  };
  Key keys_[kSize] = {};
  size_t next_ = 0, count_ = 0;
};

// The replay window: for each sender, the boot it's on and which of the last 64 sequence
// numbers have been taken. A packet that opened (so it was sealed with the group's key) is still
// dropped if it's a copy of one taken before, or older than the window, or from an earlier boot.
// The first packet from a sender, or from a new boot of it, must be fresh: its sentAt within
// kFreshS of this board's clock, when both clocks are set.
class ReplayGuard {
 public:
  static constexpr size_t kSenders = 32;  // the least recently heard is forgotten
  static constexpr uint32_t kFreshS = 30 * 60;
  enum class Verdict : uint8_t { Ok, Duplicate, TooOld, OldBoot, Stale };
  // fresh: the packet's sentAt is close enough to now, or one of the clocks isn't set. Takes the
  // packet into the window when it returns Ok.
  Verdict accept(uint32_t sender, uint32_t boot, uint32_t seq, bool fresh);
  void clear();

 private:
  struct Entry {
    uint32_t sender = 0, boot = 0, top = 0;
    uint64_t mask = 0;  // bit n: top - n was taken
    uint32_t used = 0;  // for forgetting the least recent
  };
  Entry entries_[kSenders];
  uint32_t tick_ = 0;
};

// IMA ADPCM voice: 20 ms a packet at 16 kHz, 2 samples a byte.
constexpr uint32_t kVoiceRate = 16000;
constexpr size_t kFrameSamples = 320;
constexpr size_t kFrameBytes = kFrameSamples / 2;
constexpr uint32_t kFrameMs = 20;
// A frame as boards keep it (PLAY LAST, messages): predictor (2, little-endian), step index,
// byte count, then the bytes, zero-filled to kFrameBytes. All zeros is a lost frame: silence.
constexpr size_t kStoredFrame = 4 + kFrameBytes;

}  // namespace wire
}  // namespace comms
