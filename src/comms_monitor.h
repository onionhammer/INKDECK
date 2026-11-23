// Comms (issue #66): the baby monitor's plain state and its packets, as plain C++ with no Arduino
// or ESP-IDF calls, so the self-test (comms::selfTest()) drives it with a made-up clock.
// docs/comms-protocol.md says what goes on the air; comms.cpp's task owns these and feeds them.
// Times are millis() of the board that keeps them. A level is a 20 ms frame's loudest sample in
// dBFS; on the air it's a byte, dB below full scale.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include <vector>

#include "comms_wire.h"

namespace comms {
namespace wire {

// A level on the air: 0 (full scale) to 96 dB below it, 0xFF for none.
constexpr uint8_t kNoLevel = 0xFF;
uint8_t levelByte(float db);
float levelDb(uint8_t b);  // -96 for kNoLevel

// Who may listen to a room: all of its group (none listed), or the boards listed.
struct ListenerSet {
  static constexpr size_t kMax = 8;
  uint8_t n = 0;
  uint32_t ids[kMax] = {};
  bool all() const { return n == 0; }
  bool allows(uint32_t id) const;
};

// The room's gate, a frame at a time. Every frame is kept for kPreFrames; a frame goes out
// kPreFrames after it was captured, and only if a frame over the threshold came within
// kPreFrames after it or kHangFrames before it. So a sound goes out with the half second before
// it (the start of a cry isn't cut), and on for 2 s after the level drops, always half a second
// late. Two sounds closer than that are one.
class MonitorGate {
 public:
  static constexpr uint32_t kPreFrames = 25;    // 0.5 s
  static constexpr uint32_t kHangFrames = 100;  // 2 s
  struct Step {
    bool send = false;   // the frame captured kPreFrames ago goes out now
    bool first = false;  // and it begins a sound
  };
  void setThreshold(int db) { threshold_ = db; }
  int threshold() const { return threshold_; }
  // The next frame captured, at levelDb.
  Step frame(float levelDb);
  // The mic was off: nothing before this is pre-roll for what comes after.
  void reset();
  bool sending() const { return sending_; }

 private:
  int threshold_ = -30;
  uint32_t n_ = 0, lastLoud_ = 0;
  bool loud_ = false, sending_ = false;
};

// 2 hours of levels: 60 bars of 2 minutes, the loudest frame of each, by bar number since the room
// started. The room keeps it from its frames; a listener mirrors it from the still-here packets
// (the bar now) and the history the room sends when asked (the bars before).
class MonitorHistory {
 public:
  static constexpr size_t kBars = 60;
  static constexpr uint32_t kBarMs = 120000;
  void clear() { *this = MonitorHistory(); }
  // The room: a frame at `ms` since it started.
  void note(uint32_t ms, float levelDb);
  // A bar as the room says it is. A newer bar forgets those more than kBars before it.
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

// A listener's view of the room it listens to: QUIET, SOUND, NO LINK (nothing from the room for
// kNoLinkMs: the alarm sounds until ack() or the link is back) or STOPPED (the room said so), and
// the night's events.
class ListenState {
 public:
  enum class Link : uint8_t { Off, Quiet, Sound, NoLink, Stopped };
  static constexpr uint32_t kNoLinkMs = 20000;
  static constexpr uint32_t kSoundEndMs = 600;  // no audio this long: the sound is over
  static constexpr int kLoudMarginDb = 12;      // LOUD: a frame this far over the threshold
  static constexpr size_t kMaxEvents = 48;      // the oldest goes

  struct Event {
    bool noLink = false;  // else a sound
    uint32_t startMs = 0, endMs = 0;  // a sound: its first and last frame; no link: the last
                                      // packet before, and the first after
    bool open = false;    // still going
    bool loud = false;
    float peakDb = -96;
    uint16_t sound = 0;   // the room's number for it
  };

  // Listening begins; the room was last heard at heardMs.
  void start(uint32_t now, uint32_t heardMs);
  void stop() { *this = ListenState(); }

  // Each returns true if anything a screen shows has changed.

  // A packet from the room: the link is up (back, after NO LINK).
  bool heard(uint32_t now);
  // A frame of the room's sound number `sound`, as loud as levelDb. becameLoud: this frame made
  // the sound LOUD (once a sound).
  bool audio(uint32_t now, uint16_t sound, float levelDb, bool &becameLoud);
  // The room stopped.
  bool off(uint32_t now);
  // NO LINK after kNoLinkMs without a packet; a sound over after kSoundEndMs without a frame.
  bool tick(uint32_t now);
  // The alarm stops; NO LINK stays until the link is back.
  bool ack();
  void setThreshold(int db) { threshold_ = db; }

  Link link() const { return link_; }
  bool on() const { return link_ == Link::Quiet || link_ == Link::Sound || link_ == Link::NoLink; }
  uint32_t sinceMs() const { return sinceMs_; }  // in this state since
  uint32_t heardMs() const { return heardMs_; }
  bool alarm() const { return link_ == Link::NoLink && !acked_; }
  int threshold() const { return threshold_; }
  const std::vector<Event> &events() const { return events_; }  // oldest first
  // The last sound, over or not: nullptr if none yet.
  const Event *lastSound() const;

 private:
  Event *openEvent(bool noLink);
  void push(const Event &e);
  Link link_ = Link::Off;
  uint32_t sinceMs_ = 0, heardMs_ = 0, soundMs_ = 0;
  bool acked_ = false;
  int threshold_ = -30;
  std::vector<Event> events_;
};

// --- Packets: type 15 (kExt) ------------------------------------------------------------------
// The 4-bit type ran out at 15, so type 15's body says its own type: sentAt (4), the ext type
// (1), then the fields. Each put*() writes a whole body and returns its size (0 if it doesn't
// fit); each read*() takes a Reader just past the ext type and is false for a short or bad body.

enum ExtType : uint8_t {
  kMonitorAudio = 1,  // 20 ms of the room's sound, to its listeners
  kStillHere = 2,     // the room is on: every 2 s
  kMonitorOff = 3,    // the room stopped
  kListening = 4,     // a listener says it listens to the room (or has stopped)
  kHistoryAsk = 5,    // a listener asks the room for its 2 hours of levels
  kHistory = 6,       // the room's levels, to the listener that asked
};
constexpr uint8_t kLastExt = kHistory;

constexpr uint8_t kStillSending = 0x01, kStillUsb = 0x02, kStillMicOff = 0x04;
constexpr uint8_t kOffStopped = 1, kOffPowerOff = 2;
constexpr uint8_t kListeningStopped = 0x01;

struct MonitorAudio {
  ListenerSet listeners;
  uint16_t sound = 0;  // the room's sound counter
  uint16_t frame = 0;  // within the sound, from 0
  uint8_t level = kNoLevel;  // the frame's own, as the room measured it
  int16_t predictor = 0;
  uint8_t index = 0;
  uint8_t bytes = 0;
  uint8_t data[kFrameBytes] = {};
};

struct StillHere {
  uint8_t flags = 0;    // kStill*
  uint8_t battery = 0xFF;  // percent, 0xFF none
  uint8_t threshold = 30;  // dB below full scale
  uint8_t peak = kNoLevel;  // the loudest frame since the last still-here
  uint32_t onS = 0;     // on for
  uint16_t bar = 0;     // the bar now (MonitorHistory)
  uint8_t barPeak = kNoLevel;
  uint8_t preFrames = MonitorGate::kPreFrames;
  ListenerSet listeners;
};

struct History {
  uint32_t to = 0;
  uint16_t first = 0;
  uint8_t count = 0;  // up to MonitorHistory::kBars
  uint8_t bars[MonitorHistory::kBars] = {};
};

size_t putMonitorAudio(uint8_t *body, size_t cap, uint32_t sentAt, const MonitorAudio &a);
bool readMonitorAudio(Reader &r, MonitorAudio &a);
size_t putStillHere(uint8_t *body, size_t cap, uint32_t sentAt, const StillHere &s);
bool readStillHere(Reader &r, StillHere &s);
size_t putMonitorOff(uint8_t *body, size_t cap, uint32_t sentAt, uint8_t reason);
// A listening packet (flags kListening*) or a history ask, to the room.
size_t putToRoom(uint8_t *body, size_t cap, uint32_t sentAt, uint8_t ext, uint32_t room,
                 uint8_t flags);
size_t putHistory(uint8_t *body, size_t cap, uint32_t sentAt, const History &h);
bool readHistory(Reader &r, History &h);
// The board a type 15 body is for alone (the relay rule): a listening packet's or a history
// ask's room, a history's listener, monitor audio with one listener; 0 for the others.
uint32_t extFor(const uint8_t *body, size_t len);

}  // namespace wire
}  // namespace comms
