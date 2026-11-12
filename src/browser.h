// File browser over the SD card: one directory at a time, folders first, cursor + paging.
#pragma once
#include <Adafruit_GFX.h>

#include <string>
#include <vector>

class Browser {
 public:
  struct Entry {
    std::string name;  // raw (UTF-8) file name
    bool isDir;
  };

  explicit Browser(std::string root) : root_(std::move(root)) {}

  // Lists dir; puts the cursor on `select` if present. Returns false if unreadable.
  bool open(const std::string &dir, const std::string &select = "");
  bool reload() { return open(cwd_.empty() ? root_ : cwd_, selectedName()); }
  bool goUp();  // false if already at root

  void moveCursor(int delta);
  const Entry *selected() const;
  std::string selectedName() const;
  std::string selectedPath() const;
  static bool isEpub(const std::string &name);
  static bool isMp3(const std::string &name);

  // Full paths of the files in this folder matching pred, in list order. selectedIndex is the
  // position of the cursor's entry in that list (or -1).
  std::vector<std::string> filePaths(bool (*pred)(const std::string &), int &selectedIndex) const;

  void setMessage(std::string msg) { message_ = std::move(msg); }
  void clearMessage() { message_.clear(); }

  void draw(GFXcanvas1 &canvas) const;

 private:
  int rowsPerPage() const;

  std::string root_;
  std::string cwd_;
  std::vector<Entry> entries_;
  int cursor_ = 0;
  bool readable_ = false;
  std::string message_;
};
