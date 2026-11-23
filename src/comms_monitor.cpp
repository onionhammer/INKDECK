#include "comms_monitor.h"

#include "features.h"

#if INKDECK_COMMS  // the whole file (features.h)

#include <math.h>
#include <string.h>

#include <algorithm>

namespace comms {
namespace wire {

uint8_t levelByte(float db) {
  if (!(db > -96)) return 96;  // NaN and silence too
  if (db >= 0) return 0;
  return static_cast<uint8_t>(lroundf(-db));
}

float levelDb(uint8_t b) { return b == kNoLevel || b >= 96 ? -96 : -static_cast<float>(b); }

bool ListenerSet::allows(uint32_t id) const {
  if (n == 0) return true;
  for (size_t i = 0; i < n && i < kMax; ++i) {
    if (ids[i] == id) return true;
  }
  return false;
}

// --- MonitorGate -------------------------------------------------------------------------------

MonitorGate::Step MonitorGate::frame(float levelDb) {
  const uint32_t i = n_++;
  if (levelDb >= threshold_) {
    loud_ = true;
    lastLoud_ = i;
  }
  Step s;
  // Frame i - kPreFrames goes out if a loud frame came within kPreFrames after it (so up to i)
  // or kHangFrames before it.
  s.send = loud_ && i >= kPreFrames && i - lastLoud_ <= kPreFrames + kHangFrames;
  s.first = s.send && !sending_;
  sending_ = s.send;
  return s;
}

void MonitorGate::reset() {
  n_ = lastLoud_ = 0;
  loud_ = sending_ = false;
}

// --- MonitorHistory ----------------------------------------------------------------------------

void MonitorHistory::advance(uint16_t bar) {
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

void MonitorHistory::note(uint32_t ms, float levelDb) {
  const uint16_t bar = static_cast<uint16_t>(std::min<uint32_t>(ms / kBarMs, 0xFFFF));
  advance(bar);
  if (bar + kBars <= top_) return;
  uint8_t &x = bars_[bar % kBars];
  const uint8_t v = levelByte(levelDb);
  if (x == kNoLevel || v < x) x = v;  // fewer dB below full scale: louder
}

void MonitorHistory::set(uint16_t bar, uint8_t level) {
  advance(bar);
  if (bar + kBars <= top_) return;
  bars_[bar % kBars] = level;
}

uint8_t MonitorHistory::at(uint16_t bar) const {
  if (!any_ || bar > top_ || bar + kBars <= top_) return kNoLevel;
  return bars_[bar % kBars];
}

// --- ListenState -------------------------------------------------------------------------------

void ListenState::push(const Event &e) {
  if (events_.size() >= kMaxEvents) events_.erase(events_.begin());
  events_.push_back(e);
}

ListenState::Event *ListenState::openEvent(bool noLink) {
  if (events_.empty()) return nullptr;
  Event &e = events_.back();
  return e.open && e.noLink == noLink ? &e : nullptr;
}

const ListenState::Event *ListenState::lastSound() const {
  for (auto it = events_.rbegin(); it != events_.rend(); ++it) {
    if (!it->noLink) return &*it;
  }
  return nullptr;
}

void ListenState::start(uint32_t now, uint32_t heardMs) {
  const int threshold = threshold_;
  *this = ListenState();
  threshold_ = threshold;
  link_ = Link::Quiet;
  sinceMs_ = now;
  heardMs_ = heardMs;
}

bool ListenState::heard(uint32_t now) {
  if (!on()) return false;
  heardMs_ = now;
  if (link_ != Link::NoLink) return false;
  if (Event *e = openEvent(true)) {
    e->open = false;
    e->endMs = now;
  }
  link_ = Link::Quiet;
  sinceMs_ = now;
  acked_ = false;
  return true;
}

bool ListenState::audio(uint32_t now, uint16_t sound, float levelDb, bool &becameLoud) {
  becameLoud = false;
  if (!on()) return false;
  bool changed = heard(now);
  // The same sound goes on, even after a gap the air left; another number is another sound.
  Event *e = events_.empty() || events_.back().noLink ? nullptr : &events_.back();
  if (!e || e->sound != sound) {
    if (e) e->open = false;
    Event n;
    n.startMs = n.endMs = now;
    n.open = true;
    n.sound = sound;
    push(n);
    e = &events_.back();
    changed = true;
  }
  if (!e->open) {
    e->open = true;
    changed = true;
  }
  e->endMs = now;
  e->peakDb = std::max(e->peakDb, levelDb);
  if (!e->loud && levelDb >= threshold_ + kLoudMarginDb) {
    e->loud = becameLoud = true;
    changed = true;
  }
  soundMs_ = now;
  if (link_ != Link::Sound) {
    link_ = Link::Sound;
    sinceMs_ = now;
    changed = true;
  }
  return changed;
}

bool ListenState::off(uint32_t now) {
  if (!on()) return false;
  for (Event &e : events_) {
    if (!e.open) continue;
    e.open = false;
    if (e.noLink) e.endMs = now;
  }
  link_ = Link::Stopped;
  sinceMs_ = now;
  acked_ = false;
  return true;
}

bool ListenState::tick(uint32_t now) {
  if (!on()) return false;
  bool changed = false;
  if (link_ == Link::Sound && now - soundMs_ >= kSoundEndMs) {
    if (Event *e = openEvent(false)) e->open = false;
    link_ = Link::Quiet;
    sinceMs_ = soundMs_;
    changed = true;
  }
  if (link_ != Link::NoLink && now - heardMs_ >= kNoLinkMs) {
    if (Event *e = openEvent(false)) e->open = false;
    Event n;
    n.noLink = true;
    n.startMs = n.endMs = heardMs_;
    n.open = true;
    push(n);
    link_ = Link::NoLink;
    sinceMs_ = heardMs_;  // NO LINK since the last word from the room
    acked_ = false;
    changed = true;
  }
  return changed;
}

bool ListenState::ack() {
  if (!alarm()) return false;
  acked_ = true;
  return true;
}

// --- Packets -----------------------------------------------------------------------------------

namespace {

void putListeners(Writer &w, const ListenerSet &l) {
  const uint8_t n = std::min<uint8_t>(l.n, ListenerSet::kMax);
  w.u8(n);
  for (uint8_t i = 0; i < n; ++i) w.u32(l.ids[i]);
}

bool readListeners(Reader &r, ListenerSet &l) {
  l.n = r.u8();
  if (l.n > ListenerSet::kMax) return false;
  for (uint8_t i = 0; i < l.n; ++i) l.ids[i] = r.u32();
  return r.ok();
}

}  // namespace

size_t putMonitorAudio(uint8_t *body, size_t cap, uint32_t sentAt, const MonitorAudio &a) {
  Writer w(body, cap);
  w.u32(sentAt);
  w.u8(kMonitorAudio);
  putListeners(w, a.listeners);
  w.u16(a.sound);
  w.u16(a.frame);
  w.u8(a.level);
  w.u16(static_cast<uint16_t>(a.predictor));
  w.u8(a.index);
  const uint8_t bytes = std::min<uint8_t>(a.bytes, kFrameBytes);
  w.u8(bytes);
  w.bytes(a.data, bytes);
  return w.ok() ? w.size() : 0;
}

bool readMonitorAudio(Reader &r, MonitorAudio &a) {
  if (!readListeners(r, a.listeners)) return false;
  a.sound = r.u16();
  a.frame = r.u16();
  a.level = r.u8();
  a.predictor = static_cast<int16_t>(r.u16());
  a.index = r.u8();
  a.bytes = r.u8();
  if (!r.ok() || a.bytes > kFrameBytes || a.index > 88) return false;
  const uint8_t *d = r.bytes(a.bytes);
  if (!d) return false;
  memcpy(a.data, d, a.bytes);
  return true;
}

size_t putStillHere(uint8_t *body, size_t cap, uint32_t sentAt, const StillHere &s) {
  Writer w(body, cap);
  w.u32(sentAt);
  w.u8(kStillHere);
  w.u8(s.flags);
  w.u8(s.battery);
  w.u8(s.threshold);
  w.u8(s.peak);
  w.u32(s.onS);
  w.u16(s.bar);
  w.u8(s.barPeak);
  w.u8(s.preFrames);
  putListeners(w, s.listeners);
  return w.ok() ? w.size() : 0;
}

bool readStillHere(Reader &r, StillHere &s) {
  s.flags = r.u8();
  s.battery = r.u8();
  s.threshold = r.u8();
  s.peak = r.u8();
  s.onS = r.u32();
  s.bar = r.u16();
  s.barPeak = r.u8();
  s.preFrames = r.u8();
  return r.ok() && readListeners(r, s.listeners);
}

size_t putMonitorOff(uint8_t *body, size_t cap, uint32_t sentAt, uint8_t reason) {
  Writer w(body, cap);
  w.u32(sentAt);
  w.u8(kMonitorOff);
  w.u8(reason);
  return w.ok() ? w.size() : 0;
}

size_t putToRoom(uint8_t *body, size_t cap, uint32_t sentAt, uint8_t ext, uint32_t room,
                 uint8_t flags) {
  Writer w(body, cap);
  w.u32(sentAt);
  w.u8(ext);
  w.u32(room);
  if (ext == kListening) w.u8(flags);
  return w.ok() ? w.size() : 0;
}

size_t putHistory(uint8_t *body, size_t cap, uint32_t sentAt, const History &h) {
  Writer w(body, cap);
  w.u32(sentAt);
  w.u8(kHistory);
  w.u32(h.to);
  w.u16(h.first);
  const uint8_t n = std::min<uint8_t>(h.count, MonitorHistory::kBars);
  w.u8(n);
  w.bytes(h.bars, n);
  return w.ok() ? w.size() : 0;
}

bool readHistory(Reader &r, History &h) {
  h.to = r.u32();
  h.first = r.u16();
  h.count = r.u8();
  if (!r.ok() || h.count > MonitorHistory::kBars) return false;
  const uint8_t *b = r.bytes(h.count);
  if (!b) return false;
  memcpy(h.bars, b, h.count);
  return true;
}

uint32_t extFor(const uint8_t *body, size_t len) {
  if (len < 5) return 0;
  const uint8_t *p = body + 5;
  const size_t left = len - 5;
  switch (body[4]) {
    case kListening:
    case kHistoryAsk:
    case kHistory: return left >= 4 ? get32(p) : 0;
    case kMonitorAudio: return left >= 5 && p[0] == 1 ? get32(p + 1) : 0;
    default: return 0;
  }
}

}  // namespace wire
}  // namespace comms

#endif  // INKDECK_COMMS
