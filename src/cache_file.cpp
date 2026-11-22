#include "cache_file.h"

#include <esp32-hal-log.h>
#include <stdio.h>
#include <sys/stat.h>

#include "paths.h"
#include "sdcard.h"
#include "text.h"

namespace cachefile {

std::string hexKey(const std::string &s) {
  char buf[12];
  snprintf(buf, sizeof buf, "%08lx", static_cast<unsigned long>(text::fnv1a(s)));
  return buf;
}

std::string field(const std::string &s) {
  std::string out = s;
  for (char &c : out) {
    if (c == '\t' || c == '\n' || c == '\r') c = ' ';
  }
  return out;
}

std::vector<std::string> split(const std::string &line, char sep) {
  std::vector<std::string> out;
  size_t at = 0;
  for (;;) {
    const size_t next = line.find(sep, at);
    out.push_back(line.substr(at, next == std::string::npos ? std::string::npos : next - at));
    if (next == std::string::npos) return out;
    at = next + 1;
  }
}

bool read(const std::string &path, std::string &out, size_t maxBytes) {
  FILE *f = fopen(path.c_str(), "rb");
  if (!f) return false;
  fseek(f, 0, SEEK_END);
  const long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  bool ok = n >= 0 && static_cast<size_t>(n) <= maxBytes;
  if (ok) {
    out.resize(static_cast<size_t>(n));
    ok = n == 0 || fread(&out[0], 1, n, f) == static_cast<size_t>(n);
  }
  fclose(f);
  return ok;
}

void ensureDir(const char *dir) {
  mkdir((std::string(sdcard::kMountPoint) + "/.inkdeck").c_str(), 0777);
  mkdir(dir, 0777);
}

bool writeWhole(const std::string &path, const std::string &data) {
  ensureDir(paths::folderOf(path).c_str());
  const std::string tmp = path + ".tmp";
  FILE *f = fopen(tmp.c_str(), "wb");
  if (!f) {
    log_w("cache: can't write %s", tmp.c_str());
    return false;
  }
  const bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
  if (fclose(f) != 0 || !ok) {
    log_w("cache: writing %s failed", tmp.c_str());
    remove(tmp.c_str());
    return false;
  }
  remove(path.c_str());  // FAT's rename won't replace a file
  return rename(tmp.c_str(), path.c_str()) == 0;
}

}  // namespace cachefile
