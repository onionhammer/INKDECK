#include "comms_reach.h"

#include "features.h"

#if INKDECK_COMMS  // the whole file (features.h)

#include <string.h>

#include <algorithm>

namespace comms {
namespace wire {

size_t putPoll(uint8_t *body, size_t cap, uint32_t sentAt, const Poll &p) {
  Writer w(body, cap);
  w.u32(sentAt);
  w.u8(kExtPoll);
  w.u32(0);
  w.u8(p.flags);
  w.u8(p.battery);
  w.u16(p.intervalS);
  const uint8_t n = std::min<uint8_t>(p.nameLen, sizeof p.name);
  w.u8(n);
  w.bytes(p.name, n);
  return w.ok() ? w.size() : 0;
}

bool readPoll(Reader &r, Poll &p) {
  r.u32();  // to: all
  p.flags = r.u8();
  p.battery = r.u8();
  p.intervalS = r.u16();
  p.nameLen = r.u8();
  if (!r.ok() || p.nameLen > sizeof p.name) return false;
  const uint8_t *n = r.bytes(p.nameLen);
  if (!n) return false;
  memcpy(p.name, n, p.nameLen);
  return true;
}

size_t putAnswer(uint8_t *body, size_t cap, uint32_t sentAt, const Answer &a) {
  Writer w(body, cap);
  w.u32(sentAt);
  w.u8(kExtAnswer);
  w.u32(a.to);
  w.u8(a.what);
  w.u8(a.channel);
  w.u8(a.flags);
  w.u32(a.dialer);
  w.u16(a.ring);
  w.u32(a.msgSender);
  w.u32(a.msg);
  w.u32(a.msgTo);
  w.u16(a.frames);
  return w.ok() ? w.size() : 0;
}

bool readAnswer(Reader &r, Answer &a) {
  a.to = r.u32();
  a.what = r.u8();
  a.channel = r.u8();
  a.flags = r.u8();
  a.dialer = r.u32();
  a.ring = r.u16();
  a.msgSender = r.u32();
  a.msg = r.u32();
  a.msgTo = r.u32();
  a.frames = r.u16();
  return r.ok();
}

uint32_t reachExtFor(const uint8_t *body, size_t len) {
  if (len < 5 + 4 || body[4] != kExtAnswer) return 0;
  return get32(body + 5);
}


// --- Sleeper -------------------------------------------------------------------------------------

uint32_t Sleeper::pollsFor(uint32_t seconds, uint16_t intervalS) {
  const uint32_t i = intervalS ? intervalS : 1;
  return (seconds + i - 1) / i;
}

void Sleeper::begin(SleepPlan &p, uint8_t channel, uint16_t intervalS) {
  p = SleepPlan();
  p.channel = channel >= 1 && channel <= 13 ? channel : 6;
  p.nextEcho = 1;  // the first poll checks the channel too
  (void)intervalS;
}

Sleeper::Next Sleeper::next(const SleepPlan &p, uint16_t intervalS) {
  (void)intervalS;
  Next n;
  const uint32_t number = p.polls + 1;
  if (p.misses >= kMissesToSweep && (p.nextSweep == 0 || number >= p.nextSweep)) {
    n.sweep = true;
    n.flags = kPollEcho | kPollSweep;
    return n;
  }
  if (number >= p.nextEcho) n.flags = kPollEcho;
  return n;
}

void Sleeper::result(SleepPlan &p, uint16_t intervalS, const Next &n, bool answered) {
  ++p.polls;
  if (answered) p.misses = 0;  // any answer says boards are on this channel
  if (n.flags & kPollEcho) {
    if (!answered && p.misses < 255) ++p.misses;
    p.nextEcho = p.polls + pollsFor(kEchoEveryS, intervalS);
  }
}

void Sleeper::sweepResult(SleepPlan &p, uint16_t intervalS, uint8_t channel) {
  ++p.polls;
  p.nextEcho = p.polls + pollsFor(kEchoEveryS, intervalS);
  if (channel >= 1 && channel <= 13) {
    p.channel = channel;
    p.misses = 0;
    p.nextSweep = 0;
  } else {
    // Nobody on any channel (no board on USB in range): the channel stays, and the next sweep
    // waits.
    p.nextSweep = p.polls + pollsFor(kSweepAgainS, intervalS);
  }
}

// --- WokenHold -----------------------------------------------------------------------------------

void WokenHold::begin(uint32_t now, uint8_t what) {
  *this = WokenHold();
  active_ = true;
  what_ = what;
  wakeMs_ = now;
}

WokenHold::Verdict WokenHold::step(uint32_t now, const Seen &s) {
  if (!active_) return Verdict::On;
  if (s.keyed) {
    active_ = false;
    return Verdict::On;
  }
  const uint32_t at = now ? now : 1;
  if (s.ringing) ringMs_ = at;
  if (s.transfer) transferMs_ = at;
  if (s.sound) {
    soundMs_ = at;
    micAfterSound_ = false;
  } else if (soundMs_) {
    micAfterSound_ = s.micOn;
  }
  if (s.ringing || s.sound || s.transfer) return Verdict::Stay;
  // How long after each thing that happened: the latest of them decides.
  uint32_t until = wakeMs_ + kNothingMs;
  auto later = [&](uint32_t from, uint32_t ms) {
    if (from && static_cast<int32_t>(from + ms - until) > 0) until = from + ms;
  };
  later(ringMs_, kAfterMs);
  later(transferMs_, kAfterMs);
  later(soundMs_, micAfterSound_ ? kQuietMicMs : kAfterMs);
  if (static_cast<int32_t>(now - until) < 0) return Verdict::Stay;
  active_ = false;
  return Verdict::Sleep;
}

// --- RingWatch -----------------------------------------------------------------------------------

void RingWatch::heard(int group, uint32_t dialer, uint16_t ring, uint32_t target,
                      uint32_t elapsed, uint32_t now) {
  if (elapsed >= kRingMs) return;
  Seen *slot = nullptr;
  for (Seen &s : rings_) {
    if (s.dialer == dialer && s.group == group) {  // a dialer rings one ring at a time
      slot = &s;
      break;
    }
  }
  if (!slot) {
    for (Seen &s : rings_) {
      if (!s.dialer || static_cast<uint32_t>(now - s.lastMs) >= kLostMs) {
        slot = &s;
        break;
      }
    }
  }
  if (!slot) {  // all on: the one heard from longest ago goes
    slot = &rings_[0];
    for (Seen &s : rings_) {
      if (static_cast<int32_t>(s.lastMs - slot->lastMs) < 0) slot = &s;
    }
  }
  if (slot->dialer != dialer || slot->ring != ring || slot->group != group) {
    slot->startMs = now - elapsed;
  }
  slot->group = group;
  slot->dialer = dialer;
  slot->ring = ring;
  slot->target = target;
  slot->lastMs = now ? now : 1;
}

void RingWatch::over(uint32_t dialer, uint16_t ring) {
  for (Seen &s : rings_) {
    if (s.dialer == dialer && s.ring == ring) s = Seen();
  }
}

bool RingWatch::ringFor(int group, uint32_t board, uint32_t now, Seen &out) const {
  for (const Seen &s : rings_) {
    if (!s.dialer || s.group != group || s.dialer == board) continue;
    if (static_cast<uint32_t>(now - s.lastMs) >= kLostMs) continue;
    if (static_cast<uint32_t>(now - s.startMs) >= kRingMs) continue;
    if (s.target && s.target != board) continue;
    out = s;
    return true;
  }
  return false;
}

void RingWatch::clear() {
  for (Seen &s : rings_) s = Seen();
}

// --- What a poll wants ---------------------------------------------------------------------------

uint8_t waitingFor(const WaitingNow &w, int group, uint32_t sleeper, uint16_t intervalS,
                   uint32_t now) {
  uint8_t what = 0;
  if (w.ringOn) what |= kWaitRing;
  const uint32_t window = static_cast<uint32_t>(intervalS) * 1000 + kVoiceGraceMs;
  for (size_t i = 0; i < w.micCount && i < WaitingNow::kMics; ++i) {
    const WaitingNow::MicNow &m = w.mics[i];
    if (m.group != group || m.board == sleeper || !m.soundMs) continue;
    if (m.target ? m.target != sleeper : !m.opened) continue;
    if (static_cast<uint32_t>(now - m.soundMs) <= window) {
      what |= kWaitVoice;
      break;
    }
  }
  if (w.messageOn && w.msgGroup == group && w.msgSender != sleeper &&
      (w.msgTo == 0 || w.msgTo == sleeper)) {
    bool has = false;
    for (size_t i = 0; i < w.delivered && i < 8; ++i) has = has || w.deliveredTo[i] == sleeper;
    if (!has) what |= kWaitMessage;
  }
  return what;
}

// --- Settings and what the off faces show ------------------------------------------------------

int nextReachStep(int seconds) {
  if (seconds <= 0) return kReachSteps[0];
  for (const int step : kReachSteps) {
    if (step > seconds) return step;
  }
  return 0;
}

void addMissed(MissedRec &r, uint32_t from, const char *name, uint32_t at) {
  r.from = from;
  memset(r.name, 0, sizeof r.name);
  if (name) strncpy(r.name, name, sizeof r.name - 1);
  r.at = at;
  if (r.count < 0xFFFF) ++r.count;
}

}  // namespace wire
}  // namespace comms

#endif  // INKDECK_COMMS
