// Copies one file onto the card in the background: Files' HOLD ● SAVE on a network share
// (apps/files.h). One copy at a time, on a task of its own, so the screen can show how far it has
// got and BOOT can stop it.
//
// The data goes to dst + ".part", renamed to dst once all of it is there, so a copy cut short
// (stopped, a dropped connection, a power-off) never leaves a file that looks whole; the next
// copy to the same place starts that .part over. The open source file keeps its share mounted,
// and Wi-Fi up, until the copy ends (shares.h), and the board doesn't power off while one runs
// (power.cpp's working()).
#pragma once
#include <stdint.h>

#include <string>

namespace filecopy {

enum class State : uint8_t { Idle, Copying, Done, Failed, Stopped };

struct Status {
  State state = State::Idle;
  std::string src;
  std::string dst;
  int64_t done = 0;    // bytes written so far
  int64_t total = -1;  // the source's size; -1 unknown
  uint32_t elapsedMs = 0;  // since it started (until it ended)
  std::string error;   // Failed: a few words for the screen ("CONNECTION LOST", "CARD FULL")
};

// Starts copying src (any path the VFS opens: a share's file) to dst, an absolute path on the
// card, making the folders dst needs. size: the source's, for the free-space check and the
// progress (-1 if unknown). The caller picks a dst that's free. False, with a few words in error,
// if a copy is running already or its task can't start.
bool start(const std::string &src, const std::string &dst, int64_t size, std::string &error);

// The running copy, or how the last one ended.
Status status();
bool busy();  // a copy is running

// Stops the running copy within a read or two; its .part is deleted.
void cancel();

}  // namespace filecopy
