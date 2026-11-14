// Files: browse the microSD card and the onboard FAT partition (an ONBOARD FLASH row first on
// the card's root). Opens EPUBs in the reader, plays MP3s, queueing the rest of the folder's
// MP3s after the chosen one, shows JPEG and PNG pictures full screen (apps/viewer.h), and opens
// any other file that reads as text in the text reader (apps/textview.h).
//
// The design canvas has no Files artboard, so this follows Home's list language:
//   - one row per entry: name, a type tag (DIR, EPUB, MP3, ...), and the size;
//   - the path in the status bar and the folder name in the section header;
//   - the ● verb in the hint bar follows the selection (OPEN / READ / PLAY / VIEW).
#pragma once
#include <memory>
#include <string>

#include "ui/nav.h"

namespace apps {

// Starts in dir, an absolute path under sdcard::kMountPoint (or flashfs::kMountPoint), with the
// cursor on the entry named select if given.
std::unique_ptr<ui::Screen> makeFiles(const std::string &dir, const std::string &select = "");

// The card's top-level folder called `name` in any case ("books" -> "/sdcard/Books"), or the
// card root if there isn't one.
std::string sdFolder(const char *name);

}  // namespace apps
