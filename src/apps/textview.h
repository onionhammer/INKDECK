// Text reader: a plain-text file from Files (anything that reads as text, not only .txt), set
// like the Reader but scrolled instead of paged. Decoding and line layout: textdoc.h.
//   ▲▼ scroll (held, it keeps scrolling, half a screen a step after a moment)   ● menu   BOOT back
// The body follows the Reader artboard: "§ NAME - - - SIZE" line, the text column, a progress
// strip ("LN 120/2400 [=====:::::] 5%"), plus a scrollbar in the right margin. The menu has
// text size, font (Literata, or JetBrains Mono for code and data), the scroll step (a line, a
// quarter or half a screen), back to the start, and the folder.
// The place, size and font are kept per file (readstate.h; the font in Position::chapter), and
// the file counts as the last thing read, so READER on Home comes back to it.
#pragma once
#include <memory>
#include <string>

#include "ui/nav.h"

namespace apps {

// Opens the text file at path at its saved place, or returns nullptr with err set (not text,
// unreadable). fromFiles: the menu's folder item goes back to the Files screen below rather
// than opening one.
std::unique_ptr<ui::Screen> makeTextReader(const std::string &path, std::string &err,
                                           bool fromFiles = true);

}  // namespace apps
