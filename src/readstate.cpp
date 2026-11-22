#include "readstate.h"

#include <Preferences.h>
#include <stdio.h>
#include <string.h>

#include "dirlist.h"
#include "text.h"

namespace readstate {
namespace {

constexpr const char *kNamespace = "reader";
constexpr uint8_t kVersion = 1;

struct Record {  // stored as a blob; bump kVersion when the layout changes
  uint8_t version;
  uint8_t fontSize;
  uint8_t percent;
  uint8_t reserved;
  uint16_t chapter;
  uint16_t reserved2;
  uint32_t offset;
};

// NVS keys are at most 15 characters: "p" + 32-bit FNV-1a of the path in hex.
std::string keyFor(const std::string &path) {
  char key[12];
  snprintf(key, sizeof key, "p%08lx", static_cast<unsigned long>(text::fnv1a(path)));
  return key;
}

// Preferences logs an error for every missing key, so look before reading.
std::string getString(Preferences &prefs, const char *key) {
  return prefs.isKey(key) ? prefs.getString(key).c_str() : "";
}

bool read(Preferences &prefs, const std::string &path, Position &out) {
  const std::string key = keyFor(path);
  Record r;
  if (!prefs.isKey(key.c_str()) || prefs.getBytes(key.c_str(), &r, sizeof r) != sizeof r ||
      r.version != kVersion) {
    return false;
  }
  out.chapter = r.chapter;
  out.offset = r.offset;
  out.fontSize = r.fontSize;
  out.percent = r.percent;
  return true;
}

}  // namespace

bool load(const std::string &path, Position &out) {
  Preferences prefs;
  if (!prefs.begin(kNamespace, false)) return false;
  const bool ok = read(prefs, path, out);
  prefs.end();
  return ok;
}

void save(const std::string &path, const std::string &title, const Position &pos,
          bool asLast) {
  Preferences prefs;
  if (!prefs.begin(kNamespace, false)) return;
  Position old;
  if (!read(prefs, path, old) || old.chapter != pos.chapter || old.offset != pos.offset ||
      old.fontSize != pos.fontSize || old.percent != pos.percent) {
    const Record r{kVersion, pos.fontSize, pos.percent, 0, pos.chapter, 0, pos.offset};
    prefs.putBytes(keyFor(path).c_str(), &r, sizeof r);
  }
  if (asLast && getString(prefs, "last") != path) {
    prefs.putString("last", path.c_str());
    prefs.putString("lastTitle", title.c_str());
  }
  prefs.end();
}

bool last(std::string &path, std::string &title, Position &pos) {
  Preferences prefs;
  if (!prefs.begin(kNamespace, false)) return false;
  path = getString(prefs, "last");
  title = getString(prefs, "lastTitle");
  // Builds of the text reader before asLast recorded text files here too.
  const bool ok = DirList::isEpub(path) && read(prefs, path, pos);
  prefs.end();
  return ok;
}

}  // namespace readstate
