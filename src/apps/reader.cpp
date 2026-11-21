#include "apps/reader.h"

#include <sys/stat.h>

#include <vector>

#include "apps/home.h"
#include "apps/library.h"
#include "fonts.h"
#include "book_reader.h"
#include "readstate.h"
#include "screen.h"
#include "ui/menu.h"

namespace apps {
namespace {

using ui::Key;
using ui::Rect;

// Layout from the Reader artboard.
constexpr int kStripH = 30;  // progress strip, including its 3 px rule
constexpr int kPadTop = 22;
constexpr int kPadX = 30;
constexpr int kPadBottom = 8;
constexpr int kChapterLineH = 17;
constexpr int kChapterGap = 14;
constexpr ui::TextStyle kChapterStyle{&ui::fonts::kMono13Bold, 2};  // letter-spacing 0.14em
constexpr ui::TextStyle kStripStyle{&ui::fonts::kMono13Bold, 1};

constexpr int kBodyH = screen::kHeight - ui::kStatusBarH - ui::kHintBarH;
constexpr int kTextW = screen::kWidth - 2 * kPadX;
constexpr int kTextH = kBodyH - kStripH - kPadTop - kChapterLineH - kChapterGap - kPadBottom;

std::string roman(int n) {
  static const struct {
    int value;
    const char *digits;
  } kTable[] = {{1000, "M"}, {900, "CM"}, {500, "D"}, {400, "CD"}, {100, "C"}, {90, "XC"},
                {50, "L"},   {40, "XL"},  {10, "X"},  {9, "IX"},   {5, "V"},   {4, "IV"},
                {1, "I"}};
  std::string out;
  for (const auto &t : kTable) {
    for (; n >= t.value; n -= t.value) out += t.digits;
  }
  return out;
}

std::string folderOf(const std::string &path) { return path.substr(0, path.rfind('/')); }
std::string nameOf(const std::string &path) { return path.substr(path.rfind('/') + 1); }

// One line per contents entry: the book's TOC, or one per spine item if it has none.
struct ContentsEntry {
  std::string title;
  size_t chapter;
};

std::vector<ContentsEntry> contentsOf(const Epub &book) {
  std::vector<ContentsEntry> out;
  for (const auto &e : book.toc()) out.push_back({e.title, e.chapter});
  if (out.empty()) {
    for (size_t i = 0; i < book.chapterCount(); ++i) {
      out.push_back({"Section " + std::to_string(i + 1), i});
    }
  }
  return out;
}

// The entry the reader is in: the last one at or before its chapter.
int currentEntry(const std::vector<ContentsEntry> &entries, size_t chapter) {
  int best = 0;
  for (size_t i = 0; i < entries.size(); ++i) {
    if (entries[i].chapter <= chapter &&
        entries[i].chapter > entries[static_cast<size_t>(best)].chapter) {
      best = static_cast<int>(i);
    }
  }
  return best;
}

class ContentsScreen : public ui::Screen {
 public:
  explicit ContentsScreen(Reader *reader) : reader_(reader), entries_(contentsOf(reader->book())) {
    current_ = currentEntry(entries_, reader->chapter());
    cursor_.reset(static_cast<int>(entries_.size()), current_);
  }

  std::string tag() const override { return moduleTag("READER"); }
  std::string context() const override { return ui::upper(reader_->book().title()); }
  int hints(ui::Hint *out) const override {
    out[0] = {ui::glyph::kDial, "MOVE"};
    out[1] = {ui::glyph::kPress, "GO"};
    out[2] = {"BOOT", "BACK"};
    return 3;
  }

  bool onKey(Key key) override {
    switch (key) {
      case Key::Up: return cursor_.move(-1);
      case Key::Down: return cursor_.move(+1);
      case Key::Select:
        reader_->goToChapter(entries_[cursor_.cursor()].chapter);
        ui::nav::pop();
        return true;
      default: return false;
    }
  }

  void draw(ui::Canvas &c, const Rect &body) override {
    Rect r = body;
    ui::drawSectionHeader(c, r.takeTop(ui::kSectionH), "CONTENTS", cursor_.label());
    ui::drawList(c, r, ui::kRowH, cursor_, [this](int i) {
      ui::Row row;
      char index[12];
      snprintf(index, sizeof index, "%02d", i + 1);
      row.index = index;
      row.title = entries_[i].title;
      if (i == current_) row.trailing = ui::glyph::kPlay;
      return row;
    });
  }

 private:
  Reader *reader_;  // the reader screen below this one owns it
  std::vector<ContentsEntry> entries_;
  int current_ = 0;
  ui::ListCursor cursor_;
};

class ReaderScreen : public ui::Screen {
 public:
  explicit ReaderScreen(bool fromFiles) : reader_(kTextW, kTextH), fromFiles_(fromFiles) {}
  bool open(const std::string &path, std::string &err) { return reader_.open(path, err); }

  std::string tag() const override { return moduleTag("READER"); }
  std::string context() const override { return ui::upper(reader_.book().title()); }
  int hints(ui::Hint *out) const override {
    out[0] = {ui::glyph::kUp, "PREV"};
    out[1] = {ui::glyph::kDown, "NEXT"};
    out[2] = {ui::glyph::kPress, "MENU"};
    out[3] = {"BOOT", "BACK"};
    return 4;
  }

  bool reading() const override { return true; }  // light sleep between page turns
  bool closesWhenIdle() const override { return true; }  // then off behind the cover
  void onNap() override { reader_.save(); }
  void onLeave() override { reader_.save(); }  // covered by the menu, or closed

  bool onKey(Key key) override {
    switch (key) {
      case Key::Up: return reader_.prevPage();
      case Key::Down: return reader_.nextPage();
      case Key::Select: openMenu(); return true;
      default: return false;
    }
  }

  void draw(ui::Canvas &c, const Rect &body) override {
    Rect r = body;
    drawStrip(c, r.takeBottom(kStripH));
    drawChapterLine(c, {r.x + kPadX, r.y + kPadTop, r.w - 2 * kPadX, kChapterLineH});
    const int textY = r.y + kPadTop + kChapterLineH + kChapterGap;
    if (!reader_.drawPage(c, r.x + kPadX, textY)) {
      ui::drawText(c, {r.x + kPadX, textY, kTextW, 40}, "EMPTY SECTION", ui::style::kLabel,
                   ui::Align::Center);
    }
  }

 private:
  // "§ CHAPTER - - - - AUTHOR": the TOC title (or a roman numeral), a dashed rule, the author.
  void drawChapterLine(ui::Canvas &c, const Rect &line) const {
    std::string chapter = reader_.book().chapterTitle(reader_.chapter());
    if (chapter.empty()) chapter = roman(static_cast<int>(reader_.chapter()) + 1);
    const std::string author = ui::ellipsize(kChapterStyle, ui::upper(reader_.book().author()),
                                             line.w * 2 / 5);
    const int authorW = ui::textWidth(kChapterStyle, author);
    const int chapterMax = line.w - authorW - (author.empty() ? 0 : 36);
    const std::string left = ui::ellipsize(
        kChapterStyle, std::string(ui::glyph::kSection) + " " + ui::upper(chapter), chapterMax);
    const int base = ui::baselineCentered(kChapterStyle, line.y, line.h);
    const int dashFrom = ui::drawText(c, line.x, base, left, kChapterStyle) + 12;
    const int dashTo = author.empty() ? line.right() : line.right() - authorW - 12;
    if (!author.empty()) ui::drawText(c, line.right() - authorW, base, author, kChapterStyle);
    if (dashTo > dashFrom) ui::dashedH(c, dashFrom, line.centerY(), dashTo - dashFrom);
  }

  // "P. 3/12 [=====:::::] 41%": page in this chapter, whole-book progress.
  void drawStrip(ui::Canvas &c, const Rect &strip) const {
    ui::hrule(c, strip.x, strip.y, strip.w);
    const Rect band{strip.x + ui::kPadX, strip.y + ui::kRule, strip.w - 2 * ui::kPadX,
                    strip.h - ui::kRule};
    const std::string left =
        "P. " + std::to_string(reader_.page() + 1) + "/" + std::to_string(reader_.pageCount());
    const std::string right = std::to_string(static_cast<int>(reader_.progress() * 100)) + "%";
    const int leftW = ui::textWidth(kStripStyle, left);
    const int rightW = ui::textWidth(kStripStyle, right);
    const int base = ui::baselineCentered(kStripStyle, band.y, band.h);
    ui::drawText(c, band.x, base, left, kStripStyle);
    ui::drawText(c, band.right() - rightW, base, right, kStripStyle);
    ui::drawProgress(c, {band.x + leftW + 10, band.centerY() - 7, band.w - leftW - rightW - 20, 14},
                     reader_.progress());
  }

  void openMenu() {
    Reader *r = &reader_;
    const bool fromFiles = fromFiles_;
    std::vector<ui::MenuItem> items;
    items.push_back({"RESUME", nullptr, [] { ui::nav::pop(); }});
    items.push_back({"CONTENTS",
                     [r] {
                       const auto entries = contentsOf(r->book());
                       return std::to_string(currentEntry(entries, r->chapter()) + 1) + "/" +
                              std::to_string(entries.size());
                     },
                     [r] { ui::nav::replace(std::make_unique<ContentsScreen>(r)); }});
    items.push_back({"TEXT SIZE", [r] { return ui::upper(fonts::kReader[r->fontSize()].name); },
                     [r] { r->cycleFontSize(); }});
    items.push_back({"LIBRARY", [r] { return ui::upper(nameOf(folderOf(r->path()))); },
                     [r, fromFiles] {
                       ui::nav::pop();  // the menu
                       if (fromFiles) {
                         ui::nav::pop();  // back to the library (or Files) that opened it
                       } else {
                         openBookLibrary(r->path(), true);
                       }
                     }});
    ui::nav::push(std::make_unique<ui::MenuScreen>("READER", std::move(items)));
  }

  Reader reader_;
  bool fromFiles_;
};

std::unique_ptr<ui::Screen> open(const std::string &path, bool fromFiles, std::string &err) {
  auto screen = std::make_unique<ReaderScreen>(fromFiles);
  if (!screen->open(path, err)) return nullptr;
  return screen;
}

}  // namespace

std::unique_ptr<ui::Screen> makeReader(const std::string &path, std::string &err) {
  return open(path, true, err);
}

std::unique_ptr<ui::Screen> resumeLastBook() {
  std::string path, title, err;
  readstate::Position pos;
  struct stat st;
  if (!readstate::last(path, title, pos) || stat(path.c_str(), &st) != 0) return nullptr;
  return open(path, false, err);
}

}  // namespace apps
