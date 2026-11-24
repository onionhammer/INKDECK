#include "comms_mic.h"

#include "features.h"

#if INKDECK_COMMS  // the whole file (features.h)

#include <math.h>
#include <stdlib.h>

#include <algorithm>

namespace comms {
namespace wire {

uint8_t levelByte(float db) {
  if (!(db > -96)) return 96;  // NaN and silence too
  if (db >= 0) return 0;
  return static_cast<uint8_t>(lroundf(-db));
}

float levelDb(uint8_t b) { return b == kNoLevel || b >= 96 ? -96 : -static_cast<float>(b); }

// --- MicHistory --------------------------------------------------------------------------------

void MicHistory::advance(uint16_t bar) {
  if (!any_) {
    memset(bars_, kNoLevel, sizeof bars_);
    any_ = true;
    top_ = bar;
    return;
  }
  if (bar <= top_) return;
  if (bar - top_ >= kBars) {
    memset(bars_, kNoLevel, sizeof bars_);
  } else {
    for (uint32_t b = top_ + 1u; b <= bar; ++b) bars_[b % kBars] = kNoLevel;
  }
  top_ = bar;
}

void MicHistory::note(uint32_t ms, float levelDb) {
  const uint16_t bar = static_cast<uint16_t>(std::min<uint32_t>(ms / kBarMs, 0xFFFF));
  advance(bar);
  if (bar + kBars <= top_) return;
  uint8_t &x = bars_[bar % kBars];
  const uint8_t v = levelByte(levelDb);
  if (x == kNoLevel || v < x) x = v;  // fewer dB below full scale: louder
}

void MicHistory::set(uint16_t bar, uint8_t level) {
  advance(bar);
  if (bar + kBars <= top_) return;
  bars_[bar % kBars] = level;
}

uint8_t MicHistory::at(uint16_t bar) const {
  if (!any_ || bar > top_ || bar + kBars <= top_) return kNoLevel;
  return bars_[bar % kBars];
}

// --- HeardMic ----------------------------------------------------------------------------------

HeardMic::Change HeardMic::on(uint32_t now, uint16_t session, uint32_t onMs, bool latched) {
  Change c;
  heardMs_ = now;
  if (link_ == Link::Lost) {  // back: the outage is over
    if (!events_.empty() && events_.back().open) {
      events_.back().open = false;
      events_.back().endMs = now;
    }
    link_ = Link::On;
    sinceMs_ = now;
    acked_ = false;
    c.back = c.shown = true;
  }
  if (link_ != Link::Off && session != session_) {  // another session in its place
    c.ended = true;
    c.endedSession = session_;
    c.endedMs = now - startMs_;
    link_ = Link::Off;
  }
  if (link_ == Link::Off) {
    link_ = Link::On;
    session_ = session;
    startMs_ = now - onMs;
    sinceMs_ = now;
    voiceMs_ = 0;
    acked_ = false;
    latched_ = latched;
    c.began = c.shown = true;
  } else if (onMs && static_cast<int32_t>((now - onMs) - startMs_) < 0) {
    startMs_ = now - onMs;  // its voice came first (on for 0): its still-here says since when
  }
  if (latched && !latched_) {  // a press latched it: so it stays for the session
    latched_ = true;
    c.shown = true;
  }
  return c;
}

HeardMic::Change HeardMic::off(uint32_t now, uint16_t session) {
  Change c;
  if (link_ == Link::Off || (session && session != session_)) return c;
  if (!events_.empty() && events_.back().open) {
    events_.back().open = false;
    events_.back().endMs = now;
  }
  c.ended = c.shown = true;
  c.back = link_ == Link::Lost;
  c.endedSession = session_;
  c.endedMs = now - startMs_;
  link_ = Link::Off;
  sinceMs_ = now;
  acked_ = false;
  return c;
}

HeardMic::Change HeardMic::tick(uint32_t now, bool alarmable) {
  Change c;
  if (link_ != Link::On) return c;
  const uint32_t quiet = now - heardMs_;
  if (!latched_ && quiet >= kCutMs) {  // held, then nothing: cut off, no alarm
    c.ended = c.cut = c.shown = true;
    c.endedSession = session_;
    c.endedMs = heardMs_ - startMs_;
    link_ = Link::Off;
    sinceMs_ = heardMs_;
    return c;
  }
  if (latched_ && quiet >= kLostMs) {
    if (events_.size() >= kMaxEvents) events_.erase(events_.begin());
    Event e;
    e.startMs = e.endMs = heardMs_;
    e.open = true;
    events_.push_back(e);
    link_ = Link::Lost;
    sinceMs_ = heardMs_;  // LOST since the last word from it
    acked_ = !alarmable;
    c.lost = c.shown = true;
  }
  return c;
}

bool HeardMic::ack() {
  if (!alarm()) return false;
  acked_ = true;
  return true;
}

// --- Mixer -------------------------------------------------------------------------------------

Mixer::Took Mixer::add(uint32_t sender, uint16_t session, const Frame &f, uint32_t now) {
  Source *s = nullptr;
  for (Source &x : src_) {
    if (x.used && x.sender == sender) s = &x;
  }
  if (!s) {
    for (Source &x : src_) {
      if (!x.used) {
        s = &x;
        break;
      }
    }
    if (!s) return Took::NoRoom;
    *s = Source();
    s->used = true;
    s->sender = sender;
    s->session = session;
  }
  if (s->started && s->session != session) {  // another session: its buffer starts afresh
    noteEnd(*s);
    s->got = s->lost = s->came = s->top = 0;
    s->reported = false;
    for (Slot &x : s->slots) x.full = false;
    s->started = s->primed = s->played = false;
    s->dry = 0;
    s->conceal = Concealer();
  }
  s->session = session;
  s->lastMs = now;
  if (!s->started) {
    s->started = true;
    s->next = f.frame <= kJitterWait ? 0 : f.frame;
    s->from = s->next;
    s->firstMs = now;
  }
  int ahead = static_cast<int16_t>(f.frame - s->next);
  if (ahead < 0) return Took::Late;
  if (ahead > kResyncFrames) {  // a long gap: on from here
    for (Slot &x : s->slots) x.full = false;
    s->next = f.frame;
    ahead = 0;
  }
  if (ahead >= static_cast<int>(kSlots)) {  // too far ahead: its oldest go
    const int skip = ahead - static_cast<int>(kSlots) + 1;
    for (int i = 0; i < skip; ++i) s->slots[(s->next + i) % kSlots].full = false;
    s->next = static_cast<uint16_t>(s->next + skip);
  }
  Slot &slot = s->slots[f.frame % kSlots];
  if (slot.full && slot.f.frame == f.frame) return Took::Copy;
  ++s->came;
  if (static_cast<uint16_t>(f.frame + 1) > s->top) s->top = static_cast<uint16_t>(f.frame + 1);
  s->reported = false;
  slot.full = true;
  slot.atMs = now;
  slot.f = f;
  return Took::Ok;
}

int Mixer::pull(int16_t *out, uint32_t now) {
  played_ = 0;
  memset(sum_, 0, sizeof sum_);
  for (Source &s : src_) {
    if (!s.used) continue;
    int buffered = 0;
    uint32_t oldest = now;
    for (const Slot &x : s.slots) {
      if (!x.full) continue;
      ++buffered;
      if (static_cast<int32_t>(x.atMs - oldest) < 0) oldest = x.atMs;
    }
    if (!buffered && now - s.lastMs >= kIdleMs) {
      s.used = false;
      continue;
    }
    if (!s.primed) {
      if (buffered < kPrime && !(buffered && now - oldest >= kPrimeWaitMs)) continue;
      s.primed = true;
      s.dry = 0;
      // On from the first frame there is.
      int least = 0x7FFF;
      for (const Slot &x : s.slots) {
        if (x.full) least = std::min(least, static_cast<int>(static_cast<int16_t>(x.f.frame - s.next)));
      }
      if (least > 0 && least != 0x7FFF) s.next = static_cast<uint16_t>(s.next + least);
    }
    Played &p = playedAt_[played_];
    p = Played();
    p.sender = s.sender;
    p.session = s.session;
    Slot &x = s.slots[s.next % kSlots];
    if (x.full && x.f.frame == s.next) {
      if (x.f.bytes) {
        adpcm::State st;
        st.predictor = x.f.predictor;
        st.index = x.f.index;
        const uint8_t bytes = std::min<uint8_t>(x.f.bytes, kFrameBytes);
        adpcm::decode(st, x.f.data, bytes, pcm_);
        for (size_t i = bytes * 2u; i < kFrameSamples; ++i) pcm_[i] = 0;
      } else {
        memset(pcm_, 0, sizeof pcm_);  // an empty frame (none is sent): silence
      }
      s.conceal.apply(pcm_, kFrameSamples, false);
      x.full = false;
      s.dry = 0;
      ++s.got;
      s.reported = false;
    } else if (buffered) {  // a later frame is in: this one is lost
      memset(pcm_, 0, sizeof pcm_);
      s.conceal.apply(pcm_, kFrameSamples, true);
      p.lost = true;
      ++s.lost;
      s.reported = false;
    } else if (++s.dry > kDryFrames) {  // nothing more: it rests until frames come
      s.primed = false;
      noteEnd(s);
      continue;
    } else {  // late, or the session's end
      memset(pcm_, 0, sizeof pcm_);
      s.conceal.apply(pcm_, kFrameSamples, true);
    }
    s.next = static_cast<uint16_t>(s.next + 1);
    if (!s.played) {
      s.played = true;
      p.first = true;
      p.waitedMs = now - s.firstMs;
    }
    int peak = 0;
    for (size_t i = 0; i < kFrameSamples; ++i) {
      sum_[i] += pcm_[i];
      peak = std::max(peak, abs(static_cast<int>(pcm_[i])));
    }
    p.levelDb = peak > 0 ? 20 * log10f(peak / 32768.0f) : -96;
    ++played_;
  }
  int32_t peak = 0;
  for (size_t i = 0; i < kFrameSamples; ++i) peak = std::max(peak, abs(sum_[i]));
  if (peak > 32767) {  // two or three loud voices: held under full scale, this frame
    const float k = 32767.0f / static_cast<float>(peak);
    for (size_t i = 0; i < kFrameSamples; ++i) {
      out[i] = static_cast<int16_t>(lroundf(static_cast<float>(sum_[i]) * k));
    }
  } else {
    for (size_t i = 0; i < kFrameSamples; ++i) out[i] = static_cast<int16_t>(sum_[i]);
  }
  return static_cast<int>(played_);
}

bool Mixer::active() const {
  for (const Source &s : src_) {
    if (s.used) return true;
  }
  return false;
}

size_t Mixer::sources() const {
  size_t n = 0;
  for (const Source &s : src_) n += s.used;
  return n;
}

bool Mixer::has(uint32_t sender) const {
  for (const Source &s : src_) {
    if (s.used && s.sender == sender) return true;
  }
  return false;
}

void Mixer::noteEnd(Source &s) {
  if (s.reported || (!s.got && !s.lost)) return;
  ended_.sender = s.sender;
  ended_.session = s.session;
  ended_.from = s.from;
  ended_.came = s.came;
  ended_.top = s.top;
  ended_.played = s.got;
  ended_.lost = s.lost;
  endedNew_ = true;
  s.reported = true;
}

bool Mixer::ended(Ended &out) {
  if (!endedNew_) return false;
  endedNew_ = false;
  out = ended_;
  return true;
}

void Mixer::drop(uint32_t sender) {
  for (Source &s : src_) {
    if (s.used && s.sender == sender) s.used = false;
  }
}

void Mixer::clear() {
  for (Source &s : src_) s.used = false;
  played_ = 0;
}

// --- TalkLatch ---------------------------------------------------------------------------------

TalkLatch::Act TalkLatch::holdStart(bool micOn) {
  if (latched_) {  // a new hold takes the latched mic over: its release ends it
    latched_ = false;
    held_ = true;
    return Act::Unlatch;
  }
  if (micOn) return Act::None;  // on some other way: left as it is
  held_ = true;
  return Act::MicOn;
}

TalkLatch::Act TalkLatch::holdEnd() {
  if (!held_) return Act::None;
  held_ = false;
  return latched_ ? Act::None : Act::MicOff;
}

bool TalkLatch::press(bool select, bool ends, Act &act) {
  act = Act::None;
  if (held_ && select) {  // ● while BOOT is held: latch (a second ● then does nothing)
    if (!latched_) {
      latched_ = true;
      act = Act::Latch;
    }
    return true;
  }
  if (latched_ && !held_ && ends) {  // the next ● or BOOT: it ends, and does nothing else
    latched_ = false;
    act = Act::MicOff;
    return true;
  }
  return false;
}

// --- Packets -----------------------------------------------------------------------------------

size_t putMicStill(uint8_t *body, size_t cap, uint32_t sentAt, const MicStill &s) {
  Writer w(body, cap);
  w.u32(sentAt);
  w.u8(s.flags);
  w.u8(s.battery);
  w.u8(s.peak);
  w.u16(s.session);
  w.u32(s.onS);
  w.u16(s.bar);
  w.u8(s.barPeak);
  w.u32(s.target);
  w.u32(s.conv);
  return w.ok() ? w.size() : 0;
}

bool readMicStill(Reader &r, MicStill &s) {
  s.flags = r.u8();
  s.battery = r.u8();
  s.peak = r.u8();
  s.session = r.u16();
  s.onS = r.u32();
  s.bar = r.u16();
  s.barPeak = r.u8();
  s.target = r.u32();
  s.conv = r.u32();
  return r.ok();
}

size_t putVoice(uint8_t *body, size_t cap, uint32_t sentAt, const Voice &v) {
  Writer w(body, cap);
  w.u32(sentAt);
  w.u32(v.target);
  w.u32(v.conv);
  w.u16(v.session);
  w.u16(v.frame);
  w.u8(v.level);
  w.u8(v.flags);
  w.u16(static_cast<uint16_t>(v.predictor));
  w.u8(v.index);
  const uint8_t bytes = std::min<uint8_t>(v.bytes, kFrameBytes);
  w.u8(bytes);
  w.bytes(v.data, bytes);
  return w.ok() ? w.size() : 0;
}

bool readVoice(Reader &r, Voice &v) {
  v.target = r.u32();
  v.conv = r.u32();
  v.session = r.u16();
  v.frame = r.u16();
  v.level = r.u8();
  v.flags = r.u8();
  v.predictor = static_cast<int16_t>(r.u16());
  v.index = r.u8();
  v.bytes = r.u8();
  if (!r.ok() || v.bytes > kFrameBytes || v.index > 88) return false;
  const uint8_t *d = r.bytes(v.bytes);
  if (!d) return false;
  memcpy(v.data, d, v.bytes);
  return true;
}

size_t putMicOff(uint8_t *body, size_t cap, uint32_t sentAt, const MicOff &o) {
  Writer w(body, cap);
  w.u32(sentAt);
  w.u16(o.session);
  w.u8(o.reason);
  w.u32(o.onS);
  return w.ok() ? w.size() : 0;
}

bool readMicOff(Reader &r, MicOff &o) {
  o.session = r.u16();
  o.reason = r.u8();
  o.onS = r.u32();
  return r.ok();
}

size_t putHistoryAsk(uint8_t *body, size_t cap, uint32_t sentAt, uint32_t board) {
  Writer w(body, cap);
  w.u32(sentAt);
  w.u8(kHistoryAsk);
  w.u32(board);
  return w.ok() ? w.size() : 0;
}

size_t putHistory(uint8_t *body, size_t cap, uint32_t sentAt, const History &h) {
  Writer w(body, cap);
  w.u32(sentAt);
  w.u8(kHistory);
  w.u32(h.to);
  w.u16(h.session);
  w.u16(h.first);
  const uint8_t n = std::min<uint8_t>(h.count, MicHistory::kBars);
  w.u8(n);
  w.bytes(h.bars, n);
  return w.ok() ? w.size() : 0;
}

bool readHistory(Reader &r, History &h) {
  h.to = r.u32();
  h.session = r.u16();
  h.first = r.u16();
  h.count = r.u8();
  if (!r.ok() || h.count > MicHistory::kBars) return false;
  const uint8_t *b = r.bytes(h.count);
  if (!b) return false;
  memcpy(h.bars, b, h.count);
  return true;
}

uint32_t extFor(const uint8_t *body, size_t len) {
  if (len < 5 + 4) return 0;
  switch (body[4]) {
    case kHistoryAsk:
    case kHistory: return get32(body + 5);
    default: return 0;
  }
}

}  // namespace wire
}  // namespace comms

#endif  // INKDECK_COMMS
