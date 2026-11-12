// Reading positions, kept in NVS so they survive power-off: one per book (keyed by a hash of
// its path), plus which book was read last.
#pragma once
#include <stdint.h>

#include <string>

namespace readstate {

struct Position {
  uint16_t chapter = 0;  // spine index
  uint32_t offset = 0;   // flow offset of the page's first byte
  uint8_t fontSize = 1;  // index into fonts::kReader
  uint8_t percent = 0;   // whole-book progress, for Home's preview
};

bool load(const std::string &path, Position &out);
// Also records path as the last book read. Skips the flash write if nothing changed.
void save(const std::string &path, const std::string &title, const Position &pos);
// The book read last. False if there isn't one.
bool last(std::string &path, std::string &title, Position &pos);

}  // namespace readstate
