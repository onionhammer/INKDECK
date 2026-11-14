// Reading positions, kept in NVS so they survive power-off: one per book or text file (keyed by
// a hash of its path), plus which book (EPUB) was read last.
#pragma once
#include <stdint.h>

#include <string>

namespace readstate {

struct Position {
  uint16_t chapter = 0;  // spine index; for a text file (apps/textview.h), bit 0 = monospace
  uint32_t offset = 0;   // flow offset of the page's (text file: top line's) first byte
  uint8_t fontSize = 1;  // index into fonts::kReader
  uint8_t percent = 0;   // whole-book progress, for Home's preview
};

bool load(const std::string &path, Position &out);
// Also records path as the last book read, unless asLast is false (a text file: Home's READER
// and the sleep screen are about books). Skips the flash write if nothing changed.
void save(const std::string &path, const std::string &title, const Position &pos,
          bool asLast = true);
// The book read last. False if there isn't one (or what's recorded isn't an EPUB).
bool last(std::string &path, std::string &title, Position &pos);

}  // namespace readstate
