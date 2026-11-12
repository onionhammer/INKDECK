#include "apps/files.h"

#include <Arduino.h>
#include <stdio.h>

#include "apps/music.h"
#include "apps/reader.h"
#include "audio.h"
#include "dirlist.h"
#include "sdcard.h"

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

// EPUB and MP3 names lose their extension; the tag says what they are.
std::string displayName(const DirList::Entry &e) {
  if (!e.isDir && (DirList::isEpub(e.name) || DirList::isMp3(e.name))) {
    return e.name.substr(0, e.name.rfind('.'));
  }
  return e.name;
}

const char *verbFor(const DirList::Entry *e) {
  if (!e) return nullptr;
  if (e->isDir) return "OPEN";
  if (DirList::isEpub(e->name)) return "READ";
  if (DirList::isMp3(e->name)) return "PLAY";
  return nullptr;
}

class FilesScreen : public ui::Screen {
 public:
  FilesScreen(const std::string &dir, const std::string &select) : list_(sdcard::kMountPoint) {
    openDir(dir, select);
  }

  std::string tag() const override { return "09 FILES"; }
  std::string context() const override {
    const std::string rel = list_.relativePath();
    return rel.empty() ? "SD:/" : "SD:/" + rel + "/";
  }
  bool contextIsPath() const override { return true; }

  int hints(ui::Hint *out) const override {
    int n = 0;
    if (!list_.readable()) {
      out[n++] = {ui::glyph::kPress, "RETRY"};
    } else {
      if (!list_.entries().empty()) out[n++] = {ui::glyph::kDial, "MOVE"};
      if (const char *verb = verbFor(selected())) out[n++] = {ui::glyph::kPress, verb};
      if (audio::active()) out[n++] = {ui::glyph::kHoldPress, "PLAYER"};
    }
    out[n++] = {"BOOT", list_.atRoot() ? "BACK" : "UP"};
    return n;
  }

  bool onKey(Key key) override {
    const bool hadNotice = !notice_.empty();
    notice_.clear();
    switch (key) {
      case Key::Up: return cursor_.move(-1) || hadNotice;
      case Key::Down: return cursor_.move(+1) || hadNotice;
      case Key::Select: activate(); return true;
      case Key::SelectLong:
        if (!audio::active()) return hadNotice;
        ui::nav::push(makeMusic());
        return true;
      case Key::Back: return goUp();  // unused at the root: the navigator pops
      default: return hadNotice;
    }
  }

  // The playing-track marker and the PLAYER hint follow the audio state.
  bool onTick() override { return audio::status().generation != drawnGeneration_; }

  void draw(ui::Canvas &c, const Rect &body) override {
    drawnGeneration_ = audio::status().generation;
    Rect r = body;
    const Rect head = r.takeTop(ui::kSectionH);
    if (!notice_.empty()) {
      ui::drawNotice(c, head, notice_);
    } else {
      ui::drawSectionHeader(c, head, folderLabel(), list_.readable() ? cursor_.label() : "");
    }

    if (!list_.readable()) {
      const std::string retry = std::string(" Press ") + ui::glyph::kPress + " to retry.";
      if (sdcard::mounted()) {
        drawEmpty(c, r, "CAN'T READ", "This folder couldn't be opened." + retry);
      } else {
        drawEmpty(c, r, "NO SD CARD", "Insert a microSD card." + retry);
      }
      return;
    }
    if (list_.entries().empty()) {
      drawEmpty(c, r, "EMPTY", "Nothing in this folder.");
      return;
    }

    // Sizes cost a stat() each, so only the visible page looks them up.
    const int perPage = r.h / ui::kRowH;
    const int first = cursor_.pageStart(perPage);
    const int count = static_cast<int>(list_.entries().size());
    for (int i = first; i < count && i < first + perPage; ++i) list_.sizeOf(i);

    const std::string playing = audio::active() ? audio::status().path : std::string();
    ui::drawList(c, r, ui::kRowH, cursor_, [&](int i) {
      const DirList::Entry &e = list_.entries()[i];
      ui::Row row;
      row.title = displayName(e);
      row.tag = typeTag(e);
      row.trailing = !playing.empty() && list_.pathOf(i) == playing ? kPlayingMarker
                                                                     : sizeText(e.size);
      return row;
    });
  }

 private:
  const DirList::Entry *selected() const {
    return list_.entries().empty() ? nullptr : &list_.entries()[cursor_.cursor()];
  }

  std::string folderLabel() const {
    const std::string rel = list_.relativePath();
    if (rel.empty()) return "SD CARD";
    return ui::upper(rel.substr(rel.rfind('/') + 1));  // npos + 1 = 0: the whole name
  }

  void openDir(const std::string &dir, const std::string &select) {
    list_.open(dir);
    cursor_.reset(static_cast<int>(list_.entries().size()), list_.indexOf(select));
  }

  bool goUp() {
    if (list_.atRoot()) return false;
    const std::string child = list_.cwd().substr(list_.cwd().rfind('/') + 1);
    list_.goUp();
    cursor_.reset(static_cast<int>(list_.entries().size()), list_.indexOf(child));
    return true;
  }

  void activate() {
    if (!list_.readable()) {
      if (!sdcard::mounted()) sdcard::mount();
      if (!list_.reload()) openDir(sdcard::kMountPoint, "");
      cursor_.reset(static_cast<int>(list_.entries().size()));
      return;
    }
    const DirList::Entry *e = selected();
    if (!e) {
      openDir(list_.cwd(), "");
      return;
    }
    const int i = cursor_.cursor();
    if (e->isDir) {
      openDir(list_.pathOf(i), "");
    } else if (DirList::isMp3(e->name)) {
      play(i);
    } else if (DirList::isEpub(e->name)) {
      std::string err;
      if (auto reader = makeReader(list_.pathOf(i), err)) {
        ui::nav::push(std::move(reader));
      } else {
        log_w("open failed: %s", err.c_str());
        notice_ = "CAN'T OPEN: " + ui::upper(err);
      }
    } else {
      notice_ = "CAN'T OPEN " + typeTag(*e) + " FILES";
    }
  }

  // Plays entry i with the folder's other MP3s queued after it; opens the player.
  void play(int i) {
    const std::string path = list_.pathOf(i);
    if (!(audio::active() && audio::status().path == path)) {
      int index;
      std::vector<std::string> playlist = list_.filePaths(DirList::isMp3, i, index);
      audio::play(std::move(playlist), index);
      delay(50);  // let the audio task pick it up before the player draws
    }
    ui::nav::push(makeMusic());
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

  DirList list_;
  ui::ListCursor cursor_;
  std::string notice_;
  uint32_t drawnGeneration_ = 0;
};

}  // namespace

std::unique_ptr<ui::Screen> makeFiles(const std::string &dir, const std::string &select) {
  return std::make_unique<FilesScreen>(dir, select);
}

std::string sdFolder(const char *name) {
  const std::string root = sdcard::kMountPoint;
  if (!sdcard::mounted()) return root;
  const std::string child = DirList::findSubdir(root, name);
  return child.empty() ? root : root + "/" + child;
}

}  // namespace apps
