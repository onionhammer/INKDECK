// Minimal read-only ZIP reader (enough for EPUB): central directory, stored + deflate entries.
// No ZIP64, no encryption. Entries are inflated whole into memory (PSRAM via malloc).
#pragma once
#include <stdint.h>
#include <stdio.h>

#include <string>
#include <vector>

class ZipFile {
 public:
  struct Entry {
    std::string name;
    uint32_t compressedSize;
    uint32_t size;
    uint32_t localHeaderOffset;
    uint16_t method;  // 0 = stored, 8 = deflate
  };

  ZipFile() = default;
  ZipFile(const ZipFile &) = delete;
  ZipFile &operator=(const ZipFile &) = delete;
  ~ZipFile() { close(); }

  bool open(const std::string &path, std::string &err);
  void close();

  // Exact match first, then case-insensitive. Returns -1 if missing.
  int find(const std::string &name) const;
  bool read(int index, std::string &out, std::string &err) const;
  bool read(const std::string &name, std::string &out, std::string &err) const;

  const std::vector<Entry> &entries() const { return entries_; }

  static constexpr uint32_t kMaxEntrySize = 4 * 1024 * 1024;

 private:
  FILE *file_ = nullptr;
  std::vector<Entry> entries_;
};
