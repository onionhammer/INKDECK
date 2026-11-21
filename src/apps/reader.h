// Reader (the "Reader" artboard): a book page in Literata on a 30 px line grid with an
// inverted drop cap, the "§ CHAPTER ---- AUTHOR" line above it and a progress strip below.
//   ▲ previous page   ▼ next page   ● menu (contents, text size, library)   BOOT back
// The position is saved per book (readstate.h); READER on Home resumes the last book.
#pragma once
#include <memory>
#include <string>

#include "ui/nav.h"

namespace apps {

// Opens the EPUB at path at its saved position, or returns nullptr with err set. The menu's
// LIBRARY goes back to the screen that opened it (the library, apps/library.h, or Files).
std::unique_ptr<ui::Screen> makeReader(const std::string &path, std::string &err);

// The last book read, at its saved position, or nullptr if there isn't one (or it's gone).
// LIBRARY then opens the library in its place (openBookLibrary()).
std::unique_ptr<ui::Screen> resumeLastBook();

}  // namespace apps
