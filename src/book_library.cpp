#include "book_library.h"

#include <Arduino.h>
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <algorithm>
#include <map>
#include <utility>

#include "cache_file.h"
#include "dirlist.h"
#include "epub.h"
#include "paths.h"
#include "sdcard.h"
#include "text.h"

namespace booklib {
namespace {

constexpr int kMaxDepth = 3;       // folders below the books folder
constexpr int kMaxVisited = 3000;  // entries looked at, at most
constexpr size_t kMaxBooks = 400;
constexpr const char *kCacheHeader = "INKDECK-BOOKS 1";  // bump on format changes

std::string cacheDir() { return std::string(sdcard::kMountPoint) + "/.inkdeck"; }
std::string cachePath() { return cacheDir() + "/books.tsv"; }

void collect(const std::string &dir, int depth, std::vector<std::string> &out, int &visited) {
  DIR *d = opendir(dir.c_str());
  if (!d) return;
  while (dirent *e = readdir(d)) {
    if (++visited > kMaxVisited || out.size() >= kMaxBooks) break;
    if (e->d_name[0] == '.') continue;  // hidden, and macOS's "._" files
    const std::string path = dir + "/" + e->d_name;
    if (e->d_type == DT_DIR) {
      if (depth < kMaxDepth) collect(path, depth + 1, out, visited);
    } else if (DirList::isEpub(e->d_name)) {
      out.push_back(path);
    }
  }
  closedir(d);
}

// The file name without its extension: the title of a book that gives none.

// Metadata attributes can hold tabs and line breaks, which would split the cache's fields.
using cachefile::field;
using cachefile::split;

// The cache: path -> book with its metadata.
std::map<std::string, Book> loadCache() {
  std::map<std::string, Book> out;
  std::string data;
  if (!cachefile::read(cachePath(), data, 1024 * 1024)) return out;
  const std::vector<std::string> lines = split(data, '\n');
  if (lines.empty() || lines[0] != kCacheHeader) return out;
  for (size_t i = 1; i < lines.size(); ++i) {
    const std::vector<std::string> cols = split(lines[i], '\t');
    if (cols.size() != 6 || cols[0].empty()) continue;  // a torn last line, say
    Book b;
    b.path = cols[0];
    b.size = strtoll(cols[1].c_str(), nullptr, 10);
    b.title = cols[2];
    b.titleSort = cols[3];
    b.author = cols[4];
    b.authorSort = cols[5];
    out[b.path] = std::move(b);
  }
  return out;
}

// --- Order ----------------------------------------------------------------------------------

// Latin-1 letters U+00C0..U+00FF without their accents, in lower case (× and ÷ are spaces).
const char *const kLatin1[64] = {
    "a", "a", "a", "a", "a", "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i", "i",   // C0
    "d", "n", "o", "o", "o", "o", "o",  " ", "o", "u", "u", "u", "u", "y", "th", "ss",  // D0
    "a", "a", "a", "a", "a", "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i", "i",   // E0
    "d", "n", "o", "o", "o", "o", "o",  " ", "o", "u", "u", "u", "u", "y", "th", "y",   // F0
};

// What names and titles sort by: lower case, Latin-1 accents off, apostrophes gone ("O'Brian"
// next to "Obrian"), other punctuation as one space between words.
std::string sortKey(const std::string &s) {
  std::string out;
  bool space = false;
  auto put = [&](const char *chars) {
    if (space && !out.empty()) out += ' ';
    space = false;
    out += chars;
  };
  for (size_t i = 0; i < s.size();) {
    const uint32_t cp = text::nextCodepoint(s, i);
    if (cp == '\'' || cp == 0x2019) continue;
    if ((cp >= '0' && cp <= '9') || (cp >= 'a' && cp <= 'z')) {
      const char ch[2] = {static_cast<char>(cp), 0};
      put(ch);
    } else if (cp >= 'A' && cp <= 'Z') {
      const char ch[2] = {static_cast<char>(cp - 'A' + 'a'), 0};
      put(ch);
    } else if (cp >= 0xC0 && cp <= 0xFF && kLatin1[cp - 0xC0][0] != ' ') {
      put(kLatin1[cp - 0xC0]);
    } else if (cp >= 0x100) {  // other scripts: as they are, after the Latin letters
      std::string utf8;
      text::appendUtf8(utf8, cp);
      put(utf8.c_str());
    } else {
      space = true;
    }
  }
  return out;
}

// Which author a name is: its sort key without the spaces, so "LeGuin" and "Le Guin" meet.
std::string authorKey(const std::string &name) {
  std::string key = sortKey(name);
  key.erase(std::remove(key.begin(), key.end(), ' '), key.end());
  return key;
}

// A title's key: the book's sort title, or the title without a leading article.
std::string titleKey(const Book &b) {
  if (!b.titleSort.empty()) return sortKey(b.titleSort);
  std::string key = sortKey(b.title);
  for (const char *article : {"the ", "a ", "an "}) {
    const size_t n = strlen(article);
    if (key.size() > n && key.compare(0, n, article) == 0) return key.substr(n);
  }
  return key;
}

// Byte order, except that runs of digits compare as numbers ("book 2" before "book 10").
int naturalCompare(const std::string &a, const std::string &b) {
  size_t i = 0, j = 0;
  while (i < a.size() && j < b.size()) {
    if (isdigit(static_cast<unsigned char>(a[i])) && isdigit(static_cast<unsigned char>(b[j]))) {
      size_t ie = i, je = j;
      while (ie < a.size() && isdigit(static_cast<unsigned char>(a[ie]))) ++ie;
      while (je < b.size() && isdigit(static_cast<unsigned char>(b[je]))) ++je;
      while (i + 1 < ie && a[i] == '0') ++i;  // leading zeros
      while (j + 1 < je && b[j] == '0') ++j;
      if (ie - i != je - j) return ie - i < je - j ? -1 : 1;
      if (const int c = a.compare(i, ie - i, b, j, je - j)) return c < 0 ? -1 : 1;
      i = ie;
      j = je;
      continue;
    }
    if (a[i] != b[j]) return static_cast<uint8_t>(a[i]) < static_cast<uint8_t>(b[j]) ? -1 : 1;
    ++i;
    ++j;
  }
  const size_t restA = a.size() - i, restB = b.size() - j;
  return restA == restB ? 0 : (restA < restB ? -1 : 1);
}

}  // namespace

bool Shelf::scan(const std::string &root) {
  const uint32_t t0 = millis();
  books_.clear();
  unread_ = 0;
  dirty_ = false;
  DIR *d = opendir(root.c_str());
  if (!d) return false;
  closedir(d);

  std::vector<std::string> paths;
  int visited = 0;
  collect(root, 0, paths, visited);
  std::map<std::string, Book> cache = loadCache();
  size_t matched = 0;
  for (const std::string &path : paths) {
    Entry e;
    struct stat st;
    e.book.size = stat(path.c_str(), &st) == 0 ? static_cast<int64_t>(st.st_size) : -1;
    const auto it = cache.find(path);
    if (it != cache.end() && it->second.size == e.book.size) {
      e.book = it->second;
      e.read = true;
      ++matched;
    } else {
      e.book.path = path;
      ++unread_;
    }
    books_.push_back(std::move(e));
  }
  dirty_ = unread_ > 0 || matched != cache.size();  // new or changed books, or some gone
  log_i("books: %u in %s (%d to read), %lu ms", static_cast<unsigned>(books_.size()),
        root.c_str(), unread_, static_cast<unsigned long>(millis() - t0));
  return true;
}

bool Shelf::step(uint32_t budgetMs) {
  const uint32_t t0 = millis();
  for (Entry &e : books_) {
    if (unread_ == 0) break;
    if (e.read) continue;
    EpubMeta meta;
    std::string err;
    if (!readEpubMeta(e.book.path, meta, err)) {
      // Listed under its file name, and not tried again until it changes.
      log_w("books: %s: %s", e.book.path.c_str(), err.c_str());
    }
    e.book.title = meta.title.empty() ? paths::stem(e.book.path) : meta.title;
    e.book.titleSort = meta.titleSort;
    e.book.author = meta.author;
    e.book.authorSort = meta.authorSort;
    e.read = true;
    --unread_;
    dirty_ = true;
    if (millis() - t0 >= budgetMs) break;  // at least one book a call
  }
  return unread_ == 0;
}

void Shelf::save() {
  if (!dirty_ || !sdcard::mounted()) return;
  std::string out = std::string(kCacheHeader) + "\n";
  for (const Entry &e : books_) {
    if (!e.read) continue;
    const Book &b = e.book;
    out += field(b.path) + "\t" + std::to_string(b.size) + "\t" + field(b.title) + "\t" +
           field(b.titleSort) + "\t" + field(b.author) + "\t" + field(b.authorSort) + "\n";
  }
  if (cachefile::writeWhole(cachePath(), out)) dirty_ = false;
}

std::vector<Author> Shelf::byAuthor() const {
  // Sort names, for the books of the same author that don't give one.
  std::map<std::string, std::string> sortNames;  // authorKey(as written) -> sort name
  for (const Entry &e : books_) {
    if (e.read && !e.book.author.empty() && !e.book.authorSort.empty()) {
      sortNames.emplace(authorKey(e.book.author), e.book.authorSort);
    }
  }

  struct Group {
    std::string key;  // sortKey(name); "" for no author
    Author author;
    std::vector<std::pair<std::string, const Book *>> books;  // titleKey, book
  };
  std::vector<Group> groups;
  std::map<std::string, size_t> groupOf;  // authorKey(name) -> index into groups
  for (const Entry &e : books_) {
    if (!e.read) continue;
    const Book &b = e.book;
    std::string name = b.authorSort;
    if (name.empty() && !b.author.empty()) {
      const auto it = sortNames.find(authorKey(b.author));
      name = it != sortNames.end() ? it->second : b.author;
    }
    const std::string key = authorKey(name);
    const auto found = groupOf.emplace(key, groups.size());
    if (found.second) {
      Group g;
      g.key = key.empty() ? "" : sortKey(name);
      g.author.name = key.empty() ? "" : name;
      groups.push_back(std::move(g));
    }
    groups[found.first->second].books.push_back({titleKey(b), &b});
  }

  std::sort(groups.begin(), groups.end(), [](const Group &a, const Group &b) {
    if (a.key.empty() != b.key.empty()) return b.key.empty();  // no author: last
    return naturalCompare(a.key, b.key) < 0;
  });
  std::vector<Author> out;
  out.reserve(groups.size());
  for (Group &g : groups) {
    std::sort(g.books.begin(), g.books.end(), [](const auto &a, const auto &b) {
      const int c = naturalCompare(a.first, b.first);
      return c != 0 ? c < 0 : a.second->path < b.second->path;
    });
    for (const auto &kb : g.books) g.author.books.push_back(*kb.second);
    out.push_back(std::move(g.author));
  }
  return out;
}

}  // namespace booklib
