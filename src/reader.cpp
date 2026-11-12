#include "reader.h"

#include <Arduino.h>

#include "fonts.h"
#include "screen.h"

namespace {

constexpr int kMarginX = 20;
constexpr int kMarginTop = 22;
constexpr int kStatusHeight = 34;
constexpr int kTextWidth = screen::kWidth - 2 * kMarginX;
constexpr int kTextHeight = screen::kHeight - kMarginTop - kStatusHeight - 8;

}  // namespace

bool Reader::open(const std::string &path, std::string &err) {
  close();
  const uint32_t t0 = millis();
  if (!book_.open(path, err)) return false;
  makeTypesetter();
  loadChapter(0, false);
  log_i("reader: opened in %lu ms; heap %u KB, PSRAM %u KB free", (unsigned long)(millis() - t0),
        ESP.getFreeHeap() / 1024, ESP.getFreePsram() / 1024);
  return true;
}

void Reader::close() {
  book_.close();
  flow_.clear();
  flow_.shrink_to_fit();
  pages_.clear();
  chapter_ = page_ = 0;
}

void Reader::makeTypesetter() {
  typesetter_.reset(new Typesetter(fonts::kReader[fontSize_], kTextWidth, kTextHeight));
}

void Reader::loadChapter(size_t index, bool atEnd) {
  const uint32_t t0 = millis();
  chapter_ = index;
  std::string err;
  if (!book_.chapterFlow(index, flow_, err)) {
    flow_ = "Couldn't load this chapter: " + err;
  }
  const uint32_t t1 = millis();
  pages_ = typesetter_->paginate(flow_);
  page_ = atEnd ? pages_.size() - 1 : 0;
  log_i("reader: chapter %u/%u %s: %u bytes of text, %u pages (load %lu ms, layout %lu ms)",
        static_cast<unsigned>(index + 1), static_cast<unsigned>(book_.chapterCount()),
        book_.chapterPath(index).c_str(), static_cast<unsigned>(flow_.size()),
        static_cast<unsigned>(pages_.size()), (unsigned long)(t1 - t0),
        (unsigned long)(millis() - t1));
}

void Reader::nextPage() {
  if (page_ + 1 < pages_.size()) {
    ++page_;
  } else if (chapter_ + 1 < book_.chapterCount()) {
    loadChapter(chapter_ + 1, false);
  }
}

void Reader::prevPage() {
  if (page_ > 0) {
    --page_;
  } else if (chapter_ > 0) {
    loadChapter(chapter_ - 1, true);
  }
}

void Reader::nextChapter() {
  if (chapter_ + 1 < book_.chapterCount()) loadChapter(chapter_ + 1, false);
}

void Reader::prevChapter() {
  if (page_ > 0) {
    page_ = 0;
  } else if (chapter_ > 0) {
    loadChapter(chapter_ - 1, false);
  }
}

void Reader::cycleFontSize() {
  const uint32_t keep = pages_.empty() ? 0 : pages_[page_].offset;
  fontSize_ = (fontSize_ + 1) % fonts::kReaderSizeCount;
  makeTypesetter();
  pages_ = typesetter_->paginate(flow_);
  page_ = 0;
  while (page_ + 1 < pages_.size() && pages_[page_ + 1].offset <= keep) ++page_;
  log_i("reader: font %s, %u pages", fonts::kReader[fontSize_].name,
        static_cast<unsigned>(pages_.size()));
}

void Reader::draw(GFXcanvas1 &c) const {
  const int w = screen::kWidth;
  const int h = screen::kHeight;

  if (!pages_.empty() && Typesetter::hasContent(flow_, pages_[page_].offset)) {
    typesetter_->layoutPage(flow_, pages_[page_], &c, kMarginX, kMarginTop);
  } else {
    const char *msg = "(empty section)";
    gfx::drawText(c, (w - gfx::textWidth(fonts::kUiSmall, msg)) / 2, h / 2, msg, fonts::kUiSmall);
  }

  // Status bar: whole-book progress rule, chapter, title, page.
  const int top = h - kStatusHeight;
  const size_t chapters = book_.chapterCount();
  const float progress =
      chapters ? (chapter_ + float(page_ + 1) / std::max<size_t>(1, pages_.size())) / chapters : 0;
  c.drawFastHLine(0, top, w, gfx::kBlack);
  c.fillRect(0, top - 1, static_cast<int>(w * progress), 3, gfx::kBlack);

  const int baseline = top + 24;
  const std::string left = std::to_string(chapter_ + 1) + "/" + std::to_string(chapters);
  const std::string right = std::to_string(page_ + 1) + "/" + std::to_string(pages_.size());
  const int leftW = gfx::textWidth(fonts::kUiSmall, left);
  const int rightW = gfx::textWidth(fonts::kUiSmall, right);
  gfx::drawText(c, kMarginX, baseline, left, fonts::kUiSmall);
  gfx::drawText(c, w - kMarginX - rightW, baseline, right, fonts::kUiSmall);
  const int titleMax = w - 2 * kMarginX - leftW - rightW - 32;
  const std::string title = gfx::ellipsize(fonts::kUiSmall, book_.title(), titleMax);
  gfx::drawText(c, (w - gfx::textWidth(fonts::kUiSmall, title)) / 2, baseline, title,
                fonts::kUiSmall);
}
