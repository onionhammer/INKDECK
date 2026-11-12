// EPUB: container.xml -> OPF (title, author, manifest, spine, table of contents) -> per-chapter
// XHTML converted to flow text (see text.h). Quick-and-dirty string scanning, not a real XML
// parser. Ignores CSS, images (shown as "[image]") and tables.
//
// "Chapter" here means a spine item (one XHTML file), which is what the reader pages through.
#pragma once
#include <stdint.h>

#include <string>
#include <vector>

#include "zip.h"

class Epub {
 public:
  struct TocEntry {
    std::string title;  // UTF-8
    size_t chapter;     // spine index it points at
  };

  bool open(const std::string &path, std::string &err);
  void close();

  const std::string &title() const { return title_; }    // UTF-8
  const std::string &author() const { return author_; }  // UTF-8
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

  // Loads spine item i and converts it to flow text.
  bool chapterFlow(size_t i, std::string &flow, std::string &err) const;

 private:
  struct Chapter {
    std::string path;  // zip entry name
    uint32_t size;     // uncompressed bytes
  };

  void parseNcx(const std::string &xml, const std::string &dir);
  void parseNav(const std::string &xml, const std::string &dir);
  void addTocEntry(const std::string &title, const std::string &dir, const std::string &href);

  ZipFile zip_;
  std::string title_;
  std::string author_;
  std::vector<Chapter> spine_;  // reading order
  std::vector<TocEntry> toc_;
};

// Exposed for reuse/testing: XHTML (UTF-8) -> flow text.
void xhtmlToFlow(const std::string &xhtml, std::string &flow);
