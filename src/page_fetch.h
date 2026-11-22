// Web pages read into articles (article.h), for News: the whole story when a feed gives only a
// summary and a link, and the pages its links lead to (apps/news.h).
//
// One page at a time, on a background task. With inkharness configured (agent_client.h), it's
// asked first: the daemon fetches the page and reads it with Mozilla's Readability (its
// reader.rs), which knows far more sites than the board's own reading. If the daemon isn't
// there to ask (or is from before `read`), the board does it itself: Wi-Fi up (wlan.h), the page
// downloaded into PSRAM (3 MB at most) and read (article.h). A site that turned the daemon away
// isn't tried again from here. inkharness sends the article's pictures too, dithered to 1 bit
// for the size asked. Either way the article is kept on the card in SD:/.inkdeck/news/pages/,
// a file a URL (and one for its pictures), so it opens offline afterwards (the newest 200 are
// kept). Wi-Fi stays up 20 s after a page, for the next link. Asking for another page abandons
// the one under way.
#pragma once
#include <stdint.h>

#include <string>

#include "article.h"

namespace pagefetch {

enum class Phase : uint8_t { Idle, Asking, Joining, Loading, Reading, Done, Failed };

struct Status {
  Phase phase = Phase::Idle;
  uint32_t bytes = 0;      // Loading: downloaded so far
  int64_t total = -1;      // the page's length, if the server gave it
  uint32_t startedMs = 0;  // millis() when the fetch began
  std::string error;       // Failed: why, in a few words ("HTTP 403", "NOT AN ARTICLE")

  // 0..1 across asking inkharness, or joining, loading and reading, for a progress bar. A page
  // of unknown length counts as about 600 KB.
  float progress() const;
};

// The article at url as kept on the card from an earlier fetch. False if there's none.
bool cached(const std::string &url, article::Article &out);

// Starts fetching url, unless it's the page already under way or done. pictureW x pictureH:
// the largest picture to show (inkharness scales them to fit); 0 for none.
void fetch(const std::string &url, uint16_t pictureW = 0, uint16_t pictureH = 0);
// How the fetch of url goes: Idle if url isn't the page being or last fetched.
Status status(const std::string &url);
// Done: the article (also kept on the card).
bool result(const std::string &url, article::Article &out);

void cancel();  // abandons the fetch under way
bool busy();    // the task is running (Wi-Fi in use)

}  // namespace pagefetch
