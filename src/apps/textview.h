// Text reader: a plain-text file from Files (anything that reads as text, not only .txt), set
// like the Reader but scrolled instead of paged. Decoding and line layout: textdoc.h.
//   ▲▼ scroll (held, it keeps scrolling, half a screen a step after a moment)   ● menu   BOOT back
// The body follows the Reader artboard: "§ NAME - - - SIZE" line, the text column, a progress
// strip ("LN 120/2400 [=====:::::] 5%"), plus a scrollbar in the right margin. The menu has
// text size, font (Literata, or JetBrains Mono for code and data), the scroll step (a line, a
// quarter or half a screen), back to the start, and the folder.
// The place, size and font are kept per file (readstate.h; the font in Position::chapter). A
// text file doesn't become the Reader's last book: Home's READER stays with EPUBs.
#pragma once
#include <memory>
#include <string>

#include "ui/nav.h"

namespace apps {

// Opens the text file at path at its saved place, or returns nullptr with err set (not text,
// unreadable). Pushed over the Files screen it came from; the menu's FOLDER goes back to it.
std::unique_ptr<ui::Screen> makeTextReader(const std::string &path, std::string &err);

}  // namespace apps
