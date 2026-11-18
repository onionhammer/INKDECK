#include "apps/files.h"

#include <Arduino.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include <algorithm>

#include "apps/home.h"
#include "apps/lock.h"
#include "apps/music.h"
#include "apps/reader.h"
#include "apps/textview.h"
#include "apps/viewer.h"
#include "audio.h"
#include "dirlist.h"
#include "file_copy.h"
#include "flashfs.h"
#include "shares.h"
#include "podcast_library.h"
#include "sdcard.h"
#include "textdoc.h"
#include "ui/menu.h"

namespace apps {
namespace {

using ui::Key;
using ui::Rect;

const std::string kPlayingMarker = std::string("   ") + ui::glyph::kPlay;  // size-wide

// Four characters, right-aligned ("999B", "412K", "1.2M", " 18M"), so the type tags line up
// in the monospace font.
std::string sizeText(int64_t bytes) {
  char buf[16];
  if (bytes < 0) return "    ";
  if (bytes < 1000) {
    snprintf(buf, sizeof buf, "%3dB", static_cast<int>(bytes));
    return buf;
  }
  static const char kUnits[] = "KMGT";
  double v = bytes / 1024.0;
  int unit = 0;
  while (v >= 999.5 && unit < 3) {
    v /= 1024.0;
    ++unit;
  }
  snprintf(buf, sizeof buf, v < 9.95 ? "%3.1f%c" : "%3.0f%c", v, kUnits[unit]);
  return buf;
}

std::string extensionOf(const std::string &name) {
  const size_t dot = name.rfind('.');
  return dot == std::string::npos || dot == 0 ? std::string() : name.substr(dot + 1);
}

std::string typeTag(const DirList::Entry &e) {
  if (e.isDir) return "DIR";
  const std::string ext = extensionOf(e.name);
  return ext.empty() ? "FILE" : ui::upper(ext.substr(0, 4));
}

// Names of files we open lose their extension; the tag says what they are.
std::string displayName(const DirList::Entry &e) {
  if (!e.isDir &&
      (DirList::isEpub(e.name) || DirList::isAudio(e.name) || DirList::isPicture(e.name))) {
    return e.name.substr(0, e.name.rfind('.'));
  }
  return e.name;
}

const char *verbFor(const DirList::Entry *e) {
  if (!e) return nullptr;
  if (e->isDir) return "OPEN";
  if (DirList::isEpub(e->name)) return "READ";
  if (DirList::isAudio(e->name)) return "PLAY";
  if (DirList::isPicture(e->name)) return "VIEW";
  return nullptr;
}

// path is dir or somewhere inside it. FAT ignores case, so this does too.
bool isInside(const std::string &path, const std::string &dir) {
  if (path.size() < dir.size() || strncasecmp(path.c_str(), dir.c_str(), dir.size()) != 0) {
    return false;
  }
  return path.size() == dir.size() || path[dir.size()] == '/';
}

std::string nameOf(const std::string &path) { return path.substr(path.rfind('/') + 1); }
std::string folderOf(const std::string &path) { return path.substr(0, path.rfind('/')); }

// HOLD ● SAVE on a share: where a file can go on the card, by its type. Each is a top-level
// folder of the card's, made if it's missing.
enum class Library : uint8_t { Music, Podcasts, Pictures, Documents, Books };

// The folder's name as it's made; an existing one in any case is used instead.
const char *libraryName(Library l) {
  switch (l) {
    case Library::Music: return "Music";
    case Library::Podcasts: return "Podcasts";
    case Library::Pictures: return "Pictures";
    case Library::Documents: return "Documents";
    case Library::Books: return "Books";
  }
  return "";
}

// The card's top-level folder `name`, in any case, or where it would be made.
std::string libraryFolder(Library l) {
  const std::string root = sdcard::kMountPoint;
  const std::string found = DirList::findSubdir(root, libraryName(l));
  return root + "/" + (found.empty() ? std::string(libraryName(l)) : found);
}

// A name FAT takes: \ / : * ? " < > | and control characters become _, and trailing dots and
// spaces go (an NFS export, or a Samba share, can have them).
std::string fatName(const std::string &name) {
  std::string out;
  for (const char ch : name) {
    out += static_cast<uint8_t>(ch) < 0x20 || strchr("\\/:*?\"<>|", ch) ? '_' : ch;
  }
  while (!out.empty() && (out.back() == '.' || out.back() == ' ')) out.pop_back();
  return out.empty() ? "_" : out;
}

// The name a file of `size` bytes takes in folder: name, or "name (2).ext" and on when a
// different file (or a folder) has it. already: a file of that name and size is there, most
// likely saved before; that name is returned.
std::string freeName(const std::string &folder, const std::string &name, int64_t size,
                     bool &already) {
  const std::string ext = extensionOf(name);
  const std::string stem = ext.empty() ? name : name.substr(0, name.size() - ext.size() - 1);
  already = false;
  std::string candidate = name;
  for (int n = 2; n <= 100; ++n) {
    struct stat st;
    if (stat((folder + "/" + candidate).c_str(), &st) != 0) break;
    if (!S_ISDIR(st.st_mode) && size >= 0 && st.st_size == size) {
      already = true;
      break;
    }
    candidate = stem + " (" + std::to_string(n) + ")" + (ext.empty() ? "" : "." + ext);
  }
  return candidate;
}

// The last max characters of text (UTF-8), with an ellipsis in front if it's longer.
std::string tail(const std::string &text, size_t max) {
  if (text.size() <= max) return text;
  size_t from = text.size() - max;
  while (from < text.size() && (static_cast<uint8_t>(text[from]) & 0xC0) == 0x80) ++from;
  return ui::glyph::kEllipsis + text.substr(from);
}

// Which file system a Files screen browses. The card's root lists the onboard flash and then
// each network share (SMB, NFS) as extra first rows; each opens a Files screen of its own (BOOT
// at its root comes back).
enum class Volume : uint8_t { Sd, Flash, Net };

std::string rootOf(Volume v, int share) {
  switch (v) {
    case Volume::Sd: return sdcard::kMountPoint;
    case Volume::Flash: return flashfs::kMountPoint;
    case Volume::Net: break;
  }
  return shares::all()[share].mountPath();
}

class FilesScreen : public ui::Screen {
 public:
  // share: which of shares::all() a Volume::Net screen browses. floor: a folder on the card
  // this screen stays in (openLibrary()); "" for Files proper, behind the PIN.
  FilesScreen(Volume volume, int share, const std::string &dir, const std::string &select,
              const std::string &floor = "")
      : volume_(volume), list_(rootOf(volume, share)), floor_(floor) {
    if (volume_ != Volume::Net) {
      openDir(dir, select);
      return;
    }
    // A share is listed once it's mounted: at once if it is already, else after a CONNECTING
    // header while a task joins Wi-Fi and mounts it (onTick() watches).
    lease_ = std::make_unique<shares::Lease>(share);
    pendingDir_ = dir;
    pendingSelect_ = select;
    connect();
  }

  std::string tag() const override { return moduleTag("FILES"); }
  std::string context() const override {
    // "SD:/", "SD:/Books/", "SMB:/Media/Music/"
    const std::string dir = list_.cwd().empty() ? rootOf(volume_, shareIndex()) : list_.cwd();
    const std::string shown = displayPath(dir);
    return shown.back() == '/' ? shown : shown + "/";
  }
  bool contextIsPath() const override { return true; }

  // Back from the picture viewer: the cursor on the last picture it showed. Files proper may
  // get the PIN's wheels in front of it here, if it hasn't been in use for a minute.
  void onEnter() override {
    if (!confined()) guardedOnTop(Guarded::Files);
    if (viewed_.empty()) return;
    resetCursor(viewed_);
    viewed_.clear();
  }

  int hints(ui::Hint *out) const override {
    int n = 0;
    if (!pending_.path.empty()) return 0;  // deleting: no keys until it's done
    if (saving_) {
      out[n++] = {"BOOT", "STOP"};
      return n;
    }
    if (connecting()) {
      // nothing to do but wait, or go back
    } else if (!list_.readable()) {
      out[n++] = {ui::glyph::kPress, "RETRY"};
    } else {
      if (rowCount() > 0) out[n++] = {ui::glyph::kDial, "MOVE"};
      if (extraRow() >= 0) {
        out[n++] = {ui::glyph::kPress, "OPEN"};
      } else if (const char *verb = verbFor(selected())) {
        out[n++] = {ui::glyph::kPress, verb};
      } else if (isText(entryIndex())) {
        out[n++] = {ui::glyph::kPress, "READ"};
      }
      if (canDelete()) {
        out[n++] = {ui::glyph::kHoldPress, "DELETE"};
      } else if (canSave()) {
        out[n++] = {ui::glyph::kHoldPress, "SAVE"};
      }
    }
    out[n++] = {"BOOT", atTop() ? "BACK" : "UP"};
    return n;
  }

  // Covered (a file opened, a sheet), popped, or gone home: Files was in use until now.
  void onLeave() override {
    if (!confined()) guardedLeft(Guarded::Files);
  }

  bool onKey(Key key) override {
    if (!confined()) guardedInUse(Guarded::Files);
    if (!pending_.path.empty()) return key == Key::Back;  // BOOT mustn't pop it mid-delete
    if (saving_) {  // BOOT stops the copy; onTick() sees it end
      if (key == Key::Back) filecopy::cancel();
      return key == Key::Back;
    }
    if (connecting()) return false;  // BOOT goes back; the mount carries on and is let go
    const bool hadNotice = !notice_.empty();
    notice_.clear();
    switch (key) {
      case Key::Up: return cursor_.move(-1) || hadNotice;
      case Key::Down: return cursor_.move(+1) || hadNotice;
      case Key::Select: activate(); return true;
      case Key::SelectLong:
        if (canDelete()) {
          confirmDelete(entryIndex());
        } else if (canSave()) {
          chooseSave(entryIndex());
        } else {
          return hadNotice;
        }
        return true;
      case Key::Back: return goUp();  // unused at the top: the navigator pops
      default: return hadNotice;
    }
  }

  // A folder delete runs on the tick after its DELETING header is on the panel; a share's
  // screen lists it once the mount finishes; a save's header follows its percent until it ends.
  // Otherwise the playing-track marker follows the audio state.
  bool onTick() override {
    if (!pending_.path.empty()) {
      if (!pending_.shown) return false;
      const Pending p = pending_;
      pending_ = {};
      deleteNow(p.path, true, p.row);
      return true;
    }
    if (saving_) {
      const filecopy::Status s = filecopy::status();
      if (s.state == filecopy::State::Copying) return progressText(s) != drawnProgress_;
      saving_ = false;
      finishSave(s);
      return true;
    }
    if (lease_ && waitingForMount_) {
      const shares::Lease::State s = lease_->state();
      if (s == shares::Lease::State::Connecting) return false;
      waitingForMount_ = false;
      if (s == shares::Lease::State::Ready) openDir(pendingDir_, pendingSelect_);
      return true;
    }
    return audio::status().generation != drawnGeneration_;
  }

  void draw(ui::Canvas &c, const Rect &body) override {
    drawnGeneration_ = audio::status().generation;
    Rect r = body;
    const Rect head = r.takeTop(ui::kSectionH);
    if (connecting()) {
      const shares::Share &share = shares::all()[shareIndex()];
      ui::drawSectionHeader(c, head, "CONNECTING" + std::string(ui::glyph::kEllipsis), "");
      drawEmpty(c, r, ui::upper(share.name).c_str(),
                "Joining the Wi-Fi network and connecting to " + share.url() +
                    ". This can take a few seconds.");
      return;
    }
    if (!pending_.path.empty()) {
      ui::drawSectionHeader(c, head,
                            "DELETING " + ui::upper(nameOf(pending_.path)) + ui::glyph::kEllipsis,
                            itemsText(pending_.items), {true});
      pending_.shown = true;
    } else if (saving_) {
      const filecopy::Status s = filecopy::status();
      drawnProgress_ = progressText(s);
      ui::drawSectionHeader(c, head,
                            "SAVING " + ui::upper(nameOf(s.dst)) + ui::glyph::kEllipsis,
                            drawnProgress_, {true});
    } else if (!notice_.empty()) {
      ui::drawNotice(c, head, notice_);
    } else {
      ui::drawSectionHeader(c, head, folderLabel(), list_.readable() ? cursor_.label() : "");
    }

    if (!list_.readable()) {
      const std::string retry = std::string(" Press ") + ui::glyph::kPress + " to retry.";
      if (volume_ == Volume::Net && lease_->state() == shares::Lease::State::Failed) {
        const shares::Share &share = shares::all()[shareIndex()];
        drawEmpty(c, r, "CAN'T CONNECT",
                  ui::upper(lease_->error()) + ". " + share.url() + "." + retry);
      } else if (volume_ == Volume::Net) {
        drawEmpty(c, r, "CAN'T READ", "This folder couldn't be read from the server." + retry);
      } else if (volume_ == Volume::Flash) {
        drawEmpty(c, r, "CAN'T MOUNT", "The onboard FAT partition couldn't be mounted." + retry);
      } else if (sdcard::mounted()) {
        drawEmpty(c, r, "CAN'T READ", "This folder couldn't be opened." + retry);
      } else {
        drawEmpty(c, r, "NO SD CARD", "Insert a microSD card." + retry);
      }
      return;
    }
    if (rowCount() == 0) {
      drawEmpty(c, r, "EMPTY",
                volume_ == Volume::Flash && list_.atRoot()
                    ? "Nothing on the onboard flash yet. Copy files here with the !cp dev command."
                    : "Nothing in this folder.");
      return;
    }

    // Sizes cost a stat() each, so only the visible page looks them up.
    const int perPage = r.h / ui::kRowH;
    const int first = cursor_.pageStart(perPage);
    const int extra = extraRows();
    for (int i = first; i < rowCount() && i < first + perPage; ++i) {
      if (i >= extra) list_.sizeOf(i - extra);
    }

    const std::string playing = audio::active() ? audio::status().path : std::string();
    ui::drawList(c, r, ui::kRowH, cursor_, [&](int i) {
      ui::Row row;
      if (i < flashRows()) {
        row.title = "ONBOARD FLASH";
        row.tag = "FAT";
        row.trailing = sizeText(static_cast<int64_t>(flashfs::partitionBytes()));
        return row;
      }
      if (i < extra) {
        row.title = shares::all()[i - flashRows()].name;
        row.tag = shares::all()[i - flashRows()].tag();
        row.trailing = sizeText(-1);  // blank, as wide as a size
        return row;
      }
      const DirList::Entry &e = list_.entries()[i - extra];
      row.title = displayName(e);
      row.tag = typeTag(e);
      row.trailing = !playing.empty() && list_.pathOf(i - extra) == playing ? kPlayingMarker
                                                                             : sizeText(e.size);
      return row;
    });
  }

 private:
  // A library browser (openLibrary()): stays in floor_, deletes nothing, needs no PIN.
  bool confined() const { return !floor_.empty(); }
  // As far up as this screen goes: its volume's root, or the floor.
  bool atTop() const { return confined() ? list_.cwd().size() <= floor_.size() : list_.atRoot(); }
  bool onCardRoot() const {
    return !confined() && volume_ == Volume::Sd && list_.atRoot() && list_.readable();
  }
  // The ONBOARD FLASH row: first on the card's root, when the partition exists.
  int flashRows() const { return onCardRoot() && flashfs::available() ? 1 : 0; }
  // A row per network share (SD:/inkdeck.ini), after it.
  int shareRows() const { return onCardRoot() ? static_cast<int>(shares::all().size()) : 0; }
  int extraRows() const { return flashRows() + shareRows(); }
  int rowCount() const { return static_cast<int>(list_.entries().size()) + extraRows(); }
  // Which extra row the cursor is on (0 = the first), or -1.
  int extraRow() const { return cursor_.cursor() < extraRows() ? cursor_.cursor() : -1; }
  // Index into list_.entries() of the cursor's row; -1 on an extra row or an empty list.
  int entryIndex() const {
    const int i = cursor_.cursor() - extraRows();
    return i >= 0 && i < static_cast<int>(list_.entries().size()) ? i : -1;
  }
  int shareIndex() const { return lease_ ? lease_->share() : -1; }
  // HOLD ● deletes the selection: an entry of the card's or the flash's, not a share's (read-only).
  bool canDelete() const { return selected() && volume_ != Volume::Net && !confined(); }
  // On a share, HOLD ● saves the selection to the card instead: a file of a type the card has a
  // folder for.
  bool canSave() const { return volume_ == Volume::Net && !librariesFor(entryIndex()).empty(); }
  // A share's screen before its mount has finished: nothing listed yet.
  bool connecting() const { return lease_ && lease_->state() == shares::Lease::State::Connecting; }
  const DirList::Entry *selected() const {
    const int i = entryIndex();
    return i < 0 ? nullptr : &list_.entries()[i];
  }

  std::string folderLabel() const {
    const std::string rel = list_.relativePath();
    if (rel.empty() && volume_ == Volume::Net) return ui::upper(shares::all()[shareIndex()].name);
    if (rel.empty()) return volume_ == Volume::Sd ? "SD CARD" : "ONBOARD FLASH";
    return ui::upper(rel.substr(rel.rfind('/') + 1));  // npos + 1 = 0: the whole name
  }

  // Puts the cursor on the entry called `name`, or the first row.
  void resetCursor(const std::string &name) {
    const int i = list_.indexOf(name);
    cursor_.reset(rowCount(), i >= 0 ? i + extraRows() : 0);
  }

  void openDir(const std::string &dir, const std::string &select) {
    if (volume_ == Volume::Flash) flashfs::mount();
    list_.open(dir);
    resetCursor(select);
  }

  bool goUp() {
    if (atTop()) return false;
    const std::string child = list_.cwd().substr(list_.cwd().rfind('/') + 1);
    list_.goUp();
    resetCursor(child);
    return true;
  }

  // Starts mounting the share (again, after a failure); lists it at once if it's mounted.
  void connect() {
    lease_->start();
    const shares::Lease::State s = lease_->state();
    waitingForMount_ = s == shares::Lease::State::Connecting;
    if (s == shares::Lease::State::Ready) openDir(pendingDir_, pendingSelect_);
  }

  void activate() {
    if (!list_.readable()) {
      if (volume_ == Volume::Net && lease_->state() != shares::Lease::State::Ready) {
        connect();
        return;
      }
      if (volume_ == Volume::Flash) {
        flashfs::mount();
      } else if (volume_ == Volume::Sd && !sdcard::mounted()) {
        sdcard::mount();
      }
      if (!list_.reload()) openDir(confined() ? floor_ : rootOf(volume_, shareIndex()), "");
      resetCursor("");
      return;
    }
    if (const int extra = extraRow(); extra >= 0) {
      if (extra < flashRows()) {
        ui::nav::push(std::make_unique<FilesScreen>(Volume::Flash, -1, flashfs::kMountPoint, ""));
      } else {
        const int share = extra - flashRows();
        const std::string root = shares::all()[share].mountPath();
        ui::nav::push(std::make_unique<FilesScreen>(Volume::Net, share, root, ""));
      }
      return;
    }
    const DirList::Entry *e = selected();
    if (!e) {
      openDir(list_.cwd(), "");
      return;
    }
    const int i = entryIndex();
    if (e->isDir) {
      openDir(list_.pathOf(i), "");
    } else if (DirList::isAudio(e->name)) {
      play(i);
    } else if (DirList::isPicture(e->name)) {
      view(i);
    } else if (DirList::isEpub(e->name)) {
      std::string err;
      if (auto reader = makeReader(list_.pathOf(i), err)) {
        ui::nav::push(std::move(reader));
      } else {
        log_w("open failed: %s", err.c_str());
        notice_ = "CAN'T OPEN: " + ui::upper(err);
      }
    } else if (isText(i)) {
      std::string err;
      if (auto reader = makeTextReader(list_.pathOf(i), err)) {
        ui::nav::push(std::move(reader));
      } else {
        log_w("open failed: %s", err.c_str());
        notice_ = "CAN'T OPEN: " + ui::upper(err);
      }
    } else {
      notice_ = "CAN'T OPEN " + typeTag(*e) + " FILES";
    }
  }

  // HOLD ●: a sheet that asks before entry i goes, CANCEL first. Next to DELETE it shows a
  // file's size, or how much a folder holds (it all goes with it).
  void confirmDelete(int i) {
    const DirList::Entry &e = list_.entries()[i];
    const std::string path = list_.pathOf(i);
    const bool isDir = e.isDir;
    const int items = isDir ? DirList::countInside(path) : 0;
    std::string detail;
    if (isDir) {
      detail = itemsText(items);
    } else {
      detail = sizeText(list_.sizeOf(i));
      detail.erase(0, detail.find_first_not_of(' '));
    }
    const std::string title = (isDir ? "DELETE FOLDER " : "DELETE ") + ui::upper(e.name) + "?";
    const int row = cursor_.cursor();
    ui::nav::push(std::make_unique<ui::MenuScreen>(
        title, std::vector<ui::MenuItem>{
                   {"CANCEL", nullptr, [] { ui::nav::pop(); }},
                   {"DELETE", [detail] { return detail; },
                    [this, path, isDir, items, row] {
                      ui::nav::pop();
                      startDelete(path, isDir, items, row);
                    }},
               }));
  }

  // "47 ITEMS INSIDE", "EMPTY", or "CAN'T READ IT" (-1): what a folder holds.
  static std::string itemsText(int n) {
    if (n < 0) return "CAN'T READ IT";
    if (n == 0) return "EMPTY";
    return std::to_string(n) + (n == 1 ? " ITEM INSIDE" : " ITEMS INSIDE");
  }

  // A file goes now. A folder can hold hundreds of files at a few ms each, so one with anything
  // in it goes on the next tick, once the DELETING header is on the panel.
  void startDelete(const std::string &path, bool isDir, int items, int row) {
    // The podcast worker writes downloads into the Podcasts folder while it runs.
    if (::podcasts::busy() && isInside(path, ::podcasts::kRoot)) {
      notice_ = "PODCASTS ARE DOWNLOADING; TRY LATER";
      return;
    }
    // A save from a share that carries on behind this screen has its .part open.
    if (filecopy::busy() && isInside(filecopy::status().dst + ".part", path)) {
      notice_ = "A SAVE IS WRITING THERE; TRY LATER";
      return;
    }
    if (isDir && items > 0) {
      pending_ = {path, items, row, false};
    } else {
      deleteNow(path, isDir, row);
    }
  }

  // Deletes path (all of it, for a folder), then lists the folder again with the cursor on the
  // row that took its place.
  void deleteNow(const std::string &path, bool isDir, int row) {
    // FAT here has no open-file lock: deleting a file the player has open can corrupt the card.
    if (audio::active() && isInside(audio::status().path, path)) {
      audio::stop();
      const uint32_t t0 = millis();
      while (audio::active() && millis() - t0 < 2000) delay(10);
      if (audio::active()) {
        notice_ = "CAN'T STOP THE PLAYER";
        return;
      }
    }
    log_i("files: deleting %s", path.c_str());
    if (!DirList::removeTree(path)) {
      notice_ = (isDir ? "COULDN'T DELETE ALL OF " : "CAN'T DELETE ") + ui::upper(nameOf(path));
    }
    sniffedPath_.clear();
    list_.reload();
    cursor_.reset(rowCount(), std::max(0, std::min(row, rowCount() - 1)));
  }

  // Plays entry i with the folder's other MP3s queued after it; opens the player.
  void play(int i) {
    const std::string path = list_.pathOf(i);
    if (!(audio::active() && audio::status().path == path)) {
      int index;
      std::vector<std::string> playlist = list_.filePaths(DirList::isAudio, i, index);
      audio::play(std::move(playlist), index);
      delay(50);  // let the audio task pick it up before the player draws
    }
    ui::nav::push(makeMusic());
  }

  // Any other file opens in the text reader if it reads as text. The answer for the selected
  // file is kept, so the hint bar doesn't read the card on every redraw.
  bool isText(int i) const {
    if (i < 0 || list_.entries()[i].isDir) return false;
    const std::string path = list_.pathOf(i);
    if (path != sniffedPath_) {
      sniffedPath_ = path;
      sniffedText_ = TextDoc::looksLikeText(path);
    }
    return sniffedText_;
  }

  // Where entry i can be saved on the card: an MP3 in Music or Podcasts, a WAV in Music, a
  // picture in Pictures, an EPUB in Books, anything else that reads as text in Documents.
  // Nowhere for a folder, or a file the board can't open.
  std::vector<Library> librariesFor(int i) const {
    if (i < 0 || list_.entries()[i].isDir) return {};
    const std::string &name = list_.entries()[i].name;
    if (DirList::isMp3(name)) return {Library::Music, Library::Podcasts};
    if (DirList::isAudio(name)) return {Library::Music};
    if (DirList::isPicture(name)) return {Library::Pictures};
    if (DirList::isEpub(name)) return {Library::Books};
    if (isText(i)) return {Library::Documents};
    return {};
  }

  // The folder a save to lib goes in. Pictures, Documents and Books: the library's own. Music and
  // Podcasts keep where the file was: below a folder of the share's named like the library, the
  // folders under it (music/Artist/Album -> Music/Artist/Album), else the folder it's in (the
  // album). A podcast keeps only the first of those, its show (podcasts/Show/episodes ->
  // Podcasts/Show), since a show is one folder with its MP3s in it; with none, the share's name.
  std::string saveFolder(Library lib) const {
    const std::string folder = libraryFolder(lib);
    if (lib != Library::Music && lib != Library::Podcasts) return folder;
    const std::vector<std::string> dirs = shareDirs();
    const auto named = namedFolder(dirs, lib);
    std::vector<std::string> keep;
    if (named != dirs.end()) {
      keep.assign(named + 1, dirs.end());
    } else if (!dirs.empty()) {
      keep.push_back(dirs.back());
    }
    if (lib == Library::Podcasts) {
      if (keep.size() > 1) keep.resize(1);
      if (keep.empty()) keep.push_back(shares::all()[shareIndex()].name);
    }
    std::string out = folder;
    for (const std::string &d : keep) out += "/" + fatName(d);
    return out;
  }

  // The share's folders from its root down to this one ("podcasts/Show/episodes").
  std::vector<std::string> shareDirs() const {
    std::vector<std::string> dirs;
    const std::string rel = list_.relativePath();
    for (size_t at = 0; at < rel.size();) {
      size_t slash = rel.find('/', at);
      if (slash == std::string::npos) slash = rel.size();
      if (slash > at) dirs.push_back(rel.substr(at, slash - at));
      at = slash + 1;
    }
    return dirs;
  }

  // The first of dirs named like lib, in any case (the share's own Music or Podcasts), or end.
  static std::vector<std::string>::const_iterator namedFolder(const std::vector<std::string> &dirs,
                                                              Library lib) {
    return std::find_if(dirs.begin(), dirs.end(), [lib](const std::string &d) {
      return strcasecmp(d.c_str(), libraryName(lib)) == 0;
    });
  }

  // This folder is in, or is, a folder of the share's named like lib.
  bool pathNames(Library lib) const {
    const std::vector<std::string> dirs = shareDirs();
    return namedFolder(dirs, lib) != dirs.end();
  }

  // HOLD ● on a share's file: a sheet of the card's folders it can go to, each with where it
  // lands, or ALREADY THERE when a file of its name and size is.
  void chooseSave(int i) {
    if (!sdcard::mounted()) {
      notice_ = "NO SD CARD";
      return;
    }
    const DirList::Entry &e = list_.entries()[i];
    const std::string src = list_.pathOf(i);
    const int64_t size = list_.sizeOf(i);
    std::vector<ui::MenuItem> items;
    int first = 0;  // the cursor starts on a library the share's path names (podcasts/...)
    for (const Library lib : librariesFor(i)) {
      if (pathNames(lib)) first = static_cast<int>(items.size());
      const std::string folder = saveFolder(lib);
      bool already;
      const std::string dst = folder + "/" + freeName(folder, fatName(e.name), size, already);
      const std::string label = ui::upper(libraryName(lib));
      const std::string where = already ? "ALREADY THERE" : tail(displayPath(folder) + "/", 26);
      items.push_back({label, [where] { return where; }, [this, src, dst, size, already, label] {
                         ui::nav::pop();
                         if (already) {
                           notice_ = "ALREADY IN " + label;
                         } else {
                           startSave(src, dst, size);
                         }
                       }});
    }
    ui::nav::push(std::make_unique<ui::MenuScreen>("SAVE " + ui::upper(e.name) + " TO",
                                                   std::move(items), first));
  }

  // The copy runs on its own task; the header shows how far it has got (onTick()).
  void startSave(const std::string &src, const std::string &dst, int64_t size) {
    std::string error;
    if (!filecopy::start(src, dst, size, error)) {
      notice_ = "CAN'T SAVE: " + error;
      return;
    }
    saving_ = true;
    drawnProgress_.clear();
  }

  // The copy has ended: where it went, why it failed, or that it was stopped.
  void finishSave(const filecopy::Status &s) {
    switch (s.state) {
      case filecopy::State::Done:
        notice_ = "SAVED TO " + ui::upper(displayPath(folderOf(s.dst)));
        break;
      case filecopy::State::Failed: notice_ = "CAN'T SAVE: " + s.error; break;
      case filecopy::State::Stopped: notice_ = "STOPPED; NOTHING SAVED"; break;
      default: break;
    }
  }

  // The SAVING header's right side: "42% · 3 MIN", the time left once a few seconds have shown
  // the rate (in tens of seconds under a minute and a half), or what's written so far if the
  // size isn't known.
  static std::string progressText(const filecopy::Status &s) {
    if (s.total <= 0) {
      std::string t = sizeText(s.done);
      t.erase(0, t.find_first_not_of(' '));
      return t;
    }
    std::string t = std::to_string(static_cast<int>(s.done * 100 / s.total)) + "%";
    if (s.done > 0 && s.done < s.total && s.elapsedMs >= 3000) {
      const int64_t left = (s.total - s.done) * s.elapsedMs / s.done / 1000;  // seconds
      t += std::string(" ") + ui::glyph::kDot + " " +
           (left >= 90 ? std::to_string((left + 30) / 60) + " MIN"
                       : std::to_string(std::max<int64_t>(10, (left + 9) / 10 * 10)) + " S");
    }
    return t;
  }

  // Shows picture i full screen; the dial steps through the folder's other pictures.
  void view(int i) {
    int index;
    std::vector<std::string> pictures = list_.filePaths(DirList::isPicture, i, index);
    ui::nav::push(makeViewer(std::move(pictures), index, [this](const std::string &path) {
      viewed_ = path.substr(path.rfind('/') + 1);
    }));
  }

  static void drawEmpty(ui::Canvas &c, const Rect &r, const char *title,
                        const std::string &text) {
    const Rect p = r.inset(20, 24);
    int y = p.y + ui::capHeight(ui::style::kTitle);
    ui::drawText(c, p.x, y, title, ui::style::kTitle);
    y += 24;
    for (const auto &line : ui::wrap(ui::style::kBody, text, p.w, 4)) {
      y += 22;
      ui::drawText(c, p.x, y, line, ui::style::kBody);
    }
  }

  Volume volume_;
  // A share's screen: its hold on the mount, and where to start once it's mounted.
  std::unique_ptr<shares::Lease> lease_;
  std::string pendingDir_;
  std::string pendingSelect_;
  bool waitingForMount_ = false;
  DirList list_;
  std::string floor_;  // confined(): the folder it stays in
  ui::ListCursor cursor_;
  std::string notice_;
  std::string viewed_;  // name of the picture the viewer showed last, until we're back
  mutable std::string sniffedPath_;  // isText()'s last answer
  mutable bool sniffedText_ = false;
  uint32_t drawnGeneration_ = 0;
  // A save to the card (HOLD ● SAVE) is running: the header shows it, and BOOT stops it.
  bool saving_ = false;
  std::string drawnProgress_;  // what the header's right side shows: percent, time left
  // A folder waiting to be deleted on the next tick.
  struct Pending {
    std::string path;   // "" = none
    int items = 0;      // what it holds, for the header
    int row = 0;        // the cursor's row when it was chosen
    bool shown = false;  // the DELETING header has been drawn
  };
  Pending pending_;
};

}  // namespace

std::unique_ptr<ui::Screen> makeFiles(const std::string &dir, const std::string &select) {
  const std::string flash = flashfs::kMountPoint;
  if (dir.compare(0, flash.size(), flash) == 0) {
    return std::make_unique<FilesScreen>(Volume::Flash, -1, dir, select);
  }
  if (const int share = shares::shareOf(dir); share >= 0) {
    return std::make_unique<FilesScreen>(Volume::Net, share, dir, select);
  }
  return std::make_unique<FilesScreen>(Volume::Sd, -1, dir, select);
}

void openLibrary(const char *name, const std::string &dir, const std::string &select,
                 bool replace) {
  const std::string folder = sdFolder(name);  // the card's root if there's no such folder
  const std::string at = dir.empty() ? folder : dir;
  if (folder != sdcard::kMountPoint && isInside(at, folder)) {
    auto screen = std::make_unique<FilesScreen>(Volume::Sd, -1, at, select, folder);
    if (replace) {
      ui::nav::replace(std::move(screen));
    } else {
      ui::nav::push(std::move(screen));
    }
    return;
  }
  openGuarded(Guarded::Files, [at, select] { return makeFiles(at, select); }, replace);
}

std::string displayPath(const std::string &path) {
  // A share shows as its protocol: /net/Media/x -> SMB:/Media/x.
  const int share = shares::shareOf(path);
  const std::string net = share >= 0 ? std::string(shares::all()[share].tag()) + ":" : "NET:";
  const struct {
    const char *root;
    const char *shown;
  } kRoots[] = {{sdcard::kMountPoint, "SD:"},
                {flashfs::kMountPoint, "FLASH:"},
                {shares::kMountPoint, net.c_str()}};
  for (const auto &r : kRoots) {
    const size_t n = strlen(r.root);
    if (path.compare(0, n, r.root) == 0 && (path.size() == n || path[n] == '/')) {
      return r.shown + (path.size() == n ? std::string("/") : path.substr(n));
    }
  }
  return path;
}

std::string sdFolder(const char *name) {
  const std::string root = sdcard::kMountPoint;
  if (!sdcard::mounted()) return root;
  const std::string child = DirList::findSubdir(root, name);
  return child.empty() ? root : root + "/" + child;
}

}  // namespace apps
