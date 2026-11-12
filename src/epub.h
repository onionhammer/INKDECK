// EPUB: container.xml -> OPF (title, author, manifest, spine) -> per-chapter XHTML converted to
// flow text (see text.h). Quick-and-dirty string scanning, not a real XML parser.
// Ignores CSS, images (shown as "[image]"), tables and the TOC.
#pragma once
#include <string>
#include <vector>

#include "zip.h"

class Epub {
 public:
  bool open(const std::string &path, std::string &err);
  void close();

  const std::string &title() const { return title_; }
  const std::string &author() const { return author_; }
  size_t chapterCount() const { return spine_.size(); }
  const std::string &chapterPath(size_t i) const { return spine_[i]; }

  // Loads spine item i and converts it to flow text.
  bool chapterFlow(size_t i, std::string &flow, std::string &err) const;

 private:
  ZipFile zip_;
  std::string title_;
  std::string author_;
  std::vector<std::string> spine_;  // zip entry names, reading order
};

// Exposed for reuse/testing: XHTML (UTF-8) -> flow text.
void xhtmlToFlow(const std::string &xhtml, std::string &flow);
