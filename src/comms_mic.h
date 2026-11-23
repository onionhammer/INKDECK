// Comms (issue #66): the open mic's plain state and its packets, as plain C++ with no Arduino or
// ESP-IDF calls, so the self-test (comms::selfTest()) drives it with a made-up clock. The gate
// that decides which of this board's mic frames go out (with the echo of its own speaker held
// back), the 2-hour level history, a board's view of another board's mic (QUIET, SOUND, LOST),
// the mixer that plays several mics at once, and the bodies of types 2, 3, 4 and 15.
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

// The mic's gate, a frame at a time. Every frame is kept for kPreFrames; a frame goes out
// kPreFrames after it was captured, and only if a frame over the threshold came within
// kPreFrames after it or kHangFrames before it. So a sound goes out with the 160 ms before it
// (the start of a word isn't cut), and on for 2 s after the level drops, always 160 ms late. Two
// sounds closer than that are one.
//
// The echo: while this board's speaker plays, echoDb is the level the speaker would put on the
// mic (what it plays, plus the coupling measured, plus a margin: comms.cpp). A frame no louder
// than that could be the echo: it doesn't count as loud, and if it goes out (pre-roll or hang) it
// goes as silence (muted), so the others don't hear themselves back, while a voice over it, louder
// than the echo could be, goes as it is.
class MicGate {
 public:
  static constexpr uint32_t kPreFrames = 8;     // 160 ms
  static constexpr uint32_t kHangFrames = 100;  // 2 s
  static constexpr uint32_t kRing = 16;         // frames remembered: the pre-roll and more
  struct Step {
    bool send = false;   // the frame captured kPreFrames ago goes out now
    bool first = false;  // and it begins a sound
    bool muted = false;  // as silence: the echo could explain it
  };
  void setThreshold(int db) { threshold_ = db; }
  int threshold() const { return threshold_; }
  // The next frame captured, at levelDb, while the speaker would put echoDb on the mic (-96: it's
  // silent).
  Step frame(float levelDb, float echoDb = -96);
  // The mic was off: nothing before this is pre-roll for what comes after.
  void reset();
  bool sending() const { return sending_; }
  // The mic goes off: the next frame still held back (the last kPreFrames captured), as the gate
  // would let it out if only silence came after, with its echo-muted flag; send is false once the
  // sound's hang no longer covers it. Only kPreFrames are held: call it that many times at most.
  Step drain() { return frame(-96, -96); }

 private:
  int threshold_ = -30;
  uint32_t n_ = 0, lastLoud_ = 0;
  bool loud_ = false, sending_ = false;
  bool masked_[kRing] = {};
};

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

// Another board's open mic as this board knows it: QUIET (on, its gate closed), SOUND (its voice
// comes), LOST (nothing from it for kLostMs without its off: an alarm until ack() or the link is
// back), and its sessions (on to off) and events (sounds and LOSTs). A clean off ends it (the
// caller forgets it).
class HeardMic {
 public:
  enum class Link : uint8_t { Off, Quiet, Sound, Lost };
  static constexpr uint32_t kLostMs = 20000;
  static constexpr uint32_t kSoundEndMs = 600;  // no voice this long: the sound is over
  static constexpr int kLoudMarginDb = 12;      // LOUD: a frame this far over its threshold
  static constexpr size_t kMaxEvents = 48;      // the oldest goes

  struct Event {
    bool lost = false;  // else a sound
    uint32_t startMs = 0, endMs = 0;  // a sound: its first and last frame; LOST: the last packet
                                      // before, and the first after
    bool open = false;  // still going
    bool loud = false;
    float peakDb = -96;
    uint16_t sound = 0;  // the mic's number for it
  };

  // What a packet changed, for the caller to log.
  struct Change {
    bool shown = false;  // something a screen shows
    bool began = false;  // a session began (session())
    bool ended = false;  // the session before ended: its off, or a new one in its place
    bool back = false;   // the link is back after LOST
    uint16_t endedSession = 0;
    uint32_t endedMs = 0;  // how long the ended session was on
  };

  // A packet saying the mic is on (a still-here or its voice): its session, on for onMs (0 for
  // voice, which doesn't say).
  Change on(uint32_t now, uint16_t session, uint32_t onMs);
  // A frame of its voice in sound `sound` at levelDb (after on()). becameLoud: this frame made
  // the sound LOUD (once a sound). True if anything shown changed.
  bool audio(uint32_t now, uint16_t sound, float levelDb, bool &becameLoud);
  // The mic said off (session 0: whichever is on; its board's presence said its mic is off).
  Change off(uint32_t now, uint16_t session);
  // LOST after kLostMs without a packet (with its alarm if `alarmable`); a sound over after
  // kSoundEndMs without a frame. True if anything shown changed.
  bool tick(uint32_t now, bool alarmable);
  // LOST's alarm stops; LOST stays until the link is back. False if it wasn't sounding.
  bool ack();
  void setThreshold(int db) { threshold_ = db; }

  Link link() const { return link_; }
  bool on() const { return link_ != Link::Off; }
  uint16_t session() const { return session_; }
  uint32_t startMs() const { return startMs_; }  // the session's start, as this board reckons
  uint32_t sinceMs() const { return sinceMs_; }  // in this state since
  uint32_t heardMs() const { return heardMs_; }
  bool alarm() const { return link_ == Link::Lost && !acked_; }
  int threshold() const { return threshold_; }
  const std::vector<Event> &events() const { return events_; }  // oldest first
  const Event *lastSound() const;  // over or not; nullptr if none yet

 private:
  Event *openEvent(bool lost);
  void push(const Event &e);
  void closeAll(uint32_t now);
  Link link_ = Link::Off;
  uint16_t session_ = 0;
  uint32_t startMs_ = 0, sinceMs_ = 0, heardMs_ = 0, soundMs_ = 0;
  bool acked_ = false;
  int threshold_ = -30;
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

// Plays several mics at once: a jitter buffer a source (a board's sound: its gate open), pulled a
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
    uint8_t bytes = 0;  // 0: silence (the mic's echo, muted)
    uint8_t data[kFrameBytes] = {};
  };
  enum class Took : uint8_t { Ok, Late, Copy, NoRoom };
  Took add(uint32_t sender, uint16_t sound, const Frame &f, uint32_t now);

  // What each source put into the last pull.
  struct Played {
    uint32_t sender = 0;
    uint16_t sound = 0;
    bool lost = false;   // its frame wasn't there: covered
    bool first = false;  // the first frame of its sound that played
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
  // A sound that has played out (its frames ran dry, or another sound of its board began), for
  // the log: its frames that came, the highest numbered (+1: as many as were sent, if the last
  // came), those that played and those covered as lost. A sound that rests and goes on is
  // reported again, counted from its start. Takes it, once.
  struct Ended {
    uint32_t sender = 0;
    uint16_t sound = 0;
    uint16_t came = 0, top = 0, played = 0, lost = 0;
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
    uint16_t sound = 0;
    bool started = false, primed = false, played = false;
    uint16_t next = 0;
    int dry = 0;
    uint32_t firstMs = 0, lastMs = 0;
    uint16_t got = 0, lost = 0;   // of its sound: frames played, frames covered
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

// --- Packets ----------------------------------------------------------------------------------
// Each put*() writes a whole body (sentAt first) and returns its size (0 if it doesn't fit); each
// read*() takes a Reader just past sentAt (type 15's: past the ext type too) and is false for a
// short or bad body.

// Type 2, mic: the mic is on, every 2 s while it is. Flags:
constexpr uint8_t kMicSending = 0x01;  // its gate is open: its voice goes out
constexpr uint8_t kMicUsb = 0x02;      // its board is on USB power
constexpr uint8_t kMicPaused = 0x04;   // something else has the mic for now (the agent, ...)
constexpr uint8_t kMicOpening = 0x08;  // its session opened the conversation: boards join it
struct MicStill {
  uint8_t flags = 0;
  uint8_t battery = 0xFF;   // percent, 0xFF none
  uint8_t threshold = 30;   // dB below full scale
  uint8_t peak = kNoLevel;  // the loudest frame since the last still-here
  uint16_t session = 0;     // its board's counter, one more each time the mic goes on
  uint32_t onS = 0;         // on for
  uint16_t bar = 0;         // the bar now (MicHistory)
  uint8_t barPeak = kNoLevel;
  uint8_t preFrames = MicGate::kPreFrames;
  uint32_t target = 0;  // 0: all of the group, else the one board it's to
  uint32_t conv = 0;    // the conversation it's in
};
size_t putMicStill(uint8_t *body, size_t cap, uint32_t sentAt, const MicStill &s);
bool readMicStill(Reader &r, MicStill &s);

// Type 3, voice: 20 ms of an open mic. Flags:
constexpr uint8_t kVoiceOpening = 0x01;  // as kMicOpening
struct Voice {
  uint32_t target = 0;  // first, for the relay rule
  uint32_t conv = 0;
  uint16_t session = 0;
  uint16_t sound = 0;  // the gate's sounds, numbered on across sessions
  uint16_t frame = 0;  // within the sound, from 0
  uint8_t level = kNoLevel;  // the frame's own, as the mic measured it
  uint8_t flags = 0;
  int16_t predictor = 0;
  uint8_t index = 0;
  uint8_t bytes = 0;  // 0: silence (muted as echo)
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
