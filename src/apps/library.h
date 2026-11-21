// The Reader's library: every book in SD:/Books (and the folders in it) in one list, grouped
// under their authors and put in order by book_library.h: authors by surname (the sort name a
// book gives, "LE GUIN, URSULA K."), each author's books by title. An author's heading is a row
// of its own (the name in capitals, a dashed rule, how many books); the cursor skips it. A book's
// row shows its title and how far it has been read (41%). The first time a book is seen, its
// title and author are read from it under an inverted `// READING BOOKS…` header (12/80); after
// that they come from the card's cache.
//   ▲▼ move   ● read   BOOT back
// The Reader's menu LIBRARY comes back here, with the cursor on the book.
#pragma once
#include <string>

namespace apps {

// Pushes the library (or replaces the top screen with it, for the Reader's LIBRARY), the cursor
// on `book` if it's in it. Without a Books folder on the card, or for a book outside it, Files
// opens instead, at the book's folder (openLibrary() in files.h).
void openBookLibrary(const std::string &book = "", bool replace = false);

}  // namespace apps
