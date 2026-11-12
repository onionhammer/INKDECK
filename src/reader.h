// EPUB reader screen: one chapter (spine item) paginated in memory at a time.
#pragma once
#include <Adafruit_GFX.h>

#include <memory>
#include <string>
#include <vector>

#include "epub.h"
#include "typeset.h"

class Reader {
 public:
  bool open(const std::string &path, std::string &err);
  void close();

  void nextPage();
  void prevPage();
  void nextChapter();
  void prevChapter();  // to the start of this chapter first, then the previous one
  void cycleFontSize();

  void draw(GFXcanvas1 &canvas) const;

 private:
  void loadChapter(size_t index, bool atEnd);
  void makeTypesetter();

  Epub book_;
  size_t chapter_ = 0;
  std::string flow_;
  std::vector<PagePos> pages_;
  size_t page_ = 0;
  int fontSize_ = fonts::kDefaultReaderSize;
  std::unique_ptr<Typesetter> typesetter_;
};
