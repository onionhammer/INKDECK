// News (the "News" design canvas, docs/ui-design.md): RSS and Atom feeds, read offline. The
// library is news_library.h; its off face is apps/news_edition.h.
//
//   Stories:  every feed's stories in one list, newest first, grouped by day: a solid square and
//             a bold headline for a new one, SAVED outlined. A card under the list shows the
//             selected story's whole headline and summary; a strip at the foot, the last sync.
//             ▲▼ move   ● read   HOLD ● menu   BOOT back
//             The menu (// NEWS): SAVE STORY, SHOW (ALL / NEW / SAVED), SYNC NOW, FEEDS,
//             MARK ALL READ.
//   Story:    the Note screen's body (notedoc.h): the headline in 28 px capitals, a byline, then
//             the text in Literata a page at a time. Opening it marks it read. A feed that
//             gives only a summary and a link: the linked page, read into an article
//             (page_fetch.h, article.h), the summary while it comes, the fetch's progress in
//             the strip.
//             ▲ previous page   ▼ next page, past the last the next story   ● menu   BOOT back
//             The menu (// STORY): SAVE, WHOLE STORY, LINKS, TEXT SIZE, MARK UNREAD, MORE FROM
//             the feed, TO START.
//   Links:    the links in a story's text (the feed's, then its page's).
//             ▲▼ move   ● open   BOOT back
//   Link:     a linked page, read as the story's is. One level: its links don't open.
//             ▲ previous page   ▼ next page   ● menu (TRY AGAIN, TEXT SIZE, TO START)   BOOT back
//   Feeds:    ALL FEEDS, then each feed with its new stories, ! for one whose last sync failed;
//             a card for the selection.
//             ▲▼ move   ● its stories   HOLD ● sync it (ALL FEEDS: every feed)   BOOT back
//
// A News screen opening syncs the feeds when the last sync is half an hour old; while one is
// open, and on USB power, they sync every hour (pollNews()).
#pragma once
#include <stdint.h>

#include <memory>

#include "ui/nav.h"

namespace apps {

std::unique_ptr<ui::Screen> makeNews();

// Pushes News's story list and, over it, story `uid` (the one on the off face; 0: the list
// alone).
void openNewsStory(uint32_t uid);

// From loop(): News's hourly syncs, while a News screen is open or the board is on USB power.
void pollNews();

// A News screen is the base (under a sheet or menu too): PWR shows its off face.
bool newsOnScreen();

}  // namespace apps
