// Comms (issue #66): the walkie-talkie's plain state and its packets, as plain C++ with no Arduino
// or ESP-IDF calls, so the self-test (comms::selfTest()) drives it with a made-up clock. Push to
// talk and its latch, the 2-hour level history, a board's view of another board's mic (on, LOST,
// cut off), the mixer that plays several mics at once, half duplex (nothing plays while this board
// transmits), the call sheet's once per conversation, and the bodies of types 2, 3, 4 and 15.
// docs/comms-protocol.md says what goes on the air; comms.cpp's task owns these and feeds them.
// Times are millis() of the board that keeps them. A level is a 20 ms frame's loudest sample in
// dBFS; on the air it's a byte, dB below full scale.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <vector>

#include "adpcm.h"
#include "comms_wire.h"

namespace comms {
namespace wire {

// A level on the air: 0 (full scale) to 96 dB below it, 0xFF for none.
constexpr uint8_t kNoLevel = 0xFF;
uint8_t levelByte(float db);
float levelDb(uint8_t b);  // -96 for kNoLevel

// 2 hours of levels: 60 bars of 2 minutes, the loudest frame of each, by bar number since the mic
// went on. The mic's board keeps it from its frames; a board that hears it mirrors it from the
// still-heres (the bar now) and the history sent when asked (the bars before).
class MicHistory {
 public:
  static constexpr size_t kBars = 60;
  static constexpr uint32_t kBarMs = 120000;
  void clear() { *this = MicHistory(); }
  // The mic's board: a frame at `ms` since the mic went on.
  void note(uint32_t ms, float levelDb);
  // A bar as the mic's board says it is. A newer bar forgets those more than kBars before it.
  void set(uint16_t bar, uint8_t level);
  bool any() const { return any_; }
  uint16_t top() const { return top_; }  // the newest bar
  uint8_t at(uint16_t bar) const;        // kNoLevel if none, not yet or forgotten
  uint16_t first() const { return top_ + 1 >= kBars ? top_ + 1 - kBars : 0; }  // the oldest kept

 private:
  void advance(uint16_t bar);
  uint8_t bars_[kBars] = {};
  uint16_t top_ = 0;
  bool any_ = false;
};

// Another board's mic as this board knows it: on (its voice comes while it's held or latched),
// LOST (a latched mic with nothing from it for kLostMs and no off: an alarm until ack() or the
// link is back), and its sessions (a press, on to off). A held mic (not latched) that goes silent
// without its off is cut off after kCutMs: it ends there, with no alarm (a board walking out of
// range mid-press). A clean off ends it (the caller forgets it).
class HeardMic {
 public:
  enum class Link : uint8_t { Off, On, Lost };
  // A mic on sends 50 voice frames a second, or a still-here every 2 s while paused: 5 s is two
  // still-heres missed and 250 frames; a latched mic is LOST after 10 s (five still-heres).
  static constexpr uint32_t kCutMs = 5000;
  static constexpr uint32_t kLostMs = 10000;
  static constexpr size_t kMaxEvents = 48;  // the oldest goes

  // A LOST outage: from the last packet before to the first after.
  struct Event {
    uint32_t startMs = 0, endMs = 0;
    bool open = false;  // still going
  };

  // What a packet (or tick()) changed, for the caller to log.
  struct Change {
    bool shown = false;  // something a screen shows
    bool began = false;  // a session began (session())
    bool ended = false;  // the session before ended: its off, another in its place, or cut off
    bool cut = false;    // it ended cut off (held, then nothing)
    bool lost = false;   // it went LOST now
    bool back = false;   // the link is back after LOST
    uint16_t endedSession = 0;
    uint32_t endedMs = 0;  // how long the ended session was on
  };

  // A packet saying the mic is on (a still-here or its voice): its session, on for onMs (0 for
  // voice, which doesn't say), and whether it's latched.
  Change on(uint32_t now, uint16_t session, uint32_t onMs, bool latched);
  // A frame of its voice came (after on()).
  void voice(uint32_t now) { voiceMs_ = now; }
  // The mic said off (session 0: whichever is on; its board's presence said its mic is off).
  Change off(uint32_t now, uint16_t session);
  // A held mic silent for kCutMs is cut off (ended); a latched one LOST after kLostMs (with its
  // alarm if `alarmable`).
  Change tick(uint32_t now, bool alarmable);
  // LOST's alarm stops; LOST stays until the link is back. False if it wasn't sounding.
  bool ack();

  Link link() const { return link_; }
  bool on() const { return link_ != Link::Off; }
  bool latched() const { return latched_; }
  uint16_t session() const { return session_; }
  uint32_t startMs() const { return startMs_; }  // the session's start, as this board reckons
  uint32_t sinceMs() const { return sinceMs_; }  // in this state since
  uint32_t heardMs() const { return heardMs_; }  // its last packet
  uint32_t voiceMs() const { return voiceMs_; }  // its last voice frame; 0 none this session
  bool alarm() const { return link_ == Link::Lost && !acked_; }
  const std::vector<Event> &events() const { return events_; }  // oldest first

 private:
  Link link_ = Link::Off;
  bool latched_ = false;
  uint16_t session_ = 0;
  uint32_t startMs_ = 0, sinceMs_ = 0, heardMs_ = 0, voiceMs_ = 0;
  bool acked_ = false;
  std::vector<Event> events_;
};

// Covers a lost 20 ms frame: the one before it again at half the level, then silence. A
// click-free gap for the odd frame the air loses (about 4% at -76 dBm, 2026-11-22).
struct Concealer {
  int16_t prev[kFrameSamples] = {};
  size_t samples = 0;
  int run = 0;  // lost frames in a row
  // pcm holds what decoded (or silence, for a lost frame); fixes up a lost one.
  void apply(int16_t *pcm, size_t n, bool lost) {
    if (!lost) {
      memcpy(prev, pcm, n * sizeof(int16_t));
      samples = n;
      run = 0;
      return;
    }
    if (run++ == 0 && samples) {
      for (size_t i = 0; i < n; ++i) pcm[i] = static_cast<int16_t>(prev[i % samples] / 2);
    }
  }
};

// Plays several mics at once: a jitter buffer a source (a board's session: a press), pulled a
// 20 ms frame at a time at this board's pace and summed. A source plays once kPrime frames are in
// (or one has waited kPrimeWaitMs); a frame missing when its turn comes is lost (the one before
// at half level, then silence) and dropped if it comes later; a source with nothing for
// kDryFrames turns rests until frames come again, and is gone after kIdleMs. A source more than
// kSlots frames ahead (its clock faster than this board's, or a burst) skips its oldest.
class Mixer {
 public:
  static constexpr size_t kSources = 4;  // at once; a fifth board's voice waits for a free one
  static constexpr size_t kSlots = 8;    // 160 ms a source
  static constexpr int kPrime = 2;
  static constexpr uint32_t kPrimeWaitMs = 40;
  static constexpr int kDryFrames = 3;
  static constexpr uint32_t kIdleMs = 2000;
  static constexpr int kResyncFrames = 50;  // a jump this far (1 s) starts again there
  static constexpr int kJitterWait = 3;     // a first frame this close to 0 starts at 0

  struct Frame {
    uint16_t frame = 0;
    int16_t predictor = 0;
    uint8_t index = 0;
    uint8_t bytes = 0;  // under kFrameBytes: a press's last, short frame (the rest silence)
    uint8_t data[kFrameBytes] = {};
  };
  enum class Took : uint8_t { Ok, Late, Copy, NoRoom };
  Took add(uint32_t sender, uint16_t session, const Frame &f, uint32_t now);

  // What each source put into the last pull.
  struct Played {
    uint32_t sender = 0;
    uint16_t session = 0;
    bool lost = false;   // its frame wasn't there: covered
    bool first = false;  // the first frame of its session that played
    uint32_t waitedMs = 0;  // first: from its first frame coming to playing
    float levelDb = -96;  // this frame's
  };
  // The next 20 ms of every source playing, summed (held under full scale) into
  // out[kFrameSamples]; silence if none. Returns how many sources are in it (played()).
  int pull(int16_t *out, uint32_t now);
  const Played *played(size_t &n) const {
    n = played_;
    return playedAt_;
  }
  bool active() const;  // a source has frames, or plays
  // A session that has played out (its frames ran dry, or another session of its board began),
  // for the log: its frames that came, the highest numbered (+1: as many as were sent, if the last
  // came), those that played and those covered as lost. A session that rests and goes on is
  // reported again, counted from its start. Takes it, once.
  struct Ended {
    uint32_t sender = 0;
    uint16_t session = 0;
    // Frames from..top-1 are what this source saw of the session (from: where it started, 0 unless
    // it came in late, or again after a gap of more than kIdleMs).
    uint16_t from = 0, came = 0, top = 0, played = 0, lost = 0;
  };
  bool ended(Ended &out);
  size_t sources() const;
  bool has(uint32_t sender) const;
  void drop(uint32_t sender);  // its voice stops (a board left, or this board left)
  void clear();

 private:
  struct Slot {
    bool full = false;
    uint32_t atMs = 0;  // when it came
    Frame f;
  };
  struct Source {
    bool used = false;
    uint32_t sender = 0;
    uint16_t session = 0;
    bool started = false, primed = false, played = false;
    uint16_t next = 0;
    int dry = 0;
    uint32_t firstMs = 0, lastMs = 0;
    uint16_t got = 0, lost = 0;   // of its session: frames played, frames covered
    uint16_t from = 0;            // the frame it started at
    uint16_t came = 0, top = 0;   // frames taken, the highest frame number + 1
    bool reported = false;        // noteEnd() since the counts last changed
    Slot slots[kSlots];
    Concealer conceal;
  };
  void noteEnd(Source &s);
  Source src_[kSources];
  Ended ended_;
  bool endedNew_ = false;
  Played playedAt_[kSources];
  size_t played_ = 0;
  int32_t sum_[kFrameSamples] = {};
  int16_t pcm_[kFrameSamples] = {};
};

// Half duplex, as a walkie-talkie: while this board transmits (its mic on and capturing), nothing
// of comms reaches its speaker. The mixer keeps pulling, a frame each 20 ms (buffers drain, the
// last 60 s heard keep what was talked over), and nothing it made is written to the speaker.
struct HalfDuplex {
  uint32_t talkedOver = 0;  // frames of voice silenced
  // pcm: what the mixer made (voices of them in it). True if it plays as it is.
  bool apply(int16_t *pcm, size_t n, int voices, bool transmitting) {
    if (!transmitting) return true;
    if (voices > 0) ++talkedOver;
    memset(pcm, 0, n * sizeof(int16_t));
    return false;
  }
};

// The mic's noise gate, a frame at a time, before the gain: the mic's own noise and the room's,
// which the gain would bring up with the voice, are turned down kClosedDb while nothing rises over
// them. Every frame still goes out, and nothing is fixed: the room is the quietest frame (RMS) of
// the last kBlocks * kBlockFrames frames (2.9 s; kept across presses), and a frame kOpenDb over
// it opens the gate within kOpenSamples of its start (a word that begins inside a frame isn't
// touched), for that frame and kHoldFrames after; then it closes over kCloseSamples. A room that
// gets louder holds it open until the window has only the new room in it.
class NoiseGate {
 public:
  static constexpr float kClosedDb = -24;
  static constexpr float kOpenDb = 8;
  static constexpr float kStartRoomDb = -72;  // RMS; the test boards' rooms read about that
  static constexpr float kMinRoomDb = -84;    // a frame of zeros isn't a room
  static constexpr uint32_t kHoldFrames = 20;     // 400 ms
  static constexpr uint32_t kBlockFrames = 12;    // 240 ms
  static constexpr size_t kBlocks = 12;
  static constexpr uint32_t kOpenSamples = 80;    // 5 ms
  static constexpr uint32_t kCloseSamples = 3200; // 200 ms
  NoiseGate();
  // n samples at 16 kHz, in place.
  void process(int16_t *pcm, size_t n);
  bool open() const { return hold_ > 0; }
  float roomDb() const;

 private:
  float blocks_[kBlocks];  // the quietest frame of each block before this one
  size_t at_ = 0;
  float now_ = 0;          // of the block filling; 0: no frame yet
  uint32_t inBlock_ = 0;
  uint32_t hold_ = 0;      // frames it stays open
  float gain_;             // on the last sample
};

// The mic's automatic gain, a frame at a time, before a frame is measured and encoded. The mic
// gives a voice at arm's length peaks of about -35 to -40 dBFS (measured 2026-11-24, the codec's
// PGA at its 30 dB and 12 dB after it), far too quiet at the other board's speaker. This follows
// the voice's peaks and puts them at kTargetDb, with up to kMaxGainDb. A louder frame brings the
// gain down at once (nothing clips); a quieter voice brings it up at kReleaseDbPerS; a frame under
// kFloorDb is the room, and leaves it alone (silence doesn't pump the gain up). It isn't reset
// between presses: a press starts with the gain the last one ended with.
class MicAgc {
 public:
  static constexpr float kTargetDb = -6;
  static constexpr float kMaxGainDb = 30;
  static constexpr float kStartGainDb = 24;
  static constexpr float kFloorDb = -50;  // of the mic's own level, before the gain
  static constexpr float kReleaseDbPerS = 6;
  // n samples at 16 kHz, in place.
  void process(int16_t *pcm, size_t n);
  float gainDb() const { return gainFor(envDb_); }

 private:
  static float gainFor(float envDb) {
    const float g = kTargetDb - envDb;
    return g < 0 ? 0 : g > kMaxGainDb ? kMaxGainDb : g;
  }
  float envDb_ = kTargetDb - kStartGainDb;  // the voice's peaks, as followed
};

// Push to talk, and its latch: HOLD BOOT keys the mic while held; ● while it's held latches it, and
// then BOOT can go and the mic stays on until the next ● or BOOT press, which only ends it (it isn't
// also a ring or a BACK). The keys come from the UI (apps/comms_ui.cpp); this says what they do.
class TalkLatch {
 public:
  enum class Act : uint8_t { None, MicOn, MicOff, Latch, Unlatch };
  // HOLD BOOT began on a screen that talks to the boards. micOn: the mic was on already (a
  // latched one: the hold takes it over, and its release ends it; else it's left as it is).
  Act holdStart(bool micOn);
  // HOLD BOOT let go.
  Act holdEnd();
  // ● (select) or BOOT pressed; ends: a press there ends a latch (the Comms screens and sheets).
  // True: the press is used up (it latched, unlatched, or was a ● while BOOT is held), and does
  // nothing else.
  bool press(bool select, bool ends, Act &act);
  bool held() const { return held_; }
  bool latched() const { return latched_; }
  void reset() { held_ = latched_ = false; }

 private:
  bool held_ = false;     // the mic is keyed by the hold now
  bool latched_ = false;  // it stays on after the hold
};

// The call sheet and the call chirp, once per conversation: the first voice that plays here in a
// conversation, not every press in it (a press is a session). Voice outside any conversation (0)
// counts once per board and session.
class FirstHeard {
 public:
  bool first(uint32_t conv, uint32_t sender, uint16_t session) {
    const uint64_t key = conv ? (static_cast<uint64_t>(1) << 63) | conv
                              : (static_cast<uint64_t>(sender) << 16) | session;
    if (key == last_) return false;
    last_ = key;
    return true;
  }
  void reset() { last_ = 0; }

 private:
  uint64_t last_ = 0;
};

// --- Packets ----------------------------------------------------------------------------------
// Each put*() writes a whole body (sentAt first) and returns its size (0 if it doesn't fit); each
// read*() takes a Reader just past sentAt (type 15's: past the ext type too) and is false for a
// short or bad body.

// Type 2, mic: the mic is on, every 2 s while it is (at once when it latches). Flags:
constexpr uint8_t kMicLatched = 0x01;  // latched (held transmit): it goes LOST, not cut off
constexpr uint8_t kMicUsb = 0x02;      // its board is on USB power
constexpr uint8_t kMicPaused = 0x04;   // something else has the mic for now (the agent, ...)
constexpr uint8_t kMicOpening = 0x08;  // its session opened the conversation: boards join it
struct MicStill {
  uint8_t flags = 0;
  uint8_t battery = 0xFF;   // percent, 0xFF none
  uint8_t peak = kNoLevel;  // the loudest frame since the last still-here
  uint16_t session = 0;     // its board's counter, one more each time the mic goes on
  uint32_t onS = 0;         // on for
  uint16_t bar = 0;         // the bar now (MicHistory)
  uint8_t barPeak = kNoLevel;
  uint32_t target = 0;  // 0: all of the group, else the one board it's to
  uint32_t conv = 0;    // the conversation it's in
};
size_t putMicStill(uint8_t *body, size_t cap, uint32_t sentAt, const MicStill &s);
bool readMicStill(Reader &r, MicStill &s);

// Type 3, voice: 20 ms of a mic that's on. Flags:
constexpr uint8_t kVoiceOpening = 0x01;  // as kMicOpening
constexpr uint8_t kVoiceLatched = 0x02;  // as kMicLatched
struct Voice {
  uint32_t target = 0;  // first, for the relay rule
  uint32_t conv = 0;
  uint16_t session = 0;
  uint16_t frame = 0;  // within the session, from 0
  uint8_t level = kNoLevel;  // the frame's own, as the mic measured it
  uint8_t flags = 0;
  int16_t predictor = 0;
  uint8_t index = 0;
  uint8_t bytes = 0;  // kFrameBytes; fewer for a press's last frame
  uint8_t data[kFrameBytes] = {};
};
size_t putVoice(uint8_t *body, size_t cap, uint32_t sentAt, const Voice &v);
bool readVoice(Reader &r, Voice &v);

// Type 4, mic off.
constexpr uint8_t kOffSwitched = 1, kOffPowerOff = 2;
struct MicOff {
  uint16_t session = 0;
  uint8_t reason = kOffSwitched;
  uint32_t onS = 0;  // how long it was on
};
size_t putMicOff(uint8_t *body, size_t cap, uint32_t sentAt, const MicOff &o);
bool readMicOff(Reader &r, MicOff &o);

// Type 15 (kExt): the 4-bit type ran out at 15, so type 15's body says its own type: sentAt
// (4), the ext type (1), then the fields.
enum ExtType : uint8_t {
  kHistoryAsk = 1,  // a board asks a mic's board for its 2 hours of levels
  kHistory = 2,     // the levels, to the board that asked
};
constexpr uint8_t kLastExt = kHistory;

struct History {
  uint32_t to = 0;
  uint16_t session = 0;
  uint16_t first = 0;
  uint8_t count = 0;  // up to MicHistory::kBars
  uint8_t bars[MicHistory::kBars] = {};
};
size_t putHistoryAsk(uint8_t *body, size_t cap, uint32_t sentAt, uint32_t board);
size_t putHistory(uint8_t *body, size_t cap, uint32_t sentAt, const History &h);
bool readHistory(Reader &r, History &h);
// The board a type 15 body is for alone (the relay rule): a history ask's mic, a history's
// asker; 0 for the others.
uint32_t extFor(const uint8_t *body, size_t len);

}  // namespace wire
}  // namespace comms
