// Files: browse the microSD card, the onboard FAT partition (an ONBOARD FLASH row first on the
// card's root) and network shares, SMB and NFS (a row each after it, from SD:/inkdeck.ini;
// shares.h).
// Opens EPUBs in the reader, plays MP3s, queueing the rest of the folder's MP3s after the
// chosen one, shows JPEG and PNG pictures full screen (apps/viewer.h), opens a Markdown file in
// a folder called Notes as a note (apps/note.h: what the voice assistant put on the screen), and
// any other file that reads as text in the text reader (apps/textview.h).
//
// The design canvas has no Files artboard, so this follows Home's list language:
//   - one row per entry: name, a type tag (DIR, EPUB, MP3, ...), and the size;
//   - the path in the status bar and the folder name in the section header;
//   - the ● verb in the hint bar follows the selection (OPEN / READ / PLAY / VIEW);
//   - HOLD ● deletes the selection, after a sheet that asks (CANCEL first) and shows the file's
//     size or how many items a folder holds. A folder goes with everything in it, under an
//     inverted DELETING header. The player stops first if it has a file in there open; inside
//     SD:/Podcasts it waits until no download is running. (HOLD BOOT raises the player sheet while it plays.)
// A share's screen shows a CONNECTING header while it joins Wi-Fi and mounts the share, then
// lists it like the card; if that fails it says why, and ● tries again. Shares are read-only:
// no HOLD ● DELETE there. HOLD ● SAVE copies the selected file to the card instead (file_copy.h),
// into a top-level folder by its type, after a sheet of the choices (`// SAVE NAME.MP3 TO`):
//   MP3 -> MUSIC or PODCASTS, WAV -> MUSIC, JPEG/PNG -> PICTURES, EPUB -> BOOKS, any other file
//   that reads as text -> DOCUMENTS.
// Each row shows where it lands (SD:/Podcasts/Show/), or ALREADY THERE for a file of the same
// name and size; another file of that name makes it "name (2).ext". Music keeps the folders
// below the share's own music folder (else the album's), a podcast its show's (the folder below
// the share's podcasts folder; else the one it's in, or the share's name at its root). The cursor
// starts on the library the share's path names. While it copies, an inverted
// `// SAVING NAME.MP3…` header shows the percent and the time left, and only BOOT works: it
// stops the copy. The result comes up as a notice (SAVED TO SD:/PODCASTS/SHOW, CAN'T SAVE: ...).
//
// Home opens it behind the PIN when the build asks for one there (INKDECK_PIN_ON's files) and
// Files wasn't in use in the last minute, and the PIN comes back in front of it after a minute
// away or without a key (apps/lock.h openGuarded()). Music gets a confined browser instead
// (openLibrary()); the Reader has its own library (apps/library.h).
#pragma once
#include <memory>
#include <string>

#include "ui/nav.h"

namespace apps {

// Starts in dir, an absolute path under sdcard::kMountPoint (or flashfs::kMountPoint, or a
// share's shares::Share::mountPath()), with the cursor on the entry named select if given.
std::unique_ptr<ui::Screen> makeFiles(const std::string &dir, const std::string &select = "");

// Music's and the notes' way onto the card: its top-level folder `name` ("music", "notes"),
// at dir inside it (default: the folder itself) with the cursor on select. That browser stays in
// the folder (BOOT at its top goes back), deletes nothing, has no ONBOARD FLASH or share rows,
// and needs no PIN. If the card has no such folder, or dir isn't inside it, this opens Files
// proper at dir (the card's root by default), behind the PIN as Home's FILES: what the Reader's
// library does for a book outside SD:/Books. replace: in place of the top screen (a note's
// FOLDER, the Reader's LIBRARY), else pushed.
void openLibrary(const char *name, const std::string &dir = "", const std::string &select = "",
                 bool replace = false);

// The card's top-level folder called `name` in any case ("books" -> "/sdcard/Books"), or the
// card root if there isn't one.
std::string sdFolder(const char *name);

// A path as the status bar shows it: "/sdcard/Books" -> "SD:/Books", "/ffat" -> "FLASH:/",
// "/net/Media/x" -> "SMB:/Media/x" (or NFS:, by the share).
std::string displayPath(const std::string &path);

}  // namespace apps
