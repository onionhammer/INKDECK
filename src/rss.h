// Podcast RSS: the channel's title and description and its newest episodes, read from an RSS 2.0
// document that may be cut off part-way (only the first items are downloaded). A forgiving
// scanner, not a full XML parser: it finds elements by name, reads CDATA and entities, and turns
// HTML descriptions into plain text.
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

}  // namespace rss
