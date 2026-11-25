#include "comms_calls.h"

#include "features.h"

#if INKDECK_COMMS  // the whole file (features.h)

#include <string.h>

#include <algorithm>

namespace comms {
namespace wire {

// --- ConvState -------------------------------------------------------------------------------

bool ConvState::isMember(uint32_t id) const {
  for (size_t i = 0; i < count_; ++i) {
    if (members_[i].id == id) return true;
  }
  return false;
}

uint32_t ConvState::adopt(uint32_t now, uint32_t conv, bool &changed) {
  if (!conv) return id_;
  if (!id_) {
    id_ = conv;
    startMs_ = lastWordMs_ = now;
    mine_ = false;
    company_ = false;
    count_ = 0;
    changed = true;
  } else if (conv < id_) {  // two met: the lower id wins, members and all
    if (leftId_ == id_) leftId_ = conv;  // left the one, so left the other
    id_ = conv;
    changed = true;
  }
  return id_;
}

bool ConvState::recentlyOut(uint32_t now, uint32_t id) const {
  for (const Out &o : out_) {
    if (o.id == id && now - o.ms < kMemberMs) return true;
  }
  return false;
}

bool ConvState::addMember(uint32_t now, uint32_t id, bool listed) {
  if (!id) return false;
  for (size_t i = 0; i < count_; ++i) {
    Member &m = members_[i];
    if (m.id != id) continue;
    m.seenMs = now;
    if (!listed) m.listed = false;
    return false;  // a refresh: nothing shown changes
  }
  if (listed && recentlyOut(now, id)) return false;  // it said itself it's out
  if (!listed) {
    for (Out &o : out_) {
      if (o.id == id) o = Out();
    }
  }
  if (count_ >= kMaxMembers) {  // the one heard from longest ago makes room
    size_t oldest = 0;
    for (size_t i = 1; i < count_; ++i) {
      if (now - members_[i].seenMs > now - members_[oldest].seenMs) oldest = i;
    }
    members_[oldest] = members_[--count_];
  }
  members_[count_++] = Member{id, now, listed};
  company_ = true;
  return true;
}

bool ConvState::removeMember(uint32_t id) {
  for (size_t i = 0; i < count_; ++i) {
    if (members_[i].id == id) {
      members_[i] = members_[--count_];
      return true;
    }
  }
  return false;
}

bool ConvState::revive(uint32_t now, uint32_t conv) {
  if (id_ || !conv || conv != ended_.id || now - ended_.ms >= kMemberMs) return false;
  id_ = conv;
  startMs_ = ended_.startMs;
  lastWordMs_ = ended_.lastWordMs;
  mine_ = ended_.mine;
  leftId_ = ended_.left ? conv : 0;
  company_ = false;
  count_ = 0;
  return true;
}

bool ConvState::report(uint32_t now, uint32_t from, uint32_t conv, uint32_t ageS, uint32_t quietS,
                       const uint32_t *listed, size_t n, uint32_t self) {
  if (!conv || !from) return false;
  // When it began and the last word, as the best-informed board knows them.
  const uint32_t began = now - std::min<uint32_t>(ageS, 24 * 3600) * 1000u;
  const uint32_t said = now - std::min<uint32_t>(quietS, 24 * 3600) * 1000u;
  if (!id_ && conv == ended_.id && now - ended_.ms < kMemberMs) {
    // Ended here a moment ago: back only if the report knows a word this board missed (its
    // quiet is in whole seconds, and boards note words a few frames apart: 2 s of slack).
    if (static_cast<int32_t>(said - ended_.lastWordMs) <= 2000) return false;
    revive(now, conv);
  }
  bool changed = false;
  const bool fresh = !id_;
  adopt(now, conv, changed);
  if (fresh) lastWordMs_ = said;  // not now: the report says when
  if (fresh || static_cast<int32_t>(began - startMs_) < 0) {
    if (startMs_ / 1000 != began / 1000) changed = true;
    startMs_ = began;
  }
  if (static_cast<int32_t>(said - lastWordMs_) > 0) lastWordMs_ = said;
  changed |= addMember(now, from, false);
  for (size_t i = 0; i < n; ++i) {
    if (listed[i] != self && listed[i] != from) changed |= addMember(now, listed[i], true);
  }
  return changed;
}

bool ConvState::reportNone(uint32_t now, uint32_t from) {
  out_[outNext_] = Out{from, now};
  outNext_ = (outNext_ + 1) % (sizeof out_ / sizeof out_[0]);
  return removeMember(from);
}

bool ConvState::spoke(uint32_t now, uint32_t conv, uint32_t from, uint32_t self) {
  if (!conv) return false;
  bool changed = revive(now, conv);  // a word in one that just ended here: it's still on
  adopt(now, conv, changed);
  lastWordMs_ = now;
  if (from != self) changed |= addMember(now, from, false);
  return changed;
}

bool ConvState::join(uint32_t now, uint32_t conv) {
  if (!conv && !id_) return false;
  bool changed = false;
  adopt(now, conv, changed);
  if (!mine_) {
    mine_ = true;
    changed = true;
  }
  leftId_ = 0;
  return changed;
}

bool ConvState::leave(uint32_t now) {
  if (!id_ || !mine_) return false;
  mine_ = false;
  leftId_ = id_;
  expire(now);
  return true;
}

bool ConvState::expire(uint32_t now, bool talking) {
  bool changed = false;
  for (size_t i = 0; i < count_;) {
    if (now - members_[i].seenMs >= kMemberMs) {
      members_[i] = members_[--count_];
      changed = true;
    } else {
      ++i;
    }
  }
  const bool alone = count_ == 0 && (!mine_ || (company_ && !talking));
  if (id_ && (now - lastWordMs_ >= kQuietMs || alone)) {
    ended_ = Ended{id_, now, startMs_, lastWordMs_, mine_, leftId_ == id_};
    id_ = 0;
    mine_ = false;
    count_ = 0;
    changed = true;
  }
  return changed;
}

// --- Messages --------------------------------------------------------------------------------

uint32_t messageSum(const uint8_t *stored, size_t frames, uint32_t sum) {
  const size_t n = frames * kStoredFrame;
  for (size_t i = 0; i < n; ++i) {
    sum ^= stored[i];
    sum *= 16777619u;
  }
  return sum;
}

bool MessageAssembly::add(uint16_t frame, int16_t predictor, uint8_t index, const uint8_t *data,
                          uint8_t bytes) {
  if (frame >= kMaxFrames || bytes > kFrameBytes || index > 88) return false;
  if (ended_ && frame >= total_) return false;
  if (got(frame)) return false;
  if (frame >= top_) {
    top_ = static_cast<uint16_t>(frame + 1);
    audio_.resize(static_cast<size_t>(top_) * kStoredFrame, 0);
    bits_.resize((top_ + 7) / 8, 0);
  }
  uint8_t *s = &audio_[static_cast<size_t>(frame) * kStoredFrame];
  s[0] = static_cast<uint8_t>(predictor);
  s[1] = static_cast<uint8_t>(static_cast<uint16_t>(predictor) >> 8);
  s[2] = index;
  s[3] = bytes;
  memcpy(s + 4, data, bytes);
  memset(s + 4 + bytes, 0, kFrameBytes - bytes);
  bits_[frame / 8] |= static_cast<uint8_t>(1u << (frame % 8));
  ++have_;
  return true;
}

void MessageAssembly::end(uint16_t frames, uint32_t sum) {
  if (frames > kMaxFrames) frames = kMaxFrames;
  if (ended_) return;
  ended_ = true;
  total_ = frames;
  sum_ = sum;
  if (top_ > total_) {  // frames past the end (it can't happen from one sender): dropped
    for (uint16_t f = total_; f < top_; ++f) {
      if (got(f)) --have_;
    }
    top_ = total_;
    audio_.resize(static_cast<size_t>(top_) * kStoredFrame);
  }
  if (top_ < total_) {
    top_ = total_;
    audio_.resize(static_cast<size_t>(top_) * kStoredFrame, 0);
    bits_.resize((top_ + 7) / 8, 0);
  }
}

bool MessageAssembly::sumOk() const {
  return allIn() && messageSum(audio_.data(), total_) == sum_;
}

size_t MessageAssembly::missing(uint16_t *starts, uint16_t *counts, size_t max) const {
  size_t n = 0;
  uint16_t f = 0;
  while (f < top_ && n < max) {
    if (got(f)) {
      ++f;
      continue;
    }
    const uint16_t s = f;
    while (f < top_ && !got(f)) ++f;
    starts[n] = s;
    counts[n] = static_cast<uint16_t>(f - s);
    ++n;
  }
  return n;
}

size_t MessageAssembly::missingMap(uint16_t &from, uint8_t *bits, size_t maxFrames,
                                   size_t &count) const {
  count = 0;
  uint16_t f = 0;
  while (f < top_ && got(f)) ++f;
  from = f;
  if (f >= top_) return 0;
  const size_t span = std::min<size_t>(maxFrames, top_ - f);
  memset(bits, 0, (span + 7) / 8);
  for (size_t i = 0; i < span; ++i) {
    if (!got(static_cast<uint16_t>(f + i))) {
      bits[i / 8] |= static_cast<uint8_t>(1u << (i % 8));
      ++count;
    }
  }
  return span;
}

}  // namespace wire
}  // namespace comms

#endif  // INKDECK_COMMS
