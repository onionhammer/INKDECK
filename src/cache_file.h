// The card's cache files (SD:/.inkdeck/...): Podcasts, News and the Reader's shelf keep what
// they know in text files of tab-separated fields, one record a line, written whole through a
// temporary file so a power cut never leaves half of one.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

namespace cachefile {

// A file name for s (a feed's URL): its FNV-1a in hex, "0a1b2c3d".
std::string hexKey(const std::string &s);

// s as a field: tabs and line breaks (which would split the record) become spaces.
std::string field(const std::string &s);

// The fields of a record (or the lines of a file, with '\n').
std::vector<std::string> split(const std::string &line, char sep = '\t');

// The whole file into out; false if it can't be read, or is over maxBytes.
bool read(const std::string &path, std::string &out, size_t maxBytes = SIZE_MAX);

// Calls fn(line) for each line of data, without its "\r\n" or "\n".
template <typename Fn>
void forEachLine(const std::string &data, Fn fn) {
  size_t pos = 0;
  while (pos < data.size()) {
    size_t nl = data.find('\n', pos);
    if (nl == std::string::npos) nl = data.size();
    std::string line = data.substr(pos, nl - pos);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    fn(line);
    pos = nl + 1;
  }
}

// Makes SD:/.inkdeck and dir in it (a full path under it), if they're missing.
void ensureDir(const char *dir);

// Writes data to path through path + ".tmp", so the file is either the old one or the new
// one, whole; makes its folder first. False (and a log line) if it failed.
bool writeWhole(const std::string &path, const std::string &data);

}  // namespace cachefile
