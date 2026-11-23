// Comms, reachable on battery (issue #66, phase 7): a board on battery that would power off stays
// in light sleep with its radio off instead, wakes every few seconds (mesh_reachable), asks
// "anything for me?" (a poll) and listens a few milliseconds. A board that is awake and knows of
// something for it (a ring, voice it would hear, a message) answers, and the sleeper stays awake
// for it. docs/comms-protocol.md has the packets; comms.cpp sends them and owns the radio, and
// power.cpp decides when a board sleeps this way.
//
// Plain C++ with no Arduino or ESP-IDF calls, so the self-test (comms::selfTest()) drives it with
// a made-up clock. Times are millis() of the board that keeps them.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "comms_wire.h"

namespace comms {
namespace wire {

// --- The packets: type 15's ext types 8 and 9 (3 to 7 are the long-range rate's) ----------------

constexpr uint8_t kExtPoll = 8;    // a sleeping board asks whether anything is waiting for it
constexpr uint8_t kExtAnswer = 9;  // an awake board says what is

// A poll's flags.
constexpr uint8_t kPollEcho = 0x01;   // answer even with nothing (boards on USB): the channel check
constexpr uint8_t kPollSweep = 0x02;  // one of a sweep of the channels (an echo too)

// An answer's what: what is waiting.
constexpr uint8_t kWaitRing = 0x01;     // a ring that includes the sleeper is on
constexpr uint8_t kWaitVoice = 0x02;    // voice it would hear sounds now
constexpr uint8_t kWaitMessage = 0x04;  // a message for it is kept, to be asked for

struct Poll {
  uint8_t flags = 0;
  uint8_t battery = 0xFF;  // percent, 0xFF none
  uint16_t intervalS = 0;  // seconds between its polls
  uint8_t nameLen = 0;
  char name[16] = {};      // ASCII, upper case, not terminated
};
// sentAt (4), ext (1), to (4: always 0, all; the relay rule's field), flags (1), battery (1),
// interval (2), name length (1), name: 57 bytes on the air with a 7-letter name.
size_t putPoll(uint8_t *body, size_t cap, uint32_t sentAt, const Poll &p);
// From after the ext type.
bool readPoll(Reader &r, Poll &p);

struct Answer {
  uint32_t to = 0;      // the sleeper
  uint8_t what = 0;     // kWait*; 0: nothing (an echo's answer)
  uint8_t channel = 0;  // the answering board's radio channel
  uint8_t flags = 0;    // its presence flags (kFlagUsb...)
  // kWaitRing: the ring.
  uint32_t dialer = 0;
  uint16_t ring = 0;
  // kWaitMessage: the message (its sender and id, whom it's to, how many frames).
  uint32_t msgSender = 0;
  uint32_t msg = 0;
  uint32_t msgTo = 0;
  uint16_t frames = 0;
};
// sentAt (4), ext (1), to (4), what (1), channel (1), flags (1), dialer (4), ring (2), message
// sender (4), message (4), message to (4), frames (2): 32 bytes, 68 on the air.
size_t putAnswer(uint8_t *body, size_t cap, uint32_t sentAt, const Answer &a);
bool readAnswer(Reader &r, Answer &a);

// The board a type 15 body of these is for alone (the relay rule): an answer's sleeper; 0 for a
// poll and for ext types that aren't these. Both go with ttl 0 anyway: the sleeper listens a few
// milliseconds, for the boards in range.
uint32_t reachExtFor(const uint8_t *body, size_t len);

// --- The sleeper: when to poll, on which channel, and the channel check -------------------------

// Counted in polls, not milliseconds: the board restarts for the wakes its off face needs (the
// clock's minute, a check-in), and this state is kept across those restarts (RTC memory).
struct SleepPlan {
  uint8_t channel = 0;     // the channel the polls go out on (1 to 13)
  uint8_t misses = 0;      // echo polls in a row nobody answered
  uint32_t polls = 0;      // since the plan began
  uint32_t nextEcho = 0;   // the poll number that asks boards on USB to answer even with nothing
  uint32_t nextSweep = 0;  // a sweep isn't tried before this poll number (0: any time)
};

class Sleeper {
 public:
  static constexpr uint32_t kEchoEveryS = 180;     // the channel check
  static constexpr uint8_t kMissesToSweep = 3;     // echoes unanswered in a row: sweep
  static constexpr uint32_t kSweepAgainS = 1800;   // a sweep that found nobody waits this long

  // A fresh plan on `channel` (the one the boards were last heard on).
  static void begin(SleepPlan &p, uint8_t channel, uint16_t intervalS);

  struct Next {
    uint8_t flags = 0;  // kPollEcho for the channel check
    bool sweep = false; // poll every channel instead (each with kPollSweep | kPollEcho)
  };
  static Next next(const SleepPlan &p, uint16_t intervalS);
  // What the poll got: an answer (any, echo's included) or none.
  static void result(SleepPlan &p, uint16_t intervalS, const Next &n, bool answered);
  // The sweep's result: the channel that answered, or 0 for none.
  static void sweepResult(SleepPlan &p, uint16_t intervalS, uint8_t channel);
  static uint32_t pollsFor(uint32_t seconds, uint16_t intervalS);
};

// --- The sleeper woken: how long it stays up ------------------------------------------------------

// After an answer woke the board, it listens (comms's task, as if held) and rings or plays
// through the usual paths. With no key it goes back to sleep kAfterMs after the ring, the sound
// or the transfer ends (kQuietMicMs after a sound while the mic that made it stays on: an open
// mic in a quiet room must not keep a sleeping board up all night), or kNothingMs after the wake
// if nothing came at all. A key makes it an ordinary board that's on.
class WokenHold {
 public:
  static constexpr uint32_t kAfterMs = 30000;
  static constexpr uint32_t kQuietMicMs = 60000;
  static constexpr uint32_t kNothingMs = 15000;

  enum class Verdict : uint8_t { Stay, Sleep, On };

  struct Seen {
    bool ringing = false;   // a ring rings here
    bool sound = false;     // voice plays here (a mic heard, PLAY LAST or a message)
    bool micOn = false;     // a mic heard here is still on (quiet or not)
    bool transfer = false;  // a message comes in
    bool keyed = false;     // a key since the wake
  };

  void begin(uint32_t now, uint8_t what);
  bool active() const { return active_; }
  uint8_t what() const { return what_; }
  uint32_t since() const { return wakeMs_; }
  // Each turn (a second or so): what the board is doing; the verdict from now on.
  Verdict step(uint32_t now, const Seen &s);
  void end() { active_ = false; }

 private:
  bool active_ = false;
  uint8_t what_ = 0;
  uint32_t wakeMs_ = 0;
  uint32_t ringMs_ = 0, soundMs_ = 0, transferMs_ = 0;  // last seen; 0 never
  bool micAfterSound_ = false;
};

// --- The awake side: rings seen, and what to answer ----------------------------------------------

// Rings on in this board's groups as their packets go by (rings flood, so every board that
// listens knows), to answer a sleeper's poll for one that includes it. A ring is over at its
// answer or end, or kLostMs after its last packet.
class RingWatch {
 public:
  static constexpr size_t kRings = 4;
  static constexpr uint32_t kLostMs = 3500;
  static constexpr uint32_t kRingMs = 60000;  // a dialer rings this long at most

  struct Seen {
    int group = -1;
    uint32_t dialer = 0;
    uint16_t ring = 0;
    uint32_t target = 0;  // 0: all of the group
    uint32_t lastMs = 0;
    uint32_t startMs = 0;
  };

  // A ring packet: `elapsed` is its rung (ms since the ring began).
  void heard(int group, uint32_t dialer, uint16_t ring, uint32_t target, uint32_t elapsed,
             uint32_t now);
  void over(uint32_t dialer, uint16_t ring);
  // A ring on in `group` that includes `board` (a ring to all, or to it). False if none.
  bool ringFor(int group, uint32_t board, uint32_t now, Seen &out) const;
  void clear();

 private:
  Seen rings_[kRings];
};

// What a board knows that a sleeper's poll could want, gathered by comms.cpp's task.
struct WaitingNow {
  static constexpr size_t kMics = 13;  // this board's own and the mics it hears

  // A ring on that includes the sleeper (RingWatch, or this board's own ring).
  bool ringOn = false;
  uint32_t dialer = 0;
  uint16_t ringNo = 0;

  struct MicNow {
    int group = -1;
    uint32_t target = 0;   // 0: all of the group
    uint32_t board = 0;    // whose mic
    bool opened = false;   // its session opened the conversation (the walkie-talkie)
    uint32_t soundMs = 0;  // its last sound (0: none yet)
  };
  MicNow mics[kMics];
  size_t micCount = 0;

  // A message kept to be asked for: this board's own (the sender keeps it 5 minutes).
  bool messageOn = false;
  int msgGroup = -1;
  uint32_t msgSender = 0, msg = 0, msgTo = 0;
  uint16_t frames = 0;
  uint32_t deliveredTo[8] = {};  // boards that said they have it whole
  size_t delivered = 0;
};

// The answer to a poll from `sleeper` in `group`, polling every intervalS: what's waiting
// (kWait*, 0 for nothing). Voice counts for a conversation the sleeper would be in, awake: a mic
// to it alone, or to all of the group whose session opened the conversation (every board that
// hears it joins); and only if it made a sound within the interval plus kVoiceGraceMs.
constexpr uint32_t kVoiceGraceMs = 2000;
uint8_t waitingFor(const WaitingNow &w, int group, uint32_t sleeper, uint16_t intervalS,
                   uint32_t now);

}  // namespace wire
}  // namespace comms
