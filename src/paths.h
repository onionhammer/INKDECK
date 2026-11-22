// Pieces of a path ("/sdcard/Books/Dune.epub"), for the screens and libraries that show and sort
// files. Plain string work: nothing here touches a file system.
#pragma once
#include <string.h>
#include <strings.h>

#include <string>

namespace paths {

// "Dune.epub": what follows the last '/' (all of a path without one).
inline std::string nameOf(const std::string &path) { return path.substr(path.rfind('/') + 1); }

// "/sdcard/Books": what comes before the last '/' (all of a path without one).
inline std::string folderOf(const std::string &path) { return path.substr(0, path.rfind('/')); }

// "epub", without the dot; "" for a name without one, or a dot file (".hidden").
inline std::string extensionOf(const std::string &path) {
  const std::string name = nameOf(path);
  const size_t dot = name.rfind('.');
  return dot == std::string::npos || dot == 0 ? std::string() : name.substr(dot + 1);
}

// "Dune": the name without its extension (a dot file keeps its whole name).
inline std::string stem(const std::string &path) {
  const std::string name = nameOf(path);
  const size_t dot = name.rfind('.');
  return dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);
}

// s ends in suffix (".epub"), case aside, with something before it.
inline bool hasSuffix(const std::string &s, const char *suffix) {
  const size_t n = strlen(suffix);
  return s.size() > n && strcasecmp(s.c_str() + s.size() - n, suffix) == 0;
}

}  // namespace paths
