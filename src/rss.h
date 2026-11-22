// Feeds: a podcast's RSS (the channel's title and description and its newest episodes), and
// the articles of a news feed (parseNews(), below), read from a document that may be cut off
// part-way (only the first items are downloaded). A forgiving scanner, not a full XML parser: it
// finds elements by name, reads CDATA and entities, and turns HTML into plain text or Markdown.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include <string>
#include <vector>

namespace rss {

struct Episode {
  std::string guid;   // <guid>, else the enclosure URL
  std::string title;
  std::string url;    // the enclosure: the audio file
  std::string type;   // its MIME type ("audio/mpeg")
  std::string description;  // plain text, shortened
  time_t published = 0;     // <pubDate>, UTC
  uint32_t seconds = 0;     // <itunes:duration>; 0 if not given
  int64_t bytes = 0;        // the enclosure's length attribute (often 0 or wrong)
  int number = -1;          // <itunes:episode>
};

struct Channel {
  std::string title;
  std::string description;
  std::vector<Episode> episodes;  // in feed order (newest first in nearly every feed)
};

// Reads up to maxEpisodes items that have an enclosure. False if there's no channel title and
// no episode (not a feed).
bool parse(const char *xml, size_t len, int maxEpisodes, Channel &out);

// Helpers, public for testing.
time_t parseDate(const std::string &rfc822);         // 0 if unreadable
uint32_t parseDuration(const std::string &text);     // "1:02:03", "48:12", "2892"
std::string plainText(const std::string &html, size_t maxBytes);  // tags out, entities decoded

// --- News (news_library.h): articles from RSS 2.0, RSS 1.0 (RDF) and Atom feeds, any item,
// with or without an enclosure.

struct Story {
  std::string id;       // <guid> or <id>, else the link, else the title
  std::string title;    // plain text
  std::string link;     // the article on the web
  std::string author;   // <dc:creator>, <author> (its name, not an e-mail address)
  std::string summary;  // plain text, shortened: the description, else the start of the content
  std::string body;     // the whole article as Markdown for NoteDoc (notedoc.h): the content
                        // (<content:encoded>, Atom's <content>), else the description
  bool full = false;    // body is the content, not only the description
  time_t published = 0; // <pubDate>, <dc:date>, <published>, else <updated>; UTC
};

struct NewsFeed {
  std::string title;
  std::string description;
  const char *kind = "RSS";  // "RSS", "RDF" or "ATOM"
  std::vector<Story> stories;  // in feed order (newest first in nearly every feed)
};

// Reads up to maxStories items or entries, each body cut to about maxBody bytes at a paragraph.
// False if there's no title and no story (not a feed).
bool parseNews(const char *xml, size_t len, int maxStories, size_t maxBody, NewsFeed &out);

// RFC 3339 / ISO 8601: "2026-11-21T14:05:00Z", "2026-11-21T14:05:00.000+01:00",
// "2026-11-21" (midnight UTC). 0 if unreadable.
time_t parseIsoDate(const std::string &text);

// HTML to the Markdown NoteDoc reads: paragraphs and line breaks kept, headings as "## ",
// list items as "- " or "1. ", quotes as "> ", rules as "---"; scripts, styles and images go;
// entities decoded, whitespace collapsed. Cut at a paragraph once past maxBytes. With a
// linkBase (the page's URL), links stay as [text](url), resolved against it; without, only
// their text.
std::string markdownFromHtml(const std::string &html, size_t maxBytes,
                             const std::string &linkBase = std::string());

// href (an attribute's value, entities decoded) as an absolute http(s) URL, resolved against
// base; "" for a fragment, another scheme (javascript:, mailto:) or nothing.
std::string resolveUrl(const std::string &base, const std::string &href);

}  // namespace rss
