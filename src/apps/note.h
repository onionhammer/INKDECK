// Note: a long answer the agent put on the screen (its show_note tool,
// apps/board_tools.h: a recipe, the steps of a how-to, a packing list), paged like the Reader so
// it can be followed with the board on the counter. Layout and the Markdown it understands:
// notedoc.h.
//   ▲ previous page   ▼ next page   ● menu   BOOT back
// The body follows the Reader artboard: a "§ NOTE - - - TODAY 14:32" line (when it was made), the
// page, and a progress strip ("P. 2/5 [=====:::::] 40%"). The menu has text size (the Reader's
// Literata sizes, one setting for every note), back to the first page, and the folder.
//
// Each note is kept as Markdown, "# Title" and then the text, in the Notes folder: SD:/Notes, or
// FLASH:/Notes without a card. A note with the same title replaces it. Files opens a .md file in
// any folder called Notes here instead of in the text reader, so they can be read again
// without the assistant; the agent's tool show {screen: "note"} brings back the last one. The
// place is kept per note (readstate.h). Counts as reading: the board light-sleeps between pages,
// and the idle power-off waits for a low battery, as in the text reader.
#pragma once
#include <time.h>

#include <memory>
#include <string>

#include "ui/nav.h"

namespace apps {

struct Note {
  std::string title;  // UTF-8, one line
  std::string text;   // Markdown, without the title
  std::string path;   // the file it's kept in; empty if it couldn't be kept
  time_t made = 0;    // when it was saved (the file's time)
  bool cut = false;   // longer than kMaxNoteBytes: cut off, with a line saying so
};

constexpr size_t kMaxNoteBytes = 16 * 1024;  // of a note's text from the assistant

// Keeps note in the Notes folder (made if missing), replacing one with the same title, and makes
// it the last note, to open at its start. A text longer than kMaxNoteBytes is cut at a line
// break first, and ends with a line saying so (note.cut). Sets note.path and note.made; false
// with err set if it can't be written (the note is still whole enough to show).
bool saveNote(Note &note, std::string &err);

// Reads a note file: a first "# Title" line is the title, else the file's name. False with err
// set if it can't be read.
bool readNote(const std::string &path, Note &out, std::string &err);

// The last note saved or opened, if its file is still there.
bool lastNote(Note &out);

// The note on screen (the top screen that isn't a sheet), or nullptr.
const Note *noteOnScreen();

// A .md file in a folder called Notes, in any case: Files opens it as a note.
bool isNotePath(const std::string &path);

// The Note screen. fromFiles: under Files' tag, and the menu's FOLDER goes back to the Files
// screen below; else under the Agent's, and FOLDER opens the Notes folder in its place. atStart:
// the first page, not the saved place.
std::unique_ptr<ui::Screen> makeNote(Note note, bool fromFiles, bool atStart);

}  // namespace apps
