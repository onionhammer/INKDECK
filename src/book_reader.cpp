#include "book_reader.h"

#include <Arduino.h>

#include "fonts.h"
#include "readstate.h"
#include "text.h"

namespace {

constexpr int kSaveEveryMoves = 10;  // NVS write at most every 10 page turns (and on leaving)

}  // namespace

bool Reader::open(const std::string &path, std::string &err) {
  close();
  const uint32_t t0 = millis();
  if (!book_.open(path, err)) return false;
  path_ = path;

  readstate::Position pos;
  const bool resumed = readstate::load(path, pos);
  fontSize_ = resumed && pos.fontSize < fonts::kReaderSizeCount ? pos.fontSize
                                                                 : fonts::kDefaultReaderSize;
  makeTypesetter();
  loadChapter(resumed && pos.chapter < book_.chapterCount() ? pos.chapter : 0, false);
  if (resumed) {
    while (page_ + 1 < pages_.size() && pages_[page_ + 1].offset <= pos.offset) ++page_;
  }
  log_i("reader: opened in %lu ms at chapter %u page %u%s; heap %u KB, PSRAM %u KB free",
        (unsigned long)(millis() - t0), static_cast<unsigned>(chapter_ + 1),
        static_cast<unsigned>(page_ + 1), resumed ? " (resumed)" : "",
        ESP.getFreeHeap() / 1024, ESP.getFreePsram() / 1024);
  return true;
}

void Reader::close() {
  if (path_.empty()) return;
  save();
  book_.close();
  path_.clear();
  flow_.clear();
  flow_.shrink_to_fit();
  pages_.clear();
  chapter_ = page_ = 0;
}

void Reader::save() {
  if (path_.empty() || pages_.empty()) return;
  readstate::Position pos;
  pos.chapter = static_cast<uint16_t>(chapter_);
  pos.offset = pages_[page_].offset;
  pos.fontSize = static_cast<uint8_t>(fontSize_);
  pos.percent = static_cast<uint8_t>(progress() * 100 + 0.5f);
  readstate::save(path_, book_.title(), pos);
  movesSinceSave_ = 0;
}

void Reader::moved() {
  if (++movesSinceSave_ >= kSaveEveryMoves) save();
}

void Reader::makeTypesetter() {
  typesetter_.reset(new Typesetter(fonts::kReader[fontSize_], width_, height_));
  typesetter_->setImages(&pictures_);
  pictures_.setFont(fonts::kReader[fontSize_].face[flow::kItalic]);
}

void Reader::loadChapter(size_t index, bool atEnd) {
  const uint32_t t0 = millis();
  chapter_ = index;
  std::string err;
  std::vector<Epub::Image> images;
  if (!book_.chapterFlow(index, flow_, images, err)) {
    flow_ = "Couldn't load this chapter: " + err;
  }
  if (index == 0) addCoverPage(images);
  const size_t pictureCount = images.size();
  pictures_.reset(&book_, std::move(images));
  const uint32_t t1 = millis();
  layoutChapter();
  page_ = atEnd ? pages_.size() - 1 : 0;
  log_i("reader: chapter %u/%u %s: %u bytes of text, %u pictures, %u pages (load %lu ms, "
        "layout %lu ms)",
        static_cast<unsigned>(index + 1), static_cast<unsigned>(book_.chapterCount()),
        book_.chapterPath(index).c_str(), static_cast<unsigned>(flow_.size()),
        static_cast<unsigned>(pictureCount), static_cast<unsigned>(pages_.size()),
        (unsigned long)(t1 - t0),
        (unsigned long)(millis() - t1));
}

void Reader::addCoverPage(std::vector<Epub::Image> &images) {
  const std::string &cover = book_.coverPath();
  if (cover.empty() || static_cast<int>(images.size()) >= flow::kMaxImages) return;
  for (const auto &image : images) {
    if (image.path == cover) return;  // the first chapter shows it already
  }
  // A first chapter that is a picture and little text is a cover page of its own (some books
  // use a different file there than the one the metadata names).
  size_t letters = 0;
  for (const char ch : flow_) letters += flow::isGlyph(ch) ? 1 : 0;
  if (!images.empty() && letters < 200 + 2 * images.size()) return;
  images.push_back({cover, ""});
  std::string page;
  page += flow::kImage;
  page += static_cast<char>(flow::kImageBase + images.size() - 1);
  page += flow::kPara;
  page += flow::kPageBreak;
  page += flow::kPara;
  flow_.insert(0, page);
  log_i("reader: added a cover page (%s)", cover.c_str());
}

void Reader::layoutChapter() {
  DropCap drop;
  drop.at = Typesetter::findDropCap(flow_);
  drop.font = fonts::kDropCap;
  typesetter_->setDropCap(drop);
  pages_ = typesetter_->paginate(flow_);
}

bool Reader::nextPage() {
  if (page_ + 1 < pages_.size()) {
    ++page_;
  } else if (chapter_ + 1 < book_.chapterCount()) {
    loadChapter(chapter_ + 1, false);
  } else {
    return false;
  }
  moved();
  return true;
}

bool Reader::prevPage() {
  if (page_ > 0) {
    --page_;
  } else if (chapter_ > 0) {
    loadChapter(chapter_ - 1, true);
  } else {
    return false;
  }
  moved();
  return true;
}

void Reader::goToChapter(size_t index) {
  if (index >= book_.chapterCount()) return;
  loadChapter(index, false);
  save();
}

void Reader::cycleFontSize() {
  const uint32_t keep = pages_.empty() ? 0 : pages_[page_].offset;
  fontSize_ = (fontSize_ + 1) % fonts::kReaderSizeCount;
  makeTypesetter();
  layoutChapter();
  page_ = 0;
  while (page_ + 1 < pages_.size() && pages_[page_ + 1].offset <= keep) ++page_;
  save();
}

float Reader::progress() const {
  const uint64_t total = book_.totalSize();
  if (total == 0 || pages_.empty()) return 0;
  const float inChapter = float(page_ + 1) / pages_.size();
  return (book_.sizeBefore(chapter_) + book_.chapterSize(chapter_) * inChapter) / float(total);
}

bool Reader::drawPage(GFXcanvas1 &c, int x, int y) const {
  if (pages_.empty() || !Typesetter::hasContent(flow_, pages_[page_].offset)) return false;
  typesetter_->layoutPage(flow_, pages_[page_], &c, x, y);
  return true;
}
