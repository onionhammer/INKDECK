#include "comms_store.h"

#include "features.h"

#if INKDECK_COMMS  // the whole file (features.h)

#include <esp32-hal-log.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "cache_file.h"
#include "comms_wire.h"
#include "sdcard.h"

namespace comms {
namespace store {
namespace {

constexpr const char *kDir = "/sdcard/.inkdeck/comms";
constexpr const char *kIndex = "/sdcard/.inkdeck/comms/messages.tsv";
constexpr size_t kMaxAudio = 3000 * wire::kStoredFrame;  // 60 s

std::string audioPath(const Entry &e) {
  char name[32];
  snprintf(name, sizeof name, "/%08lx-%08lx.adp", static_cast<unsigned long>(e.sender),
           static_cast<unsigned long>(e.msg));
  return std::string(kDir) + name;
}

uint32_t hex(const std::string &s) { return static_cast<uint32_t>(strtoul(s.c_str(), nullptr, 16)); }
uint32_t dec(const std::string &s) { return static_cast<uint32_t>(strtoul(s.c_str(), nullptr, 10)); }

}  // namespace

bool available() { return sdcard::mounted(); }

// A line: sender, msg, group, to (hex), at, frames, sum (hex), complete, played, playedAt, name.
std::vector<Entry> load() {
  std::vector<Entry> out;
  if (!available()) return out;
  std::string data;
  if (!cachefile::read(kIndex, data, 64 * 1024)) return out;
  cachefile::forEachLine(data, [&out](const std::string &line) {
    const std::vector<std::string> f = cachefile::split(line);
    if (f.size() < 11) return;
    Entry e;
    e.sender = hex(f[0]);
    e.msg = hex(f[1]);
    e.group = hex(f[2]);
    e.to = hex(f[3]);
    e.at = static_cast<time_t>(dec(f[4]));
    e.frames = dec(f[5]);
    e.sum = hex(f[6]);
    e.complete = f[7] == "1";
    e.played = f[8] == "1";
    e.playedAt = static_cast<time_t>(dec(f[9]));
    e.fromName = f[10];
    struct stat st;
    if (!e.sender || stat(audioPath(e).c_str(), &st) != 0) return;
    out.push_back(e);
  });
  return out;
}

bool saveIndex(const std::vector<Entry> &entries) {
  if (!available()) return false;
  std::string data;
  char line[160];
  for (const Entry &e : entries) {
    snprintf(line, sizeof line, "%08lx\t%08lx\t%08lx\t%08lx\t%lu\t%lu\t%08lx\t%d\t%d\t%lu\t",
             static_cast<unsigned long>(e.sender), static_cast<unsigned long>(e.msg),
             static_cast<unsigned long>(e.group), static_cast<unsigned long>(e.to),
             static_cast<unsigned long>(e.at), static_cast<unsigned long>(e.frames),
             static_cast<unsigned long>(e.sum), e.complete ? 1 : 0, e.played ? 1 : 0,
             static_cast<unsigned long>(e.playedAt));
    data += line;
    data += cachefile::field(e.fromName);
    data += '\n';
  }
  return cachefile::writeWhole(kIndex, data);
}

bool saveAudio(const Entry &e, const std::vector<uint8_t> &stored) {
  if (!available()) return false;
  // From PSRAM, never flash (AGENTS.md, gotcha 7): a std::string's buffer is in PSRAM.
  return cachefile::writeWhole(audioPath(e),
                               std::string(reinterpret_cast<const char *>(stored.data()),
                                           stored.size()));
}

bool loadAudio(const Entry &e, std::vector<uint8_t> &stored) {
  stored.clear();
  if (!available()) return false;
  FILE *f = fopen(audioPath(e).c_str(), "rb");
  if (!f) return false;
  fseek(f, 0, SEEK_END);
  const long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  bool ok = n > 0 && static_cast<size_t>(n) <= kMaxAudio;
  if (ok) {
    stored.resize(static_cast<size_t>(n));
    ok = fread(stored.data(), 1, stored.size(), f) == stored.size();
  }
  fclose(f);
  if (!ok) {
    log_w("comms: can't read the message %s", audioPath(e).c_str());
    stored.clear();
  }
  return ok;
}

void removeAudio(const Entry &e) {
  if (available()) remove(audioPath(e).c_str());
}

}  // namespace store
}  // namespace comms

#endif  // INKDECK_COMMS
