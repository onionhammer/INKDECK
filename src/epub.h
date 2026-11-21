// EPUB: container.xml -> OPF (title, author, manifest, spine, table of contents) -> per-chapter
// XHTML converted to flow text (see text.h). Quick-and-dirty string scanning, not a real XML
// parser. Ignores tables, and CSS except display:none (elements the book hides, like the
// "Cover" heading over a cover picture). Pictures become flow::kImage paragraphs pointing into
// the chapter's picture list (SVG wrappers around an <image> included); the Reader draws them.
//
// "Chapter" here means a spine item (one XHTML file), which is what the reader pages through.
#pragma once
#include <stdint.h>

#include <map>
#include <string>
#include <vector>

#include "zip.h"

// A selector from a CSS rule that says display:none, kept only if it's a compound this can
// match: an optional tag, classes, an id, and one [class^= / *= / ~= / =] test (books use
// those for their "Nonprinting" classes). Combinators, pseudo-classes and rules inside @media
// are ignored.
struct HiddenSelector {
  std::string tag;  // lower-case; "" for any
  std::vector<std::string> classes;
  std::string id;
  char classOp = 0;  // '^', '*', '~', '=' or 0 for none
  std::string classValue;
};

// The display:none selectors in a stylesheet.
std::vector<HiddenSelector> hiddenSelectors(const std::string &css);

// What the Reader's library sorts by (book_library.h), from the OPF's metadata: the first title
// and the first author (a creator with the role "aut" or none; failing that, the first creator),
// each with the sort form the book gives, or "" where it gives none: EPUB 2's opf:file-as, an
// EPUB 3 file-as <meta> refining it, or calibre's title_sort. All UTF-8.
struct EpubMeta {
  std::string title;
  std::string titleSort;   // "Dispossessed, The"
  std::string author;      // "Ursula K. Le Guin"
  std::string authorSort;  // "Le Guin, Ursula K."
};

// Reads only container.xml and the OPF, so it's much quicker than Epub::open(). False with err
// set if the file isn't an EPUB this can read. title is "" if the book names none.
bool readEpubMeta(const std::string &path, EpubMeta &out, std::string &err);

class Epub {
 public:
  struct TocEntry {
    std::string title;  // UTF-8
    size_t chapter;     // spine index it points at
  };

  struct Image {
    std::string path;  // zip entry
    std::string alt;   // UTF-8, may be empty
  };

  bool open(const std::string &path, std::string &err);
  void close();

  const std::string &title() const { return title_; }    // UTF-8
  const std::string &author() const { return author_; }  // UTF-8
  // Zip entry of the cover picture, or "" if the book names none (see open()).
  const std::string &coverPath() const { return cover_; }
  size_t chapterCount() const { return spine_.size(); }
  const std::string &chapterPath(size_t i) const { return spine_[i].path; }

  // Table of contents (EPUB 2 NCX or EPUB 3 nav), flattened, in reading order. May be empty.
  const std::vector<TocEntry> &toc() const { return toc_; }
  // Title of the TOC entry chapter i belongs to (the last one at or before it), or "".
  std::string chapterTitle(size_t i) const;

  // Uncompressed size of chapter i and of all chapters before it, for whole-book progress.
  uint32_t chapterSize(size_t i) const { return spine_[i].size; }
  uint64_t sizeBefore(size_t i) const;
  uint64_t totalSize() const { return sizeBefore(spine_.size()); }

  // Loads spine item i and converts it to flow text; images gets its pictures, in flow order.
  bool chapterFlow(size_t i, std::string &flow, std::vector<Image> &images,
                   std::string &err) const;

  // Any file in the book (a picture), whole.
  bool readFile(const std::string &path, std::string &out, std::string &err) const;

 private:
  struct Chapter {
    std::string path;  // zip entry name
    uint32_t size;     // uncompressed bytes
  };

  void parseNcx(const std::string &xml, const std::string &dir);
  void parseNav(const std::string &xml, const std::string &dir);
  void addTocEntry(const std::string &title, const std::string &dir, const std::string &href);

  // The display:none selectors of the chapter's stylesheets and <style> blocks.
  std::vector<HiddenSelector> chapterHidden(const std::string &xhtml,
                                            const std::string &dir) const;

  ZipFile zip_;
  std::string title_;
  std::string author_;
  std::string cover_;
  std::vector<Chapter> spine_;  // reading order
  std::vector<TocEntry> toc_;
  mutable std::map<std::string, std::vector<HiddenSelector>> css_;  // per stylesheet file
};

// Exposed for reuse/testing: XHTML (UTF-8) -> flow text. With images, pictures are collected
// (paths resolved against baseDir, the chapter's folder with a trailing '/'); without, they
// become "[image]" placeholders. Elements matching hidden, or with an inline display:none or a
// hidden attribute, are left out.
void xhtmlToFlow(const std::string &xhtml, std::string &flow,
                 std::vector<Epub::Image> *images = nullptr, const std::string &baseDir = {},
                 const std::vector<HiddenSelector> *hidden = nullptr);
