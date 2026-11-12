#include "browser.h"

#include <dirent.h>
#include <esp32-hal-log.h>
#include <string.h>
#include <strings.h>

#include <algorithm>

#include "fonts.h"
#include "screen.h"
#include "text.h"
#include "typeset.h"

namespace {

constexpr size_t kMaxEntries = 1000;
constexpr int kHeaderHeight = 52;
constexpr int kFooterHeight = 44;
constexpr int kRowHeight = 44;
constexpr int kPadX = 16;

bool endsWithNoCase(const std::string &s, const char *suffix) {
  const size_t n = strlen(suffix);
  return s.size() >= n && strcasecmp(s.c_str() + s.size() - n, suffix) == 0;
}

}  // namespace

bool Browser::isEpub(const std::string &name) { return endsWithNoCase(name, ".epub"); }

bool Browser::open(const std::string &dir, const std::string &select) {
  cwd_ = dir;
  entries_.clear();
  cursor_ = 0;
  DIR *d = opendir(dir.c_str());
  readable_ = d != nullptr;
  if (!d) {
    log_w("browser: can't open %s", dir.c_str());
    return false;
  }
  while (dirent *e = readdir(d)) {
    const std::string name = e->d_name;
    if (name.empty() || name[0] == '.' || name == "System Volume Information") continue;
    entries_.push_back({name, e->d_type == DT_DIR});
    if (entries_.size() >= kMaxEntries) break;
  }
  closedir(d);

  std::sort(entries_.begin(), entries_.end(), [](const Entry &a, const Entry &b) {
    if (a.isDir != b.isDir) return a.isDir;
    return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
  });
  for (size_t i = 0; i < entries_.size(); ++i) {
    if (entries_[i].name == select) cursor_ = static_cast<int>(i);
  }
  log_i("browser: %s (%u entries)", dir.c_str(), static_cast<unsigned>(entries_.size()));
  return true;
}

bool Browser::goUp() {
  if (cwd_.size() <= root_.size()) return false;
  const size_t slash = cwd_.rfind('/');
  const std::string child = cwd_.substr(slash + 1);
  open(slash <= root_.size() ? root_ : cwd_.substr(0, slash), child);
  return true;
}

void Browser::moveCursor(int delta) {
  if (entries_.empty()) return;
  cursor_ = std::max(0, std::min(static_cast<int>(entries_.size()) - 1, cursor_ + delta));
}

const Browser::Entry *Browser::selected() const {
  return entries_.empty() ? nullptr : &entries_[cursor_];
}

std::string Browser::selectedName() const {
  const Entry *e = selected();
  return e ? e->name : std::string();
}

std::string Browser::selectedPath() const {
  const Entry *e = selected();
  return e ? cwd_ + "/" + e->name : cwd_;
}

int Browser::rowsPerPage() const {
  return (screen::kHeight - kHeaderHeight - 4 - kFooterHeight) / kRowHeight;
}

void Browser::draw(GFXcanvas1 &c) const {
  const int w = screen::kWidth;
  const int h = screen::kHeight;

  // Header: current folder, white on black.
  c.fillRect(0, 0, w, kHeaderHeight, gfx::kBlack);
  std::string title = cwd_.size() > root_.size() ? cwd_.substr(root_.size()) : "SD card";
  title = gfx::ellipsizeFront(fonts::kUiBold, text::toAscii(title), w - 2 * kPadX);
  gfx::drawText(c, kPadX, 35, title, fonts::kUiBold, gfx::kWhite);

  const int top = kHeaderHeight + 4;
  const int rows = rowsPerPage();
  const int textOffset = (kRowHeight + gfx::ascent(fonts::kUi)) / 2;

  if (!readable_ || entries_.empty()) {
    const char *msg = readable_ ? "(empty folder)" : "No SD card. Press to retry.";
    const int tw = gfx::textWidth(fonts::kUi, msg);
    gfx::drawText(c, (w - tw) / 2, h / 2, msg, fonts::kUi);
  } else {
    const int first = (cursor_ / rows) * rows;
    for (int i = 0; i < rows && first + i < static_cast<int>(entries_.size()); ++i) {
      const Entry &e = entries_[first + i];
      const int y = top + i * kRowHeight;
      const bool sel = first + i == cursor_;
      const uint16_t fg = sel ? gfx::kWhite : gfx::kBlack;
      if (sel) c.fillRect(0, y, w, kRowHeight, gfx::kBlack);
      const GFXfont *font = e.isDir ? fonts::kUiBold : fonts::kUi;
      std::string label = text::toAscii(e.name);
      if (e.isDir) label += "/";
      label = gfx::ellipsize(font, label, w - 2 * kPadX);
      gfx::drawText(c, kPadX, y + textOffset, label, font, fg);
      if (!sel && i + 1 < rows) c.drawFastHLine(kPadX, y + kRowHeight - 1, w - 2 * kPadX, gfx::kBlack);
    }
  }

  // Footer: message or key hints, plus position.
  const int fy = h - kFooterHeight;
  c.drawFastHLine(0, fy, w, gfx::kBlack);
  const int baseline = fy + 28;
  std::string right;
  if (!entries_.empty()) right = std::to_string(cursor_ + 1) + "/" + std::to_string(entries_.size());
  const int rightW = gfx::textWidth(fonts::kUiSmall, right);
  const std::string left = message_.empty() ? "Press: open   BOOT: up" : message_;
  gfx::drawText(c, kPadX, baseline, gfx::ellipsize(fonts::kUiSmall, left, w - 3 * kPadX - rightW),
                fonts::kUiSmall);
  gfx::drawText(c, w - kPadX - rightW, baseline, right, fonts::kUiSmall);
}
