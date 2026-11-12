// One directory of the SD card as a sorted list: folders first, then files, case-insensitive.
// No drawing and no cursor; the Files screen (apps/files.cpp) owns those.
#pragma once
#include <stdint.h>

#include <string>
#include <vector>

class DirList {
 public:
  struct Entry {
    std::string name;   // raw (UTF-8) file name
    bool isDir;
    int64_t size = -1;  // bytes; -1 until sizeOf() looks it up (folders stay -1)
  };

  explicit DirList(std::string root) : root_(std::move(root)) {}

  // Lists dir. Returns false if it can't be read (no card, bad path); entries() is then empty.
  bool open(const std::string &dir);
  bool reload() { return open(cwd_); }
  // Opens the parent folder. False if already at the root.
  bool goUp();

  const std::vector<Entry> &entries() const { return entries_; }
  int indexOf(const std::string &name) const;  // -1 if absent
  const std::string &cwd() const { return cwd_; }
  bool atRoot() const { return cwd_.size() <= root_.size(); }
  bool readable() const { return readable_; }
  // cwd below the root, "" at the root ("/Books/Wells" -> "Books/Wells").
  std::string relativePath() const;
  std::string pathOf(int index) const { return cwd_ + "/" + entries_[index].name; }

  // File size, looked up with stat() on first use and cached.
  int64_t sizeOf(int index);

  // Paths of the files matching pred, in list order. selectedIndex = position of entry `index`
  // in that list, or -1.
  std::vector<std::string> filePaths(bool (*pred)(const std::string &), int index,
                                     int &selectedIndex) const;

  // The name of dir's subfolder matching `name` case-insensitively, or "" (FAT ignores case,
  // but the name we show should be the one on the card).
  static std::string findSubdir(const std::string &dir, const char *name);

  static bool isEpub(const std::string &name);
  static bool isMp3(const std::string &name);

 private:
  std::string root_;
  std::string cwd_;
  std::vector<Entry> entries_;
  bool readable_ = false;
};
