// Comms (issue #66): messages kept on the card, in SD:/.inkdeck/comms/. An index, messages.tsv
// (one line a message, written whole through a temporary file, cache_file.h), and the audio of
// each in <sender>-<message>.adp: its stored frames (comms_wire.h, kStoredFrame each), as they
// came. comms.cpp keeps messages here when the card is in, in PSRAM otherwise.
#pragma once
#include <stdint.h>
#include <time.h>

#include <string>
#include <vector>

namespace comms {
namespace store {

struct Entry {
  uint32_t sender = 0;  // the board that left it
  uint32_t msg = 0;     // its message id
  uint32_t group = 0;   // the group's id (not its index)
  uint32_t to = 0;      // 0: all of the group, else this board
  time_t at = 0;        // when it began, UTC; 0 if the clock wasn't set
  uint32_t frames = 0;  // 20 ms each
  uint32_t sum = 0;     // the checksum the sender gave (comms_calls.h, messageSum())
  bool complete = false;
  bool played = false;
  time_t playedAt = 0;  // UTC; 0 if not played, or the clock wasn't set
  std::string fromName;
};

bool available();  // the card is mounted
std::vector<Entry> load();  // the index; entries whose audio file is gone are left out
bool saveIndex(const std::vector<Entry> &entries);
bool saveAudio(const Entry &e, const std::vector<uint8_t> &stored);
bool loadAudio(const Entry &e, std::vector<uint8_t> &stored);
void removeAudio(const Entry &e);

}  // namespace store
}  // namespace comms
