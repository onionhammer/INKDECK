#include "zip.h"

#include <esp_heap_caps.h>
#include <miniz.h>  // ESP32-S3 ROM tinfl
#include <stdlib.h>
#include <strings.h>

#include <algorithm>
#include <memory>

namespace {

constexpr uint32_t kEocdSig = 0x06054b50;
constexpr uint32_t kCentralSig = 0x02014b50;
constexpr uint32_t kLocalSig = 0x04034b50;
constexpr size_t kEocdSize = 22;
constexpr size_t kCentralHeaderSize = 46;
constexpr size_t kLocalHeaderSize = 30;

uint16_t le16(const uint8_t *p) { return p[0] | (p[1] << 8); }
uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24); }

bool readAt(FILE *f, long offset, void *buf, size_t len) {
  return fseek(f, offset, SEEK_SET) == 0 && fread(buf, 1, len, f) == len;
}

}  // namespace

bool ZipFile::open(const std::string &path, std::string &err) {
  close();
  file_ = fopen(path.c_str(), "rb");
  if (!file_) {
    err = "can't open file";
    return false;
  }
  fseek(file_, 0, SEEK_END);
  const long fileSize = ftell(file_);
  if (fileSize < static_cast<long>(kEocdSize)) {
    err = "not a zip (too small)";
    return false;
  }

  // The end-of-central-directory record sits in the last 22 + up to 64K (comment) bytes.
  const size_t tailLen = static_cast<size_t>(std::min<long>(fileSize, kEocdSize + 0xFFFF));
  std::string tail(tailLen, '\0');
  if (!readAt(file_, fileSize - tailLen, &tail[0], tailLen)) {
    err = "read error";
    return false;
  }
  const auto *t = reinterpret_cast<const uint8_t *>(tail.data());
  long eocd = -1;
  for (long i = static_cast<long>(tailLen - kEocdSize); i >= 0; --i) {
    if (le32(t + i) == kEocdSig) {
      eocd = i;
      break;
    }
  }
  if (eocd < 0) {
    err = "not a zip (no end record)";
    return false;
  }
  const uint16_t count = le16(t + eocd + 10);
  const uint32_t cdSize = le32(t + eocd + 12);
  const uint32_t cdOffset = le32(t + eocd + 16);
  if (count == 0xFFFF || cdOffset == 0xFFFFFFFF) {
    err = "ZIP64 not supported";
    return false;
  }

  std::string cd(cdSize, '\0');
  if (!readAt(file_, cdOffset, &cd[0], cdSize)) {
    err = "can't read zip directory";
    return false;
  }
  const auto *p = reinterpret_cast<const uint8_t *>(cd.data());
  size_t pos = 0;
  entries_.reserve(count);
  for (uint16_t n = 0; n < count; ++n) {
    if (pos + kCentralHeaderSize > cdSize || le32(p + pos) != kCentralSig) {
      err = "corrupt zip directory";
      return false;
    }
    const uint8_t *h = p + pos;
    const uint16_t nameLen = le16(h + 28);
    const uint16_t extraLen = le16(h + 30);
    const uint16_t commentLen = le16(h + 32);
    if (pos + kCentralHeaderSize + nameLen > cdSize) {
      err = "corrupt zip directory";
      return false;
    }
    Entry e;
    e.method = le16(h + 10);
    e.compressedSize = le32(h + 20);
    e.size = le32(h + 24);
    e.localHeaderOffset = le32(h + 42);
    e.name.assign(reinterpret_cast<const char *>(h + kCentralHeaderSize), nameLen);
    entries_.push_back(std::move(e));
    pos += kCentralHeaderSize + nameLen + extraLen + commentLen;
  }
  return true;
}

void ZipFile::close() {
  if (file_) fclose(file_);
  file_ = nullptr;
  entries_.clear();
}

int ZipFile::find(const std::string &name) const {
  for (size_t i = 0; i < entries_.size(); ++i) {
    if (entries_[i].name == name) return static_cast<int>(i);
  }
  for (size_t i = 0; i < entries_.size(); ++i) {
    if (strcasecmp(entries_[i].name.c_str(), name.c_str()) == 0) return static_cast<int>(i);
  }
  return -1;
}

bool ZipFile::read(const std::string &name, std::string &out, std::string &err) const {
  const int index = find(name);
  if (index < 0) {
    err = "missing " + name;
    return false;
  }
  return read(index, out, err);
}

bool ZipFile::read(int index, std::string &out, std::string &err) const {
  out.clear();
  if (!file_ || index < 0 || index >= static_cast<int>(entries_.size())) {
    err = "bad entry";
    return false;
  }
  const Entry &e = entries_[index];
  if (e.size > kMaxEntrySize || e.compressedSize > kMaxEntrySize) {
    err = e.name + " too large";
    return false;
  }

  uint8_t lh[kLocalHeaderSize];
  if (!readAt(file_, e.localHeaderOffset, lh, sizeof lh) || le32(lh) != kLocalSig) {
    err = "bad local header for " + e.name;
    return false;
  }
  const long dataOffset = e.localHeaderOffset + kLocalHeaderSize + le16(lh + 26) + le16(lh + 28);
  if (e.size == 0) return true;

  if (e.method == 0) {
    out.resize(e.size);
    if (!readAt(file_, dataOffset, &out[0], e.size)) {
      err = "read error";
      return false;
    }
    return true;
  }
  if (e.method != 8) {
    err = "unsupported compression in " + e.name;
    return false;
  }

  std::string compressed(e.compressedSize, '\0');
  if (!readAt(file_, dataOffset, &compressed[0], e.compressedSize)) {
    err = "read error";
    return false;
  }
  // ~11 KB of decoder state: keep it off the task stack, prefer internal RAM for speed.
  void *mem = heap_caps_malloc(sizeof(tinfl_decompressor), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!mem) mem = malloc(sizeof(tinfl_decompressor));
  std::unique_ptr<tinfl_decompressor, void (*)(void *)> inflater(
      static_cast<tinfl_decompressor *>(mem), free);
  if (!inflater) {
    err = "out of memory";
    return false;
  }
  tinfl_init(inflater.get());

  out.resize(e.size);
  size_t inLen = compressed.size();
  size_t outLen = out.size();
  auto *outBuf = reinterpret_cast<mz_uint8 *>(&out[0]);
  const tinfl_status status =
      tinfl_decompress(inflater.get(), reinterpret_cast<const mz_uint8 *>(compressed.data()),
                       &inLen, outBuf, outBuf, &outLen, TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
  if (status != TINFL_STATUS_DONE || outLen != e.size) {
    err = "inflate failed for " + e.name;
    out.clear();
    return false;
  }
  return true;
}
