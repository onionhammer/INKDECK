#include "covers.h"

#include <Arduino.h>
#include <stdio.h>
#include <sys/stat.h>

#include "epub.h"
#include "sdcard.h"

namespace covers {
namespace {

constexpr uint32_t kMagic = 0x31425449;  // "ITB1" little-endian; bump the digit on format changes

struct Header {
  uint32_t magic;
  uint16_t w, h;  // 0 x 0: the book has no cover we can decode
};

// The last thumbnail, by book path and box.
std::string lastKey;
bool lastOk = false;
picture::Bitmap last;

// Cache file per book file (path and size, so a replaced book gets a new cover) and box.
std::string cacheKey(const std::string &bookPath, int maxW, int maxH) {
  struct stat st = {};
  stat(bookPath.c_str(), &st);
  uint32_t h = 2166136261u;  // FNV-1a
  for (const char c : bookPath) h = (h ^ static_cast<uint8_t>(c)) * 16777619u;
  char key[64];
  snprintf(key, sizeof key, "%08lx-%lx-%dx%d", static_cast<unsigned long>(h),
           static_cast<unsigned long>(st.st_size), maxW, maxH);
  return key;
}

std::string cacheDir() { return std::string(sdcard::kMountPoint) + "/.inkdeck/covers"; }

bool load(const std::string &file, bool &hasCover, picture::Bitmap &out) {
  FILE *f = fopen(file.c_str(), "rb");
  if (!f) return false;
  Header hdr;
  bool ok = fread(&hdr, sizeof hdr, 1, f) == 1 && hdr.magic == kMagic;
  if (ok) {
    hasCover = hdr.w && hdr.h;
    out.w = hdr.w;
    out.h = hdr.h;
    out.bits.resize(static_cast<size_t>((hdr.w + 7) / 8) * hdr.h);
    ok = out.bits.empty() || fread(out.bits.data(), out.bits.size(), 1, f) == 1;
  }
  fclose(f);
  return ok;
}

void store(const std::string &file, bool hasCover, const picture::Bitmap &bmp) {
  mkdir((std::string(sdcard::kMountPoint) + "/.inkdeck").c_str(), 0777);
  mkdir(cacheDir().c_str(), 0777);
  FILE *f = fopen(file.c_str(), "wb");
  if (!f) {
    log_w("covers: can't write %s", file.c_str());
    return;
  }
  const Header hdr{kMagic, static_cast<uint16_t>(hasCover ? bmp.w : 0),
                   static_cast<uint16_t>(hasCover ? bmp.h : 0)};
  fwrite(&hdr, sizeof hdr, 1, f);
  if (hasCover && !bmp.bits.empty()) fwrite(bmp.bits.data(), bmp.bits.size(), 1, f);
  fclose(f);
}

// From the EPUB: open it, find the cover, fit and dither it.
bool renderCover(const std::string &bookPath, int maxW, int maxH, picture::Bitmap &out) {
  const uint32_t t0 = millis();
  Epub book;
  std::string err, data;
  if (!book.open(bookPath, err)) return false;
  if (book.coverPath().empty() || !book.readFile(book.coverPath(), data, err)) return false;
  int w = 0, h = 0;
  if (!picture::size(data, w, h)) return false;
  picture::fit(w, h, maxW, maxH, w, h);
  if (!picture::render(data, w, h, out)) return false;
  log_i("covers: %s -> %dx%d in %lu ms", book.coverPath().c_str(), w, h,
        static_cast<unsigned long>(millis() - t0));
  return true;
}

}  // namespace

bool thumbnail(const std::string &bookPath, int maxW, int maxH, picture::Bitmap &out) {
  if (!sdcard::mounted() || bookPath.empty()) return false;
  // RAM first, by path and box (Home redraws often); the card's cache key also needs a stat().
  const std::string ramKey = bookPath + "|" + std::to_string(maxW) + "x" + std::to_string(maxH);
  if (ramKey != lastKey) {
    const std::string file = cacheDir() + "/" + cacheKey(bookPath, maxW, maxH) + ".bin";
    bool hasCover = false;
    picture::Bitmap bmp;
    if (!load(file, hasCover, bmp)) {
      hasCover = renderCover(bookPath, maxW, maxH, bmp);
      store(file, hasCover, bmp);
    }
    lastKey = ramKey;
    lastOk = hasCover;
    last = std::move(bmp);
  }
  if (lastOk) out = last;
  return lastOk;
}

}  // namespace covers
