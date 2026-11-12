#include "ui/menu.h"

namespace ui {

MenuScreen::MenuScreen(std::string title, std::vector<MenuItem> items)
    : title_(std::move(title)), items_(std::move(items)) {
  cursor_.reset(static_cast<int>(items_.size()));
}

int MenuScreen::hints(Hint *out) const {
  out[0] = {glyph::kDial, "MOVE"};
  out[1] = {glyph::kPress, "SELECT"};
  out[2] = {"BOOT", "CLOSE"};
  return 3;
}

bool MenuScreen::onKey(Key key) {
  switch (key) {
    case Key::Up: return cursor_.move(-1, true);
    case Key::Down: return cursor_.move(+1, true);
    case Key::Select:
      if (items_.empty()) return false;
      if (items_[cursor_.cursor()].action) items_[cursor_.cursor()].action();
      return true;
    default: return false;  // BOOT closes (the navigator pops)
  }
}

void MenuScreen::draw(Canvas &c, const Rect &body) {
  const int rows = static_cast<int>(items_.size());
  Rect sheet{body.x, body.bottom() - (kRule + kSectionH + rows * kRowH), body.w, 0};
  sheet.h = body.bottom() - sheet.y;
  fill(c, sheet, kPaper);
  hrule(c, sheet.x, sheet.y, sheet.w);
  Rect r = sheet;
  r.takeTop(kRule);
  drawSectionHeader(c, r.takeTop(kSectionH), title_);
  drawList(c, r, kRowH, cursor_, [this](int i) {
    Row row;
    row.title = items_[i].label;
    if (items_[i].value) row.trailing = items_[i].value();
    return row;
  });
}

}  // namespace ui
