#include "apps/note.h"

#include <Preferences.h>
#include <errno.h>
#include <stdio.h>
#include <strings.h>
#include <sys/stat.h>

#include <algorithm>

#include "apps/files.h"
#include "apps/home.h"
#include "dirlist.h"
#include "flashfs.h"
#include "fonts.h"
#include "notedoc.h"
#include "readstate.h"
#include "screen.h"
#include "sdcard.h"
#include "text.h"
#include "ui/menu.h"

namespace apps {
namespace {

using ui::Key;
using ui::Rect;

// The Reader's layout (apps/reader.cpp), with the chapter line saying when the note was made.
constexpr int kStripH = 30;  // progress strip, including its 3 px rule
constexpr int kPadTop = 22;
constexpr int kPadX = 30;
constexpr int kPadBottom = 8;
constexpr int kHeadLineH = 17;
constexpr int kHeadGap = 14;
constexpr ui::TextStyle kHeadStyle{&ui::fonts::kMono13Bold, 2};
constexpr ui::TextStyle kStripStyle{&ui::fonts::kMono13Bold, 1};

constexpr int kBodyH = screen::kHeight - ui::kStatusBarH - ui::kHintBarH;
constexpr int kTextW = screen::kWidth - 2 * kPadX;
constexpr int kTextH = kBodyH - kStripH - kPadTop - kHeadLineH - kHeadGap - kPadBottom;

constexpr const char *kPrefs = "note";
constexpr size_t kMaxFileBytes = 64 * 1024;  // of a note file read; the rest is cut off
constexpr size_t kMaxTitleBytes = 120;
constexpr const char *kCutLine = "*[Cut off here: too long for the board.]*";

// --- Settings: the text size (one for every note) and the last note -------------------------

uint8_t loadSize() {
  Preferences prefs;
  if (!prefs.begin(kPrefs, false)) return fonts::kDefaultReaderSize;  // read-only fails until it exists
  const uint8_t v = prefs.isKey("size") ? prefs.getUChar("size") : fonts::kDefaultReaderSize;
  prefs.end();
  return v % fonts::kReaderSizeCount;
}

void saveSize(uint8_t size) {
  Preferences prefs;
  if (!prefs.begin(kPrefs, false)) return;
  prefs.putUChar("size", size);
  prefs.end();
}

std::string loadLast() {
  Preferences prefs;
  if (!prefs.begin(kPrefs, false)) return {};
  const std::string path = prefs.isKey("last") ? prefs.getString("last").c_str() : "";
  prefs.end();
  return path;
}

void saveLast(const std::string &path) {
  if (loadLast() == path) return;  // spare the flash
  Preferences prefs;
  if (!prefs.begin(kPrefs, false)) return;
  prefs.putString("last", path.c_str());
  prefs.end();
}

// --- Files ----------------------------------------------------------------------------------

std::string folderOf(const std::string &path) { return path.substr(0, path.rfind('/')); }
std::string nameOf(const std::string &path) { return path.substr(path.rfind('/') + 1); }

bool isMarkdown(const std::string &name) {
  return name.size() > 3 && strcasecmp(name.c_str() + name.size() - 3, ".md") == 0;
}

std::string trim(const std::string &s) {
  const size_t first = s.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return {};
  return s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1);
}

// One line, at most kMaxTitleBytes (not inside a character).
std::string cleanTitle(std::string title) {
  for (char &c : title) {
    if (c == '\n' || c == '\r' || c == '\t') c = ' ';
  }
  title = trim(title);
  if (title.size() > kMaxTitleBytes) {
    size_t at = kMaxTitleBytes;
    while (at > 0 && (static_cast<uint8_t>(title[at]) & 0xC0) == 0x80) --at;
    title = trim(title.substr(0, at));
  }
  return title;
}

// A text that starts with its title as a heading loses it: the note shows the title anyway.
// Without a title, a first heading becomes it (only a "# " one with h1Only).
void takeTitleHeading(std::string &text, std::string &title, bool h1Only) {
  const size_t start = text.find_first_not_of(" \t\r\n");
  if (start == std::string::npos || text[start] != '#') return;
  const size_t eol = text.find('\n', start);
  const std::string line = text.substr(start, eol == std::string::npos ? eol : eol - start);
  size_t hashes = 0;
  while (hashes < line.size() && line[hashes] == '#') ++hashes;
  if (hashes > (h1Only ? 1u : 6u) || (hashes < line.size() && line[hashes] != ' ')) return;
  std::string heading = trim(line.substr(hashes));
  while (!heading.empty() && heading.back() == '#') heading.pop_back();
  heading = trim(heading);
  if (title.empty()) {
    title = heading;
  } else if (!NoteDoc::sameText(heading, title)) {
    return;
  }
  text.erase(0, eol == std::string::npos ? text.size() : eol + 1);
}

// Cuts text to at most max bytes, at a line break (or a space) if there's one in the second
// half, and ends it with a line saying so.
void cutOff(std::string &text, size_t max) {
  size_t at = text.rfind('\n', max);
  if (at == std::string::npos || at < max / 2) at = text.rfind(' ', max);
  if (at == std::string::npos || at < max / 2) {
    at = std::min(max, text.size());
    while (at > 0 && (static_cast<uint8_t>(text[at]) & 0xC0) == 0x80) --at;
  }
  text.resize(at);
  text += "\n\n";
  text += kCutLine;
}

// "Banana bread" -> "Banana bread.md": ASCII, without what FAT forbids in a name.
std::string fileNameFor(const std::string &title) {
  std::string name;
  for (char c : text::toAscii(title)) {
    if (!isalnum(static_cast<unsigned char>(c)) && !strchr(" -_.,()'&!+@", c)) c = ' ';
    if (c == ' ' && (name.empty() || name.back() == ' ')) continue;
    name += c;
    if (name.size() >= 60) break;
  }
  while (!name.empty() && (name.back() == ' ' || name.back() == '.')) name.pop_back();
  return (name.empty() ? std::string("Note") : name) + ".md";
}

// The Notes folder on the card, or on the onboard flash without one; "" if neither is there.
std::string notesFolder(bool create) {
  std::string root;
  if (sdcard::mounted()) {
    root = sdcard::kMountPoint;
  } else if (flashfs::mounted()) {
    root = flashfs::kMountPoint;
  } else {
    return {};
  }
  const std::string found = DirList::findSubdir(root, "notes");
  if (!found.empty()) return root + "/" + found;
  const std::string dir = root + "/Notes";
  if (create && mkdir(dir.c_str(), 0775) != 0 && errno != EEXIST) return {};
  return dir;
}

// --- The screen -----------------------------------------------------------------------------

class NoteScreen;
NoteScreen *current = nullptr;  // the newest Note screen, while it lives

class NoteScreen : public ui::Screen {
 public:
  NoteScreen(Note note, bool fromFiles, bool atStart)
      : note_(std::move(note)), fromFiles_(fromFiles), size_(loadSize()) {
    current = this;
    doc_.parse(note_.title, note_.text);
    readstate::Position pos;
    const bool resume = !atStart && !note_.path.empty() && readstate::load(note_.path, pos);
    layout(resume ? pos.offset : 0);
    if (!note_.path.empty()) saveLast(note_.path);
  }
  ~NoteScreen() override {
    if (current == this) current = nullptr;
  }

  const Note &note() const { return note_; }

  std::string tag() const override { return moduleTag(fromFiles_ ? "FILES" : "VOICE"); }
  std::string context() const override { return ui::upper(note_.title); }
  int hints(ui::Hint *out) const override {
    out[0] = {ui::glyph::kUp, "PREV"};
    out[1] = {ui::glyph::kDown, "NEXT"};
    out[2] = {ui::glyph::kPress, "MENU"};
    out[3] = {"BOOT", "BACK"};
    return 4;
  }

  bool reading() const override { return true; }  // light sleep between pages
  void onNap() override { save(); }
  void onLeave() override { save(); }  // covered (the menu, the voice sheet), or closed

  bool onKey(Key key) override {
    switch (key) {
      case Key::Up: return turn(-1);
      case Key::Down: return turn(+1);
      case Key::Select: openMenu(); return true;
      default: return false;
    }
  }

  void draw(ui::Canvas &c, const Rect &body) override {
    Rect r = body;
    drawStrip(c, r.takeBottom(kStripH));
    drawHeadLine(c, {r.x + kPadX, r.y + kPadTop, r.w - 2 * kPadX, kHeadLineH});
    const int textY = r.y + kPadTop + kHeadLineH + kHeadGap;
    if (doc_.empty()) {
      ui::drawText(c, {r.x + kPadX, textY, kTextW, 40}, "EMPTY NOTE", ui::style::kLabel,
                   ui::Align::Center);
      return;
    }
    doc_.drawPage(c, page_, r.x + kPadX, textY);
  }

 private:
  bool turn(int delta) {
    const int page = std::max(0, std::min(page_ + delta, doc_.pageCount() - 1));
    if (page == page_) return false;
    page_ = page;
    return true;
  }

  float progress() const { return static_cast<float>(page_ + 1) / doc_.pageCount(); }

  // Lays the note out in the current size, on the page that holds `at`.
  void layout(uint32_t at) {
    doc_.layout(fonts::kReader[size_], kTextW, kTextH);
    page_ = doc_.pageAt(at);
  }

  void save() const {
    if (note_.path.empty()) return;
    readstate::Position pos;
    pos.offset = doc_.pageOffset(page_);
    pos.fontSize = size_;
    pos.percent = static_cast<uint8_t>(progress() * 100);
    readstate::save(note_.path, note_.title, pos, false);  // not the Reader's last book
  }

  // "TODAY 14:32", or "19 NOV 14:32" from another day; nothing if the clock wasn't set.
  std::string madeText() const {
    if (note_.made <= 0) return {};
    tm made, today;
    localtime_r(&note_.made, &made);
    const time_t now = time(nullptr);
    localtime_r(&now, &today);
    if (made.tm_year + 1900 < 2024) return {};
    char buf[24];
    const bool sameDay = made.tm_year == today.tm_year && made.tm_yday == today.tm_yday;
    strftime(buf, sizeof buf, sameDay ? "TODAY %H:%M" : "%d %b %H:%M", &made);
    return ui::upper(buf);
  }

  // "§ NOTE - - - - TODAY 14:32", like the Reader's chapter line.
  void drawHeadLine(ui::Canvas &c, const Rect &line) const {
    const std::string right = madeText();
    const int rightW = right.empty() ? 0 : ui::textWidth(kHeadStyle, right);
    const int base = ui::baselineCentered(kHeadStyle, line.y, line.h);
    const int dashFrom =
        ui::drawText(c, line.x, base, std::string(ui::glyph::kSection) + " NOTE", kHeadStyle) + 12;
    const int dashTo = right.empty() ? line.right() : line.right() - rightW - 12;
    if (!right.empty()) ui::drawText(c, line.right() - rightW, base, right, kHeadStyle);
    if (dashTo > dashFrom) ui::dashedH(c, dashFrom, line.centerY(), dashTo - dashFrom);
  }

  // "P. 2/5 [=====:::::] 40%", as the Reader's.
  void drawStrip(ui::Canvas &c, const Rect &strip) const {
    ui::hrule(c, strip.x, strip.y, strip.w);
    const Rect band{strip.x + ui::kPadX, strip.y + ui::kRule, strip.w - 2 * ui::kPadX,
                    strip.h - ui::kRule};
    const std::string left =
        "P. " + std::to_string(page_ + 1) + "/" + std::to_string(doc_.pageCount());
    const std::string right = std::to_string(static_cast<int>(progress() * 100)) + "%";
    const int leftW = ui::textWidth(kStripStyle, left);
    const int rightW = ui::textWidth(kStripStyle, right);
    const int base = ui::baselineCentered(kStripStyle, band.y, band.h);
    ui::drawText(c, band.x, base, left, kStripStyle);
    ui::drawText(c, band.right() - rightW, base, right, kStripStyle);
    ui::drawProgress(c, {band.x + leftW + 10, band.centerY() - 7, band.w - leftW - rightW - 20, 14},
                     progress());
  }

  void openMenu() {
    NoteScreen *n = this;  // below the menu, so it outlives it
    std::vector<ui::MenuItem> items;
    items.push_back({"RESUME", nullptr, [] { ui::nav::pop(); }});
    items.push_back({"TEXT SIZE", [n] { return ui::upper(fonts::kReader[n->size_].name); },
                     [n] {
                       const uint32_t at = n->doc_.pageOffset(n->page_);
                       n->size_ = (n->size_ + 1) % fonts::kReaderSizeCount;
                       saveSize(n->size_);
                       n->layout(at);
                     }});
    items.push_back({"TO START", nullptr, [n] {
                       n->page_ = 0;
                       ui::nav::pop();
                     }});
    if (!note_.path.empty()) {
      const std::string path = note_.path;
      const bool fromFiles = fromFiles_;
      items.push_back({"FOLDER", [path] { return ui::upper(nameOf(folderOf(path))); },
                       [path, fromFiles] {
                         ui::nav::pop();  // the menu
                         if (fromFiles) {
                           ui::nav::pop();  // back to the Files screen that opened it
                         } else {
                           openLibrary("notes", folderOf(path), nameOf(path), true);
                         }
                       }});
    }
    ui::nav::push(std::make_unique<ui::MenuScreen>("NOTE", std::move(items)));
  }

  Note note_;
  bool fromFiles_;
  uint8_t size_;
  NoteDoc doc_;
  int page_ = 0;
};

}  // namespace

bool saveNote(Note &note, std::string &err) {
  note.title = cleanTitle(note.title);
  takeTitleHeading(note.text, note.title, false);
  note.title = cleanTitle(note.title);
  if (note.title.empty()) note.title = "Note";
  note.cut = note.text.size() > kMaxNoteBytes;
  if (note.cut) cutOff(note.text, kMaxNoteBytes);
  note.made = time(nullptr);
  note.path.clear();

  const std::string dir = notesFolder(true);
  if (dir.empty()) {
    err = "there's no card or flash to keep it on";
    return false;
  }
  const std::string path = dir + "/" + fileNameFor(note.title);
  const std::string data = "# " + note.title + "\n\n" + note.text + "\n";  // PSRAM: fine to fwrite
  FILE *f = fopen(path.c_str(), "wb");
  if (!f) {
    err = "can't write " + displayPath(path);
    return false;
  }
  const bool wrote = fwrite(data.data(), 1, data.size(), f) == data.size();
  if (fclose(f) != 0 || !wrote) {
    err = "writing " + displayPath(path) + " failed";
    return false;
  }
  note.path = path;
  readstate::save(path, note.title, readstate::Position{}, false);  // a new note opens at its start
  saveLast(path);
  return true;
}

bool readNote(const std::string &path, Note &out, std::string &err) {
  struct stat st;
  FILE *f = stat(path.c_str(), &st) == 0 ? fopen(path.c_str(), "rb") : nullptr;
  if (!f) {
    err = "can't open it";
    return false;
  }
  std::string data(std::min<size_t>(st.st_size, kMaxFileBytes), '\0');
  data.resize(fread(&data[0], 1, data.size(), f));
  fclose(f);
  const bool cut = static_cast<size_t>(st.st_size) > data.size();
  if (cut) cutOff(data, data.size());
  if (data.rfind("\xEF\xBB\xBF", 0) == 0) data.erase(0, 3);  // a byte-order mark

  out = Note{};
  takeTitleHeading(data, out.title, true);
  if (out.title.empty()) {
    const std::string name = nameOf(path);
    out.title = isMarkdown(name) ? name.substr(0, name.size() - 3) : name;
  }
  out.title = cleanTitle(out.title);
  out.text = std::move(data);
  out.path = path;
  out.made = st.st_mtime;
  out.cut = cut;
  return true;
}

bool lastNote(Note &out) {
  const std::string path = loadLast();
  std::string err;
  return !path.empty() && readNote(path, out, err);
}

const Note *noteOnScreen() {
  return current && ui::nav::base() == current ? &current->note() : nullptr;
}

bool isNotePath(const std::string &path) {
  const std::string folder = nameOf(folderOf(path));
  return isMarkdown(path) && strcasecmp(folder.c_str(), "notes") == 0;
}

std::unique_ptr<ui::Screen> makeNote(Note note, bool fromFiles, bool atStart) {
  return std::make_unique<NoteScreen>(std::move(note), fromFiles, atStart);
}

}  // namespace apps
