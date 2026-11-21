// The Reader's books (apps/library.h): every EPUB in the card's Books folder and the folders in
// it (three deep, at most 400), each with its title and author from its own metadata
// (readEpubMeta() in epub.h), grouped by author and put in order.
//
// Reading a book's metadata means opening it, so what's read is kept on the card in
// SD:/.inkdeck/books.tsv, by path and size: a book is read again only when it's new or its size
// has changed, and books that have gone drop out of the file.
//
// Order: authors by the sort name a book gives ("Le Guin, Ursula K."), else by the name as
// written; a book without a sort name borrows one from another book with the same author. Books
// without an author come last. Each author's books by title: the book's sort title if it has
// one, else the title without a leading The, A or An. Case, Latin-1 accents and punctuation are
// ignored, and numbers go in numeric order (2 before 10).
#pragma once
#include <stdint.h>

#include <string>
#include <vector>

namespace booklib {

struct Book {
  std::string path;        // absolute
  int64_t size = -1;       // bytes
  std::string title;       // UTF-8, as the book gives it; the file name if it gives none
  std::string titleSort;   // the book's own sort title ("Dispossessed, The"), or ""
  std::string author;      // as written; "" if the book names none
  std::string authorSort;  // the book's own sort name ("Le Guin, Ursula K."), or ""
};

struct Author {
  std::string name;  // the sort name if any book of theirs gives one, else as written; "" unknown
  std::vector<Book> books;  // by title
};

class Shelf {
 public:
  Shelf() = default;
  Shelf(const Shelf &) = delete;
  Shelf &operator=(const Shelf &) = delete;
  ~Shelf() { save(); }

  // Lists the books folder `root` and looks each book up in the cache. False if root can't be
  // read (no card); the shelf is then empty.
  bool scan(const std::string &root);
  // Reads the metadata of the books the cache doesn't have, for about budgetMs at a time. True
  // once all of them are read.
  bool step(uint32_t budgetMs);
  int total() const { return static_cast<int>(books_.size()); }
  int unread() const { return unread_; }  // books step() still has to read

  // Writes the cache if anything has changed since it was read (also on destruction, so
  // metadata read before BOOT left the screen isn't read again).
  void save();

  // The books read so far, by author and then by title.
  std::vector<Author> byAuthor() const;

 private:
  struct Entry {
    Book book;
    bool read = false;  // book holds its metadata (from the cache or step())
  };
  std::vector<Entry> books_;
  int unread_ = 0;
  bool dirty_ = false;  // the cache doesn't match books_
};

}  // namespace booklib
