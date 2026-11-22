// The article in a web page, as Markdown for NoteDoc (notedoc.h): what News opens when a feed
// gives only a summary and a link (apps/news.h, page_fetch.h).
//
// A small take on Readability's method, on the page as it is (no DOM is built). One pass
// tokenizes the HTML, keeping a stack of open elements: each element gets its visible text,
// the share of it inside links, its commas and paragraphs. Every paragraph of 25 characters or
// more scores 1, plus its commas, plus one a 100 characters (at most 3), and passes it up to its
// ancestors: all to the parent, half to the grandparent, a sixth to the next, and so on, five
// levels up. Class, id, role and data-component names weigh in ("article", "content" +25,
// "comment", "promo" -25), and elements that are never the text (scripts, menus, share bars,
// bylines, captions, video players, asides) are left out of everything. The element with the
// best score, discounted by its link density, is the article, with any sibling that scores
// nearly as well.
//
// Its HTML then goes to rss::markdownFromHtml() without what's inside it that isn't text:
// figures, the parts left out above, and blocks that are mostly links (related stories).
// Links stay, as [text](url) resolved against the page. A page whose text comes out short
// tries the JSON-LD articleBody some sites carry. The title, byline, site name and date come
// from the page's meta tags (Open Graph, article:*), else its <title> and first <time>.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include <string>
#include <vector>

namespace article {

// A picture in an article (inkharness sends them: page_fetch.h), 1 bit a pixel: rows of
// (width + 7) / 8 bytes, the leftmost pixel in the top bit, 1 for ink. The text shows it where
// ![](inkimg:index) stands.
struct Picture {
  uint32_t index = 0;
  uint16_t width = 0, height = 0;
  std::vector<uint8_t> bits;
};

struct Article {
  std::string title;     // plain text
  std::string byline;    // "" if the page doesn't say
  std::string site;      // og:site_name, else the host ("bbc.co.uk")
  time_t published = 0;  // UTC; 0 if the page doesn't say
  std::string markdown;  // the text, links as [text](url)
  std::vector<Picture> pictures;  // inkharness's only: the board's own reading has none
};

// The article in html (len bytes; the whole page). url: where it came from, for links and the
// site's name. The text is cut at a paragraph past maxBytes. False if nothing reads as an
// article (under 200 characters of text).
bool extract(const char *html, size_t len, const std::string &url, size_t maxBytes, Article &out);

// "https://www.bbc.co.uk/news/x" -> "bbc.co.uk"; "" if it isn't a URL.
std::string host(const std::string &url);

}  // namespace article
