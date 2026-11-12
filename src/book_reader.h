// EPUB reader model: one chapter (spine item) paginated in memory at a time, the reading
// position (saved per book in NVS, see readstate.h), and page drawing. The Reader screen
// (apps/reader.cpp) owns the chrome around it.
#pragma once
#include <Adafruit_GFX.h>

#include <memory>
#include <string>
#include <vector>

#include "epub.h"
#include "typeset.h"

class Reader {
 public:
  // Text area size in px. Pages are laid out for it.
  Reader(int textWidth, int textHeight) : width_(textWidth), height_(textHeight) {}
  ~Reader() { close(); }

  // Opens the book at its saved position (or the start).
  bool open(const std::string &path, std::string &err);
  void close();  // saves the position
  void save();   // persists the position now

  // Each returns false if there's nowhere to go (start or end of the book).
  bool nextPage();
  bool prevPage();
  void goToChapter(size_t index);
  void cycleFontSize();

  // Draws the current page with the text area's top-left at (x, y). False if the page has
  // nothing to draw (an empty section).
  bool drawPage(GFXcanvas1 &c, int x, int y) const;

  const Epub &book() const { return book_; }
  const std::string &path() const { return path_; }
  size_t chapter() const { return chapter_; }
  size_t page() const { return page_; }
  size_t pageCount() const { return pages_.size(); }
  int fontSize() const { return fontSize_; }
  float progress() const;  // 0..1 through the whole book, weighted by chapter size

 private:
  void loadChapter(size_t index, bool atEnd);
  void layoutChapter();  // drop cap + pagination for the current flow and typesetter
  void makeTypesetter();
  void moved();  // after a page turn: save now and then

  int width_;
  int height_;
  std::string path_;
  Epub book_;
  size_t chapter_ = 0;
  std::string flow_;
  std::vector<PagePos> pages_;
  size_t page_ = 0;
  int fontSize_ = 1;
  std::unique_ptr<Typesetter> typesetter_;
  int movesSinceSave_ = 0;
};
