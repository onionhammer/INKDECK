// A menu sheet: an overlay screen that rises from the bottom of the screen below it, with a
// "// TITLE" header and one row per item (label left, current value right). It grows to fit its
// items, up to the whole body; past that it shows a page of them at a time, with the cursor's
// place ("03/12") on the header's right.
//   ▲▼ move   ● run the item   BOOT close
// An item's action runs inside the key handler. It can change things in place (the menu
// redraws, and so does the screen below, live) or navigate: nav::pop() closes the menu, and
// nav::replace() swaps the menu for another screen.
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "ui/nav.h"

namespace ui {

struct MenuItem {
  std::string label;
  std::function<std::string()> value;  // optional; drawn right-aligned
  std::function<void()> action;
};

class MenuScreen : public Screen {
 public:
  // cursor: the row selected when it opens.
  MenuScreen(std::string title, std::vector<MenuItem> items, int cursor = 0);

  std::string tag() const override { return {}; }  // the screen below supplies the chrome
  bool overlay() const override { return true; }
  int hints(Hint *out) const override;
  bool onKey(Key key) override;
  void draw(Canvas &c, const Rect &body) override;

 private:
  std::string title_;
  std::vector<MenuItem> items_;
  ListCursor cursor_;
};

}  // namespace ui
