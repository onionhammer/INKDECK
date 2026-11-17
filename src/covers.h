// Cover thumbnails (Home's READER preview): a book's cover picture (Epub::coverPath()) fitted
// into a small box and dithered once, then kept on the SD card in /sdcard/.inkdeck/covers/,
// so Home doesn't open the EPUB and decode a JPEG on every boot. The last one is also kept in
// RAM, since Home redraws often. Books without a cover we can decode are remembered too.
#pragma once
#include <string>

#include "picture.h"

namespace covers {

// The thumbnail for the book at bookPath, at most maxW x maxH with the cover's aspect kept.
// The first call for a book renders it (a few hundred ms); later ones read the cache. False if
// the book has no cover we can decode, or there's no card.
bool thumbnail(const std::string &bookPath, int maxW, int maxH, picture::Bitmap &out);

// The cover at full size, for a w x h area (the closed book, apps/closed_book.h): scaled to
// fill it when that crops at most an eighth of the cover's width or height (the bitmap is then
// larger than the area: draw it centred and clipped), otherwise fitted inside. Rendered from
// the EPUB each time (not cached). False as for thumbnail().
bool full(const std::string &bookPath, int w, int h, picture::Bitmap &out);

}  // namespace covers
