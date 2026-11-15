#include "apps/updater.h"

#include <stdio.h>

#include <algorithm>

#include "apps/home.h"
#include "ota.h"
#include "sysinfo.h"
#include "wlan.h"

namespace apps {
namespace {

using ui::Key;
using ui::Rect;
using ui::TextStyle;

constexpr TextStyle kHeadline = ui::style::kTitle;
constexpr TextStyle kRowValue{&ui::fonts::kMono14Bold, 0};
constexpr TextStyle kCommand{&ui::fonts::kMono14Bold, 0};
constexpr TextStyle kNote{&ui::fonts::kMono14Medium, 0};

constexpr int kRowH = 26;
constexpr int kRowKeyW = 104;
constexpr int kNoteH = 22;
constexpr int kCells = 10;

std::string megabytes(uint32_t bytes) {
  char buf[16];
  snprintf(buf, sizeof buf, "%.2f", bytes / 1048576.0);
  return buf;
}

int percentOf(const ota::Status &s) {
  if (s.phase == ota::Phase::Done) return 100;
  return s.total ? static_cast<int>(static_cast<uint64_t>(s.bytes) * 100 / s.total) : 0;
}

const char *phaseName(ota::Phase p) {
  switch (p) {
    case ota::Phase::Joining: return "JOINING";
    case ota::Phase::Listening: return "LISTENING";
    case ota::Phase::Receiving: return "RECEIVING";
    case ota::Phase::Done: return "DONE";
    case ota::Phase::Failed: return "FAILED";
    case ota::Phase::Idle: break;
  }
  return "STOPPED";
}

// The big line: what matters most in each phase.
std::string headline(const ota::Status &s) {
  switch (s.phase) {
    case ota::Phase::Joining: return "JOINING\xE2\x80\xA6";
    case ota::Phase::Listening: return std::to_string((s.secondsLeft + 59) / 60) + " MIN LEFT";
    case ota::Phase::Receiving: return std::to_string(percentOf(s)) + "%";
    case ota::Phase::Done: return "UPDATED";
    case ota::Phase::Failed: return "NO UPDATE";
    case ota::Phase::Idle: break;
  }
  return "STOPPED";
}

// Under it: what's happening, or what went wrong.
std::string detail(const ota::Status &s) {
  switch (s.phase) {
    case ota::Phase::Joining: return std::string("joining ") + wlan::network();
    case ota::Phase::Listening:
      return s.result.empty() ? "waiting for an upload from the computer" : "last try: " + s.result;
    case ota::Phase::Receiving:
      return megabytes(s.bytes) + " / " + megabytes(s.total) + " MB into the idle slot";
    case ota::Phase::Done: return "restarting into the new build\xE2\x80\xA6";
    case ota::Phase::Failed: return s.result;
    case ota::Phase::Idle: break;
  }
  return "\xE2\x97\x8F listens again";
}

class UpdaterScreen : public ui::Screen {
 public:
  UpdaterScreen() { ota::start(); }
  ~UpdaterScreen() override { ota::stop(); }

  std::string tag() const override { return moduleTag("SYSTEM"); }
  std::string context() const override { return "UPDATE OVER WI-FI"; }

  int hints(ui::Hint *out) const override {
    switch (ota::status().phase) {
      case ota::Phase::Receiving:
        out[0] = {"BOOT", ui::glyph::kEmDash};  // can't cancel mid-transfer
        return 1;
      case ota::Phase::Done: return 0;
      case ota::Phase::Idle:
      case ota::Phase::Failed:
        out[0] = {ui::glyph::kPress, "LISTEN"};
        out[1] = {"BOOT", "BACK"};
        return 2;
      default:
        out[0] = {"BOOT", "CANCEL"};
        return 1;
    }
  }

  bool onKey(Key key) override {
    const ota::Phase p = ota::status().phase;
    switch (key) {
      case Key::Select:
        if (p != ota::Phase::Idle && p != ota::Phase::Failed) return false;
        ota::start();
        return true;
      case Key::Back:
        // Not while an image is arriving or the board is about to restart into it.
        return p == ota::Phase::Receiving || p == ota::Phase::Done;
      default: return false;
    }
  }

  // Redraws when the phase, the 5 % step, the minutes left or the message change. Once the
  // DONE frame is on the panel (this tick comes after its refresh), the board may restart.
  bool onTick() override {
    if (doneDrawn_) ota::doneShown();
    return signature(ota::status()) != drawnSignature_;
  }

  void draw(ui::Canvas &c, const Rect &body) override {
    const ota::Status s = ota::status();
    drawnSignature_ = signature(s);
    doneDrawn_ = s.phase == ota::Phase::Done;

    Rect r = body;
    const Rect firmware = r.takeBottom(ui::kRule + ui::kSectionH + 4 + 2 * kRowH + 6);
    const Rect upload = r.takeBottom(ui::kRule + ui::kSectionH + 8 + 3 * kNoteH + 8);
    const Rect address = r.takeBottom(ui::kRule + ui::kSectionH + 4 + 3 * kRowH + 6);
    drawState(c, r, s);
    drawAddress(c, address, s);
    drawUpload(c, upload);
    drawFirmware(c, firmware);
  }

 private:
  static std::string signature(const ota::Status &s) {
    std::string sig = phaseName(s.phase);
    if (s.phase == ota::Phase::Receiving) sig += std::to_string(percentOf(s) / 5);
    if (s.phase == ota::Phase::Listening) sig += std::to_string((s.secondsLeft + 59) / 60);
    return sig + "|" + s.ip + "|" + s.result;
  }

  static void drawState(ui::Canvas &c, const Rect &area, const ota::Status &s) {
    using namespace ui;
    const Rect p{area.x + 20, area.y + 12, area.w - 40, area.h - 12 - 14};
    constexpr int kHeadH = 19;
    drawText(c, {p.x, p.y, p.w, kHeadH}, "// STATE", style::kSection);
    const std::string pill = phaseName(s.phase);
    drawTag(c, p.right() - tagWidth(pill, style::kPill, 6), p.y + kHeadH / 2, kHeadH, pill,
            style::kPill, TagKind::Inverted, 6);

    // Headline, detail and gauge, centred in what's left of the panel.
    constexpr int kCellH = 28;
    const int blockH = capHeight(kHeadline) + 16 + lineHeight(style::kBody) + 18 + kCellH;
    const int top = p.y + kHeadH + std::max(12, (p.h - kHeadH - blockH) / 2);
    const int headBase = top + capHeight(kHeadline);
    drawText(c, {p.x, headBase - capHeight(kHeadline), p.w, capHeight(kHeadline)}, headline(s),
             kHeadline);
    const int detailTop = headBase + 16;
    drawText(c, {p.x, detailTop, p.w, lineHeight(style::kBody)}, detail(s), style::kBody);

    // Segmented gauge, one cell per 10 %, a part-full cell dithered (System's battery gauge).
    const int percent = s.phase == ota::Phase::Receiving || s.phase == ota::Phase::Done
                            ? percentOf(s)
                            : 0;
    const int cellsTop = detailTop + lineHeight(style::kBody) + 18;
    constexpr int kGap = 4;
    for (int i = 0; i < kCells; ++i) {
      const int x0 = p.x + i * (p.w + kGap) / kCells;
      const int x1 = p.x + (i + 1) * (p.w + kGap) / kCells - kGap;
      const Rect cell{x0, cellsTop, x1 - x0, kCellH};
      const int fill = percent - i * 10;
      if (fill >= 10) {
        ui::fill(c, cell, kInk);
      } else {
        frame(c, cell, 2);
        if (fill > 0) dither50(c, cell.inset(2, 2));
      }
    }
  }

  static void drawRows(ui::Canvas &c, Rect &r, const std::pair<const char *, std::string> *rows,
                       int count) {
    using namespace ui;
    for (int i = 0; i < count; ++i) {
      const Rect line = r.takeTop(kRowH);
      const int x = line.x + kPadX;
      drawText(c, {x, line.y, kRowKeyW, line.h}, rows[i].first, style::kKey);
      drawText(c, {x + kRowKeyW, line.y, line.right() - kPadX - x - kRowKeyW, line.h},
               rows[i].second, kRowValue);
      if (i + 1 < count) dashedH(c, line.x, line.bottom() - 1, line.w);
    }
  }

  static void drawAddress(ui::Canvas &c, const Rect &area, const ota::Status &s) {
    using namespace ui;
    Rect r = area;
    hrule(c, r.x, r.y, r.w);
    r.takeTop(kRule);
    drawSectionHeader(c, r.takeTop(kSectionH), "ADDRESS", "PORT " + std::to_string(ota::kPort));
    r.takeTop(4);
    const std::pair<const char *, std::string> rows[] = {
        {"HOST", std::string(ota::kHostname) + ".local"},
        {"IP", s.ip.empty() ? "--" : s.ip},
        {"NETWORK", wlan::network()},
    };
    drawRows(c, r, rows, 3);
  }

  static void drawUpload(ui::Canvas &c, const Rect &area) {
    using namespace ui;
    Rect r = area;
    hrule(c, r.x, r.y, r.w);
    r.takeTop(kRule);
    drawSectionHeader(c, r.takeTop(kSectionH), "ON THE COMPUTER", "ESPOTA");
    r.takeTop(8);
    const int x = r.x + kPadX;
    const int w = r.w - 2 * kPadX;
    const std::string command =
        std::string("$ pio run -t upload --upload-port ") + ota::kHostname + ".local";
    drawText(c, {x, r.takeTop(kNoteH).y, w, kNoteH}, command, kCommand);
    drawText(c, {x, r.takeTop(kNoteH).y, w, kNoteH}, "password: OTA_PASSWORD from .env", kNote);
    drawText(c, {x, r.takeTop(kNoteH).y, w, kNoteH}, "let Python accept incoming connections",
             kNote);
  }

  static void drawFirmware(ui::Canvas &c, const Rect &area) {
    using namespace ui;
    Rect r = area;
    hrule(c, r.x, r.y, r.w);
    r.takeTop(kRule);
    drawSectionHeader(c, r.takeTop(kSectionH), "FIRMWARE", "IDF " + sysinfo::idfVersion());
    r.takeTop(4);
    const std::pair<const char *, std::string> rows[] = {
        {"RUNNING", std::string(sysinfo::kFirmwareVersion) + " \xC2\xB7 " + sysinfo::buildId()},
        {"SLOTS", sysinfo::otaState()},
    };
    drawRows(c, r, rows, 2);
  }

  std::string drawnSignature_;
  bool doneDrawn_ = false;
};

}  // namespace

std::unique_ptr<ui::Screen> makeUpdater() { return std::make_unique<UpdaterScreen>(); }

}  // namespace apps
