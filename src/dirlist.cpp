#include "dirlist.h"

#include <dirent.h>
#include <esp32-hal-log.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include <algorithm>

namespace {

constexpr size_t kMaxEntries = 1000;

bool endsWithNoCase(const std::string &s, const char *suffix) {
  const size_t n = strlen(suffix);
  return s.size() >= n && strcasecmp(s.c_str() + s.size() - n, suffix) == 0;
}

}  // namespace

bool DirList::isEpub(const std::string &name) { return endsWithNoCase(name, ".epub"); }
bool DirList::isMp3(const std::string &name) { return endsWithNoCase(name, ".mp3"); }
bool DirList::isAudio(const std::string &name) {
  return isMp3(name) || endsWithNoCase(name, ".wav");
}

bool DirList::open(const std::string &dir) {
  cwd_ = dir;
  entries_.clear();
  DIR *d = opendir(dir.c_str());
  readable_ = d != nullptr;
  if (!d) {
    log_w("dirlist: can't open %s", dir.c_str());
    return false;
  }
  while (dirent *e = readdir(d)) {
    const std::string name = e->d_name;
    if (name.empty() || name[0] == '.' || name == "System Volume Information") continue;
    entries_.push_back({name, e->d_type == DT_DIR});
    if (entries_.size() >= kMaxEntries) break;
  }
  closedir(d);
  std::sort(entries_.begin(), entries_.end(), [](const Entry &a, const Entry &b) {
    if (a.isDir != b.isDir) return a.isDir;
    return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
  });
  log_i("dirlist: %s (%u entries)", dir.c_str(), static_cast<unsigned>(entries_.size()));
  return true;
}

bool DirList::goUp() {
  if (atRoot()) return false;
  const size_t slash = cwd_.rfind('/');
  open(slash <= root_.size() ? root_ : cwd_.substr(0, slash));
  return true;
}

int DirList::indexOf(const std::string &name) const {
  for (size_t i = 0; i < entries_.size(); ++i) {
    if (entries_[i].name == name) return static_cast<int>(i);
  }
  return -1;
}

std::string DirList::relativePath() const {
  return atRoot() ? std::string() : cwd_.substr(root_.size() + 1);
}

int64_t DirList::sizeOf(int index) {
  Entry &e = entries_[index];
  if (!e.isDir && e.size < 0) {
    struct stat st;
    e.size = stat(pathOf(index).c_str(), &st) == 0 ? st.st_size : 0;
  }
  return e.size;
}

std::vector<std::string> DirList::filePaths(bool (*pred)(const std::string &), int index,
                                            int &selectedIndex) const {
  std::vector<std::string> paths;
  selectedIndex = -1;
  for (size_t i = 0; i < entries_.size(); ++i) {
    if (entries_[i].isDir || !pred(entries_[i].name)) continue;
    if (static_cast<int>(i) == index) selectedIndex = static_cast<int>(paths.size());
    paths.push_back(cwd_ + "/" + entries_[i].name);
  }
  return paths;
}

std::string DirList::findSubdir(const std::string &dir, const char *name) {
  std::string found;
  DIR *d = opendir(dir.c_str());
  if (!d) return found;
  while (dirent *e = readdir(d)) {
    if (e->d_type == DT_DIR && strcasecmp(e->d_name, name) == 0) {
      found = e->d_name;
      break;
    }
  }
  closedir(d);
  return found;
}
