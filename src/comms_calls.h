// Comms (issue #66): the state behind conversations and messages, as plain C++ with no Arduino or
// ESP-IDF calls, so the self-test (comms::selfTest()) drives it with a made-up clock and lost or
// doubled frames. docs/comms-protocol.md says what goes on the air; comms.cpp's task owns these
// and feeds them what comes in. Times are millis() of the board that keeps them.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include <vector>

#include "comms_wire.h"

namespace comms {
namespace wire {

// One group's conversation as this board knows it. A conversation is on from an answered ring or
// the first voice until 60 s after the last word, or until nobody is in it. Its boards say so in
// their presence every few seconds (with when it began, the last word and who else is in it), so
// a board that isn't in it can still tell it's on. A group has one at a time: if two meet (two
// boards began one each at the same moment), both become the one with the lower id.
class ConvState {
 public:
  static constexpr uint32_t kQuietMs = 60000;   // over this long after the last word
  static constexpr uint32_t kMemberMs = 13000;  // a member that hasn't said so for this long is out
  static constexpr size_t kMaxMembers = 8;      // besides this board

  struct Member {
    uint32_t id = 0;
    uint32_t seenMs = 0;  // last said it's in (or was listed by another member)
    bool listed = false;  // only another member has said so, not it itself yet
  };

  uint32_t id() const { return id_; }
  bool on() const { return id_ != 0; }
  bool mine() const { return mine_; }  // this board is in it
  bool left() const { return id_ != 0 && leftId_ == id_; }  // this board left it
  uint32_t startMs() const { return startMs_; }
  uint32_t lastWordMs() const { return lastWordMs_; }
  size_t count() const { return count_; }  // members besides this board
  const Member &member(size_t i) const { return members_[i]; }
  bool isMember(uint32_t id) const;

  // Each of these returns true if anything a screen shows has changed.

  // A board's presence says it's in conv, which began ageS ago and had its last word quietS ago
  // as it knows, with `listed` the other members it knows (self, this board, is skipped).
  bool report(uint32_t now, uint32_t from, uint32_t conv, uint32_t ageS, uint32_t quietS,
              const uint32_t *listed, size_t n, uint32_t self);
  // A board's presence says it's in no conversation in this group: it left, or it ended there.
  bool reportNone(uint32_t now, uint32_t from);
  // Voice in conv from `from` (self for this board's own): a word, and the talker is in it. A
  // conversation not on yet begins.
  bool spoke(uint32_t now, uint32_t conv, uint32_t from, uint32_t self);
  // This board is in conv now (it talked, answered, joined, or played the voice). conv 0: the one
  // that is on (false if none is). A conversation not on yet begins.
  bool join(uint32_t now, uint32_t conv);
  // This board leaves: it's out until join(). The conversation ends if nobody else is in it.
  bool leave(uint32_t now);
  // Members that stopped saying so go; the conversation ends kQuietMs after the last word, or
  // when nobody is in it.
  bool expire(uint32_t now);
  void clear() { *this = ConvState(); }

 private:
  // conv is the group's conversation from now on, or loses to the one on (the lower id wins).
  // Returns the id that stays.
  uint32_t adopt(uint32_t now, uint32_t conv, bool &changed);
  bool addMember(uint32_t now, uint32_t id, bool listed);
  bool removeMember(uint32_t id);
  bool recentlyOut(uint32_t now, uint32_t id) const;

  // A conversation that has ended here may still be on, a moment longer, for a board whose
  // clock runs a little behind: its report of it is old news, but a word in it brings it back.
  bool revive(uint32_t now, uint32_t conv);

  uint32_t id_ = 0;
  uint32_t startMs_ = 0, lastWordMs_ = 0;
  bool mine_ = false;
  uint32_t leftId_ = 0;  // the conversation this board left
  struct Ended {
    uint32_t id = 0, ms = 0, startMs = 0, lastWordMs = 0;
    bool mine = false, left = false;
  } ended_;  // the last one that ended here
  Member members_[kMaxMembers];
  size_t count_ = 0;
  struct Out {
    uint32_t id = 0, ms = 0;
  } out_[4];  // boards that said they're out: another member's list doesn't bring them back
  size_t outNext_ = 0;
};

// FNV-1a (32-bit) over a message's stored frames: its checksum in the end packet.
uint32_t messageSum(const uint8_t *stored, size_t frames, uint32_t sum = 2166136261u);

// A message as it comes in: frames numbered from 0, each taken once in any order, until the end
// packet has said how many there are and every one is there with the checksum matching.
class MessageAssembly {
 public:
  static constexpr size_t kMaxFrames = 3000;  // 60 s of 20 ms frames

  // A frame. False if it's a copy of one already in, past kMaxFrames or past the end, or bad.
  bool add(uint16_t frame, int16_t predictor, uint8_t index, const uint8_t *data, uint8_t bytes);
  // The end packet: how many frames there are, and their checksum.
  void end(uint16_t frames, uint32_t sum);
  bool ended() const { return ended_; }
  uint16_t total() const { return total_; }  // 0 until ended
  uint16_t have() const { return have_; }
  uint16_t top() const { return top_; }  // frames 0 to top - 1 are the span seen so far
  bool allIn() const { return ended_ && have_ == total_; }
  bool sumOk() const;  // every frame in, and the checksum is the end packet's
  // The frames still missing, as up to max ranges (start, count) from the lowest. Before the end
  // is known, only gaps below the highest frame in.
  size_t missing(uint16_t *starts, uint16_t *counts, size_t max) const;
  // The same as a map: from the lowest frame missing (from), up to maxFrames frames, a bit each
  // (set: missing; the lowest bit of the first byte is `from`). Returns the frames the map covers
  // (0: none missing); `count` gets how many of them are missing.
  size_t missingMap(uint16_t &from, uint8_t *bits, size_t maxFrames, size_t &count) const;
  // The audio, kStoredFrame a frame, frames 0 to total - 1 (top - 1 before the end); a missing
  // one is all zeros, which plays as silence.
  const std::vector<uint8_t> &audio() const { return audio_; }
  std::vector<uint8_t> &audio() { return audio_; }
  void clear() { *this = MessageAssembly(); }

 private:
  bool got(uint16_t f) const { return f < top_ && (bits_[f / 8] >> (f % 8)) & 1; }
  std::vector<uint8_t> audio_;
  std::vector<uint8_t> bits_;
  uint16_t top_ = 0, have_ = 0, total_ = 0;
  uint32_t sum_ = 0;
  bool ended_ = false;
};

}  // namespace wire
}  // namespace comms
