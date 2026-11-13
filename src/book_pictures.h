// The Reader's pictures for one chapter (typeset.h's ImageSource): sizes from the file headers
// (read once per picture), fitted to the text column, rendered by picture.h when a page shows
// them and kept for the last few. Pictures it can't decode (GIF, SVG, broken files) take one
// line, with their alt text in brackets, or "[image]".
#pragma once
#include <string>
#include <vector>

#include "epub.h"
#include "picture.h"
#include "typeset.h"

class BookPictures : public ImageSource {
 public:
  // A new chapter's pictures. book must outlive this (it reads the files).
  void reset(const Epub *book, std::vector<Epub::Image> images);
  // The face for placeholders (the reader's italic at the current size).
  void setFont(const GFXfont *font) { font_ = font; }

  void layoutSize(int index, int maxW, int maxH, int &w, int &h) override;
  void draw(GFXcanvas1 &canvas, int index, int x, int y, int w, int h) override;

 private:
  struct Info {
    bool probed = false;
    bool ok = false;  // a format we decode, with a readable size
    int w = 0;
    int h = 0;
  };
  struct Cached {
    int index;
    picture::Bitmap bitmap;
  };
  static constexpr size_t kCacheSize = 3;  // bitmaps; a full-page one is about 36 KB

  const Info &probe(int index);
  void drawPlaceholder(GFXcanvas1 &canvas, int index, int x, int y, int w, int h) const;

  const Epub *book_ = nullptr;
  std::vector<Epub::Image> images_;
  std::vector<Info> info_;
  std::vector<Cached> cache_;  // least recently used first
  const GFXfont *font_ = nullptr;
};
