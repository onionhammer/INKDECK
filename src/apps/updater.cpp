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

constexpr int kRowH = 26;
constexpr int kRowKeyW = 104;
constexpr int kCells = 10;

std::string megabytes(uint32_t bytes) {
  char buf[16];
  snprintf(buf, sizeof buf, "%.2f", bytes / 1048576.0);
  return buf;
}

int percentOf(const ota::Status &s) {
  if (s.phase == ota::Phase::Done) return 100;
  return s.release.size ? static_cast<int>(static_cast<uint64_t>(s.bytes) * 100 / s.release.size)
                        : 0;
}

bool known(const ota::Status &s) { return !s.release.version.empty(); }

const char *phaseName(const ota::Status &s) {
  switch (s.phase) {
    case ota::Phase::Joining: return "JOINING";
    case ota::Phase::Checking: return "CHECKING";
    case ota::Phase::Checked: return s.order > 0 ? "NEW" : s.order == 0 ? "LATEST" : "AHEAD";
    case ota::Phase::Downloading: return "DOWNLOADING";
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
    case ota::Phase::Checking: return "CHECKING\xE2\x80\xA6";
    case ota::Phase::Checked:
      if (s.order > 0) return ui::upper(s.release.version);
      return s.order == 0 ? "UP TO DATE" : "AHEAD";
    case ota::Phase::Downloading: return std::to_string(percentOf(s)) + "%";
    case ota::Phase::Done: return "UPDATED";
    case ota::Phase::Failed: return "NO UPDATE";
    case ota::Phase::Idle: break;
  }
  return "STOPPED";
}

// Under it: what's happening, or what went wrong.
std::string detail(const ota::Status &s) {
  const std::string running = sysinfo::firmwareVersion();
  switch (s.phase) {
    case ota::Phase::Joining: return std::string("joining ") + wlan::network();
    case ota::Phase::Checking: return "asking GitHub for the latest release";
    case ota::Phase::Checked:
      if (s.order > 0) return "new release \xC2\xB7 running " + running;
      if (s.order == 0) return s.release.version + " is the latest release";
      return "running " + running + ", newer than " + s.release.version;
    case ota::Phase::Downloading:
      return megabytes(s.bytes) + " / " + megabytes(s.release.size) + " MB into the idle slot";
    case ota::Phase::Done: return "restarting into " + s.release.version + "\xE2\x80\xA6";
    case ota::Phase::Failed: return s.result;
    case ota::Phase::Idle: break;
  }
  return "\xE2\x97\x8F checks again";
}

class UpdaterScreen : public ui::Screen {
 public:
  UpdaterScreen() { ota::check(); }
  ~UpdaterScreen() override { ota::stop(); }

  std::string tag() const override { return moduleTag("SYSTEM"); }
  std::string context() const override { return "FIRMWARE UPDATE"; }

  int hints(ui::Hint *out) const override {
    const ota::Status s = ota::status();
    switch (s.phase) {
      case ota::Phase::Done: return 0;
      case ota::Phase::Checked:
        if (s.order > 0) {
          out[0] = {ui::glyph::kPress, "INSTALL"};
          out[1] = {"BOOT", "BACK"};
          return 2;
        }
        out[0] = {ui::glyph::kPress, "CHECK"};
        if (s.order == 0) {
          out[1] = {"BOOT", "BACK"};
          return 2;
        }
        out[1] = {ui::glyph::kHoldPress, "INSTALL"};  // going back to the release
        out[2] = {"BOOT", "BACK"};
        return 3;
      case ota::Phase::Idle:
      case ota::Phase::Failed:
        out[0] = {ui::glyph::kPress, "CHECK"};
        out[1] = {"BOOT", "BACK"};
        return 2;
      default:  // joining, checking, downloading: leaving cancels
        out[0] = {"BOOT", "CANCEL"};
        return 1;
    }
  }

  bool onKey(Key key) override {
    const ota::Status s = ota::status();
    switch (key) {
      case Key::Select:
        if (s.phase == ota::Phase::Checked && s.order > 0) {
          ota::install();
        } else if (s.phase == ota::Phase::Checked || s.phase == ota::Phase::Idle ||
                   s.phase == ota::Phase::Failed) {
          ota::check();
        } else {
          return false;
        }
        return true;
      case Key::SelectLong:
        // An older release (the board runs a build from the computer): only on a hold.
        if (s.phase != ota::Phase::Checked || s.order >= 0) return false;
        ota::install();
        return true;
      case Key::Back:
        // Not while the board is about to restart into the new image.
        return s.phase == ota::Phase::Done;
      default: return false;
    }
  }

  // Redraws when the phase, the 5 % step or the message change. Once the DONE frame is on the
  // panel (this tick comes after its refresh), the board may restart.
  bool onTick() override {
    if (doneDrawn_) ota::doneShown();
    return signature(ota::status()) != drawnSignature_;
  }

  void draw(ui::Canvas &c, const Rect &body) override {
    const ota::Status s = ota::status();
    drawnSignature_ = signature(s);
    doneDrawn_ = s.phase == ota::Phase::Done;

    Rect r = body;
    const Rect firmware = r.takeBottom(ui::kRule + ui::kSectionH + 4 + 3 * kRowH + 6);
    const Rect release = r.takeBottom(ui::kRule + ui::kSectionH + 4 + 4 * kRowH + 6);
    drawState(c, r, s);
    drawRelease(c, release, s);
    drawFirmware(c, firmware);
  }

 private:
  static std::string signature(const ota::Status &s) {
    std::string sig = phaseName(s);
    if (s.phase == ota::Phase::Downloading) sig += std::to_string(percentOf(s) / 5);
    return sig + "|" + s.release.version + "|" + s.result;
  }

  static void drawState(ui::Canvas &c, const Rect &area, const ota::Status &s) {
    using namespace ui;
    const Rect p{area.x + 20, area.y + 12, area.w - 40, area.h - 12 - 14};
    constexpr int kHeadH = 19;
    drawText(c, {p.x, p.y, p.w, kHeadH}, "// STATE", style::kSection);
    const std::string pill = phaseName(s);
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
    const int percent = s.phase == ota::Phase::Downloading || s.phase == ota::Phase::Done
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

  // The latest release, once a check has found it, and where it comes from.
  static void drawRelease(ui::Canvas &c, const Rect &area, const ota::Status &s) {
    using namespace ui;
    Rect r = area;
    hrule(c, r.x, r.y, r.w);
    r.takeTop(kRule);
    const std::string defaultUrl =
        std::string("https://github.com/") + ota::kRepo + "/releases/latest/download/manifest.json";
    const std::string url = ota::manifestUrl();
    drawSectionHeader(c, r.takeTop(kSectionH), "LATEST RELEASE",
                      url == defaultUrl ? "GITHUB" : "TEST SERVER");  // !update URL
    r.takeTop(4);
    const ota::Release &rel = s.release;
    std::string version = known(s) ? rel.version : "--";
    if (known(s) && !rel.build.empty()) version += " \xC2\xB7 " + rel.build;
    const std::pair<const char *, std::string> rows[] = {
        {"VERSION", version},
        {"DATE", known(s) && !rel.date.empty() ? rel.date : "--"},
        {"SIZE", known(s) ? megabytes(rel.size) + " MB" : "--"},
        {"FROM", url == defaultUrl ? std::string(ota::kRepo) : url},
    };
    drawRows(c, r, rows, 4);
  }

  static void drawFirmware(ui::Canvas &c, const Rect &area) {
    using namespace ui;
    Rect r = area;
    hrule(c, r.x, r.y, r.w);
    r.takeTop(kRule);
    drawSectionHeader(c, r.takeTop(kSectionH), "FIRMWARE", "IDF " + sysinfo::idfVersion());
    r.takeTop(4);
    const std::pair<const char *, std::string> rows[] = {
        {"RUNNING", std::string(sysinfo::firmwareVersion()) + " \xC2\xB7 " + sysinfo::buildId()},
        {"SLOTS", sysinfo::otaState()},
        {"NETWORK", wlan::network()},
    };
    drawRows(c, r, rows, 3);
  }

  std::string drawnSignature_;
  bool doneDrawn_ = false;
};

}  // namespace

std::unique_ptr<ui::Screen> makeUpdater() { return std::make_unique<UpdaterScreen>(); }

}  // namespace apps
