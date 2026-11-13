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

}  // namespace covers
