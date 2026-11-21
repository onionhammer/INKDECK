#include "apps/library.h"

#include <esp32-hal-log.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <map>
#include <memory>
#include <vector>

#include "apps/files.h"
#include "apps/home.h"
#include "apps/reader.h"
#include "book_library.h"
#include "readstate.h"
#include "sdcard.h"
#include "ui/nav.h"

namespace apps {
namespace {

using ui::Key;
using ui::Rect;

constexpr uint32_t kReadBudgetMs = 1500;  // metadata read per tick, between progress redraws
constexpr ui::TextStyle kHeading{&ui::fonts::kMono15ExtraBold, 1};  // an author's name

std::string folderOf(const std::string &path) { return path.substr(0, path.rfind('/')); }
std::string nameOf(const std::string &path) { return path.substr(path.rfind('/') + 1); }

// path is dir or somewhere inside it. FAT ignores case, so this does too.
bool isInside(const std::string &path, const std::string &dir) {
  if (path.size() < dir.size() || strncasecmp(path.c_str(), dir.c_str(), dir.size()) != 0) {
    return false;
  }
  return path.size() == dir.size() || path[dir.size()] == '/';
}

class LibraryScreen : public ui::Screen {
 public:
  LibraryScreen(std::string root, std::string select)
      : root_(std::move(root)), select_(std::move(select)) {
    rescan();
  }

  std::string tag() const override { return moduleTag("READER"); }
  std::string context() const override { return displayPath(root_); }  // "SD:/Books"
  bool contextIsPath() const override { return true; }

  int hints(ui::Hint *out) const override {
    int n = 0;
    if (!scanning()) {
      if (bookCount_ > 0) {
        out[n++] = {ui::glyph::kDial, "MOVE"};
        out[n++] = {ui::glyph::kPress, "READ"};
      } else {
        out[n++] = {ui::glyph::kPress, "RETRY"};
      }
    }
    out[n++] = {"BOOT", "BACK"};
    return n;
  }

  // Back from a book: how far it has been read has changed.
  void onEnter() override { percent_.clear(); }

  bool onKey(Key key) override {
    if (scanning()) return false;  // BOOT goes back; what's been read is kept (~Shelf())
    const bool hadNotice = !notice_.empty();
    notice_.clear();
    switch (key) {
      case Key::Up: return move(-1) || hadNotice;
      case Key::Down: return move(+1) || hadNotice;
      case Key::Select: activate(); return true;
      case Key::Back: return false;
      default: return hadNotice;
    }
  }

  // New books are read on the ticks after the READING header is on the panel, a second and a
  // half at a time, the count redrawn between.
  bool onTick() override {
    if (!scanning() || !progressShown_) return false;
    if (shelf_.step(kReadBudgetMs)) finish();
    return true;
  }

  void draw(ui::Canvas &c, const Rect &body) override {
    Rect r = body;
    const Rect head = r.takeTop(ui::kSectionH);
    if (scanning()) {
      const int read = shelf_.total() - shelf_.unread();
      ui::drawSectionHeader(c, head, std::string("READING BOOKS") + ui::glyph::kEllipsis,
                            std::to_string(read) + "/" + std::to_string(shelf_.total()), {true});
      drawEmpty(c, r, "NEW BOOKS",
                "Reading the title and author of each new book, to list it under its author. "
                "Each book is read once, then kept on the card.");
      progressShown_ = true;
      return;
    }
    if (!notice_.empty()) {
      ui::drawNotice(c, head, notice_);
    } else {
      ui::drawSectionHeader(c, head, "LIBRARY", bookCount_ > 0 ? positionLabel() : "");
    }

    const std::string retry = std::string(" Press ") + ui::glyph::kPress + " to retry.";
    if (!readable_) {
      if (sdcard::mounted()) {
        drawEmpty(c, r, "CAN'T READ", "The books folder couldn't be opened." + retry);
      } else {
        drawEmpty(c, r, "NO SD CARD", "Insert a microSD card." + retry);
      }
      return;
    }
    if (bookCount_ == 0) {
      drawEmpty(c, r, "NO BOOKS",
                "No EPUB files in " + displayPath(root_) +
                    ". Copy some there over the Wi-Fi drive (System) or from a share in Files." +
                    retry);
      return;
    }

    const int perPage = r.h / ui::kRowH;
    const int first = cursor_.pageStart(perPage);
    for (int i = first; i < cursor_.count() && i < first + perPage; ++i) {
      const Rect row{r.x, r.y + (i - first) * ui::kRowH, r.w, ui::kRowH};
      const Item &item = items_[i];
      if (!item.book) {
        drawHeading(c, row, authors_[item.author]);
        continue;
      }
      ui::Row line;
      line.title = item.book->title;
      line.trailing = percentText(item.book->path);
      ui::drawRow(c, row, line, i == cursor_.cursor());
    }
  }

 private:
  // A row of the list: an author's heading, or one of their books.
  struct Item {
    const booklib::Book *book;  // nullptr: the heading of authors_[author]
    int author;
    int number;  // the book's place among all the books, from 1 (the header's 03/40)
  };

  // Reading the metadata of books the cache doesn't have yet.
  bool scanning() const { return readable_ && shelf_.unread() > 0; }

  // Lists the folder again; the list is built at once if every book is in the cache.
  void rescan() {
    if (!sdcard::mounted()) sdcard::mount();
    readable_ = shelf_.scan(root_);
    progressShown_ = false;
    if (!scanning()) finish();
  }

  // Every book read: the rows, a heading before each author's books, and the cursor on the
  // book asked for (else the first).
  void finish() {
    shelf_.save();
    authors_ = shelf_.byAuthor();
    items_.clear();
    bookCount_ = 0;
    int cursor = -1;
    for (size_t a = 0; a < authors_.size(); ++a) {
      items_.push_back({nullptr, static_cast<int>(a), 0});
      for (const booklib::Book &b : authors_[a].books) {
        ++bookCount_;
        if (cursor < 0 && !select_.empty() && strcasecmp(b.path.c_str(), select_.c_str()) == 0) {
          cursor = static_cast<int>(items_.size());
        }
        items_.push_back({&b, static_cast<int>(a), bookCount_});
      }
    }
    cursor_.reset(static_cast<int>(items_.size()), cursor >= 0 ? cursor : 1);
  }

  // ▲▼: the next book up or down, past an author's heading. A held dial moves a page at a time
  // (ListCursor) and lands on the page's first book.
  bool move(int delta) {
    const int before = cursor_.cursor();
    if (!cursor_.move(delta)) return false;
    if (items_[cursor_.cursor()].book) return true;
    const bool paged = abs(cursor_.cursor() - before) > 1;
    int to = cursor_.cursor() + (paged || delta > 0 ? 1 : -1);
    if (to < 0) to = 1;  // above the first book: stay on it
    cursor_.set(to);     // a heading always has a book after it
    return cursor_.cursor() != before;
  }

  void activate() {
    if (!readable_ || bookCount_ == 0) {
      rescan();
      return;
    }
    const booklib::Book *book = items_[cursor_.cursor()].book;
    if (!book) return;
    std::string err;
    if (auto reader = makeReader(book->path, err)) {
      ui::nav::push(std::move(reader));
    } else {
      log_w("open failed: %s", err.c_str());
      notice_ = "CAN'T OPEN: " + ui::upper(err);
    }
  }

  // "03/40": the selected book's place among the books (headings don't count).
  std::string positionLabel() const {
    char buf[16];
    snprintf(buf, sizeof buf, "%02d/%02d", items_[cursor_.cursor()].number, bookCount_);
    return buf;
  }

  // "41%" for a book that has been opened, "" for one that hasn't. Kept until the next
  // onEnter(), so a redraw doesn't read NVS for every row.
  std::string percentText(const std::string &path) const {
    auto it = percent_.find(path);
    if (it == percent_.end()) {
      readstate::Position pos;
      it = percent_.emplace(path, readstate::load(path, pos) ? pos.percent : -1).first;
    }
    return it->second < 0 ? "" : std::to_string(it->second) + "%";
  }

  // "LE GUIN, URSULA K. - - - - - - 3": the heading over an author's books, from the edge the
  // books' selection marker starts at.
  static void drawHeading(ui::Canvas &c, const Rect &row, const booklib::Author &author) {
    constexpr int kGap = 10;
    const Rect band = row.inset(ui::kPadX, 0);
    const std::string count = std::to_string(author.books.size());
    const int countW = ui::textWidth(ui::style::kStatus, count);
    const std::string name =
        ui::ellipsize(kHeading, author.name.empty() ? "UNKNOWN AUTHOR" : ui::upper(author.name),
                      band.w - countW - 4 * kGap);
    const int dashFrom =
        ui::drawText(c, band.x, ui::baselineCentered(kHeading, band.y, band.h), name, kHeading) +
        kGap;
    const int dashTo = band.right() - countW - kGap;
    ui::drawText(c, band.right() - countW, ui::baselineCentered(ui::style::kStatus, band.y, band.h),
                 count, ui::style::kStatus);
    if (dashTo > dashFrom) ui::dashedH(c, dashFrom, band.centerY(), dashTo - dashFrom);
  }

  static void drawEmpty(ui::Canvas &c, const Rect &r, const char *title,
                        const std::string &text) {
    const Rect p = r.inset(20, 24);
    int y = p.y + ui::capHeight(ui::style::kTitle);
    ui::drawText(c, p.x, y, title, ui::style::kTitle);
    y += 24;
    for (const auto &line : ui::wrap(ui::style::kBody, text, p.w, 4)) {
      y += 22;
      ui::drawText(c, p.x, y, line, ui::style::kBody);
    }
  }

  std::string root_;    // the books folder
  std::string select_;  // the book the cursor starts on
  booklib::Shelf shelf_;
  bool readable_ = false;
  bool progressShown_ = false;  // the READING header is on the panel
  std::vector<booklib::Author> authors_;
  std::vector<Item> items_;  // points into authors_
  int bookCount_ = 0;
  ui::ListCursor cursor_;
  std::string notice_;
  mutable std::map<std::string, int> percent_;  // path -> percent read, -1 never opened
};

}  // namespace

void openBookLibrary(const std::string &book, bool replace) {
  const std::string root = sdFolder("books");  // the card's root if there's no such folder
  if (root == sdcard::kMountPoint || (!book.empty() && !isInside(book, root))) {
    openLibrary("books", book.empty() ? "" : folderOf(book), book.empty() ? "" : nameOf(book),
                replace);
    return;
  }
  auto screen = std::make_unique<LibraryScreen>(root, book);
  if (replace) {
    ui::nav::replace(std::move(screen));
  } else {
    ui::nav::push(std::move(screen));
  }
}

}  // namespace apps
