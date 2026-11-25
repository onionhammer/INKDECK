#include "apps/wifi_drive.h"

#include <Arduino.h>
#include <stdio.h>

#include <algorithm>

#include "apps/home.h"
#include "podcast_library.h"
#include "settings.h"
#include "tailnet.h"
#include "ui/menu.h"
#include "webdav.h"
#include "wlan.h"

namespace apps {
namespace {

using ui::Key;
using ui::Rect;
using ui::TextStyle;

constexpr TextStyle kHeadline = ui::style::kTitle;
constexpr TextStyle kRowValue{&ui::fonts::kMono14Bold, 0};
constexpr TextStyle kNote{&ui::fonts::kMono14Medium, 0};

constexpr int kRowH = 26;
constexpr int kRowKeyW = 104;
constexpr int kNoteH = 22;
// Data this recent counts as READING or WRITING (and BOOT asks first). Finder moves a file in
// bursts with pauses of a second or so between them.
constexpr uint32_t kActiveMs = 4000;
constexpr uint32_t kCounterRedrawMs = 5000;  // the byte counts and rate redraw at most this often
constexpr uint32_t kStopWaitMs = 6000;       // leaving waits this long for the server to stop

std::string bytes(uint64_t n) {
  char buf[24];
  if (n >= 1000ull * 1000 * 1000) {
    snprintf(buf, sizeof buf, "%.2f GB", n / 1e9);
  } else {
    snprintf(buf, sizeof buf, "%.2f MB", n / 1e6);
  }
  return buf;
}

// The headline's font has capitals only.
std::string hostName(bool upper = false) {
  std::string h = std::string(webdav::kHostname) + ".local";
  if (upper) {
    for (char &c : h) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
  }
  return h;
}

class WifiDriveScreen : public ui::Screen {
 public:
  WifiDriveScreen() {
    webdav::start();
    lastSampleMs_ = millis();
  }

  ~WifiDriveScreen() override {
    webdav::stop();
    if (!webdav::changedSettings() && !webdav::changedPodcasts()) return;
    // What the computer changed is read again once nothing can still be writing it.
    const uint32_t t0 = millis();
    while (webdav::active() && millis() - t0 < kStopWaitMs) delay(50);
    if (webdav::changedSettings()) settings::loadCard();
    if (webdav::changedPodcasts() && !podcasts::busy()) podcasts::load(false);
  }

  std::string tag() const override { return moduleTag("SYSTEM"); }
  std::string context() const override { return "WI-FI DRIVE"; }

  int hints(ui::Hint *out) const override {
    switch (webdav::status().phase) {
      case webdav::Phase::Idle:
      case webdav::Phase::Failed:
        out[0] = {ui::glyph::kPress, "START"};
        out[1] = {"BOOT", "BACK"};
        return 2;
      default:
        out[0] = {"BOOT", "STOP"};
        return 1;
    }
  }

  bool onKey(Key key) override {
    const webdav::Status s = webdav::status();
    switch (key) {
      case Key::Select:
        if (webdav::active()) return false;
        webdav::start();
        return true;
      case Key::Back:
        if (!moving(s)) return false;  // the navigator pops the screen, which stops the server
        // The computer is copying: stopping now fails the copy there.
        ui::nav::push(std::make_unique<ui::MenuScreen>(
            "STOP SHARING?", std::vector<ui::MenuItem>{
                                 {"CANCEL", nullptr, [] { ui::nav::pop(); }},
                                 {"STOP", [] { return std::string("COPY UNDER WAY"); },
                                  [] {
                                    ui::nav::pop();  // the sheet
                                    ui::nav::pop();  // and this screen
                                  }},
                             }));
        return true;
      default: return false;
    }
  }

  bool onTick() override {
    const webdav::Status s = webdav::status();
    sampleRate(s);
    if (coarseSignature(s) != drawnCoarse_) return true;
    return fineSignature(s) != drawnFine_ && millis() - drawnMs_ >= kCounterRedrawMs;
  }

  void draw(ui::Canvas &c, const Rect &body) override {
    const webdav::Status s = webdav::status();
    drawnCoarse_ = coarseSignature(s);
    drawnFine_ = fineSignature(s);
    drawnMs_ = millis();

    Rect r = body;
    const int noteLines = tailnet::configured() ? 4 : 3;
    const Rect notes = r.takeBottom(ui::kRule + ui::kSectionH + 8 + noteLines * kNoteH + 8);
    const Rect transfer = r.takeBottom(ui::kRule + ui::kSectionH + 4 + 3 * kRowH + 6);
    const Rect signIn = r.takeBottom(ui::kRule + ui::kSectionH + 4 + 2 * kRowH + 6);
    drawState(c, r, s);
    drawSignIn(c, signIn, s);
    drawTransfer(c, transfer, s);
    drawNotes(c, notes, s);
  }

 private:
  static bool moving(const webdav::Status &s) {
    return s.msSinceWrite < kActiveMs || s.msSinceRead < kActiveMs;
  }

  static const char *pill(const webdav::Status &s) {
    switch (s.phase) {
      case webdav::Phase::Joining: return "JOINING";
      case webdav::Phase::Serving:
        // Writing wins: a copy to the card reads a little as it goes.
        if (s.msSinceWrite < kActiveMs) return "WRITING";
        if (s.msSinceRead < kActiveMs) return "READING";
        return s.clients ? "CONNECTED" : "READY";
      case webdav::Phase::Failed: return "FAILED";
      case webdav::Phase::Idle: break;
    }
    return "STOPPED";
  }

  static std::string headline(const webdav::Status &s) {
    switch (s.phase) {
      case webdav::Phase::Joining: return "JOINING\xE2\x80\xA6";
      case webdav::Phase::Serving: return hostName(true);
      case webdav::Phase::Failed: return "NO DRIVE";
      case webdav::Phase::Idle: break;
    }
    return "STOPPED";
  }

  static std::string detail(const webdav::Status &s) {
    switch (s.phase) {
      case webdav::Phase::Joining: return "joining " + wlan::network();
      case webdav::Phase::Serving:
        return "or " + s.ip + " \xC2\xB7 " + wlan::network();
      case webdav::Phase::Failed: return s.result;
      case webdav::Phase::Idle: break;
    }
    return "\xE2\x97\x8F starts it again";
  }

  // Bytes per second over the last few ticks, while data moves.
  void sampleRate(const webdav::Status &s) {
    const uint64_t total = s.readBytes + s.writtenBytes;
    const uint32_t now = millis();
    const uint32_t dt = now - lastSampleMs_;
    if (dt < 2000) return;
    rate_ = total > lastTotal_ ? (total - lastTotal_) * 1000 / dt : 0;
    lastTotal_ = total;
    lastSampleMs_ = now;
  }

  // The phase, address and last change redraw at once; the counts and the rate wait for
  // kCounterRedrawMs. The idle stop's minutes show (and redraw) only in its last five.
  static std::string coarseSignature(const webdav::Status &s) {
    std::string sig = std::string(pill(s)) + "|" + s.ip + "|" + s.result + "|" + s.lastChange +
                      "|" + std::to_string(s.badSignIns) + "|" + tailnet::describe();
    if (s.phase == webdav::Phase::Serving && s.secondsLeft <= 5 * 60) {
      sig += "|" + std::to_string((s.secondsLeft + 59) / 60);
    }
    return sig;
  }
  std::string fineSignature(const webdav::Status &s) const {
    return std::to_string(s.readBytes / 100000) + "|" + std::to_string(s.writtenBytes / 100000) +
           "|" + std::to_string(rate_ / 10000);
  }

  void drawState(ui::Canvas &c, const Rect &area, const webdav::Status &s) const {
    using namespace ui;
    const Rect p{area.x + 20, area.y + 12, area.w - 40, area.h - 12 - 14};
    constexpr int kHeadH = 19;
    drawText(c, {p.x, p.y, p.w, kHeadH}, "// STATE", style::kSection);
    const std::string tag = pill(s);
    drawTag(c, p.right() - tagWidth(tag, style::kPill, 6), p.y + kHeadH / 2, kHeadH, tag,
            style::kPill, TagKind::Inverted, 6);

    // Headline and detail, centred in what's left of the panel.
    const int blockH = capHeight(kHeadline) + 16 + lineHeight(style::kBody);
    const int top = p.y + kHeadH + std::max(12, (p.h - kHeadH - blockH) / 2);
    const int headBase = top + capHeight(kHeadline);
    drawText(c, {p.x, headBase - capHeight(kHeadline), p.w, capHeight(kHeadline)}, headline(s),
             kHeadline);
    drawText(c, {p.x, headBase + 16, p.w, lineHeight(style::kBody)}, detail(s), style::kBody);
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

  static void drawSignIn(ui::Canvas &c, const Rect &area, const webdav::Status &s) {
    using namespace ui;
    Rect r = area;
    hrule(c, r.x, r.y, r.w);
    r.takeTop(kRule);
    drawSectionHeader(c, r.takeTop(kSectionH), "SIGN IN", "DIGEST");
    r.takeTop(4);
    std::string user = webdav::kUser;
    if (s.badSignIns) {
      user += " \xC2\xB7 " + std::to_string(s.badSignIns) + " wrong";
    }
    const std::pair<const char *, std::string> rows[] = {
        {"USER", user},
        {"PASSWORD", webdav::password()},
    };
    drawRows(c, r, rows, 2);
  }

  void drawTransfer(ui::Canvas &c, const Rect &area, const webdav::Status &s) const {
    using namespace ui;
    Rect r = area;
    hrule(c, r.x, r.y, r.w);
    r.takeTop(kRule);
    char right[32];
    if (rate_ > 0 && s.phase == webdav::Phase::Serving) {
      snprintf(right, sizeof right, "%lu KB/S", static_cast<unsigned long>(rate_ / 1000));
    } else if (s.phase == webdav::Phase::Serving && s.secondsLeft <= 5 * 60) {
      snprintf(right, sizeof right, "IDLE \xC2\xB7 STOPS IN %lu MIN",
               static_cast<unsigned long>((s.secondsLeft + 59) / 60));
    } else {
      snprintf(right, sizeof right, "WEBDAV \xC2\xB7 PORT %u", webdav::kPort);
    }
    drawSectionHeader(c, r.takeTop(kSectionH), "TRANSFER", right);
    r.takeTop(4);
    const std::pair<const char *, std::string> rows[] = {
        {"WRITTEN", bytes(s.writtenBytes)},
        {"READ", bytes(s.readBytes)},
        {"LAST", s.lastChange.empty() ? "--" : s.lastChange},
    };
    drawRows(c, r, rows, 3);
  }

  // While serving with a tailnet set up: where the board answers on it, or why it doesn't.
  static std::string tailnetNote(const webdav::Status &s) {
    if (s.phase != webdav::Phase::Serving) return "Tailnet: while it's serving";
    const std::string at = tailnet::address();
    if (!at.empty()) return "Tailnet: http://" + tailnet::name() + " or http://" + at;
    const std::string state = tailnet::describe();
    if (state.compare(0, 7, "joining") == 0) return "Tailnet: joining\xE2\x80\xA6";
    return "Tailnet: " + state.substr(0, state.find(" \xC2\xB7 "));
  }

  static void drawNotes(ui::Canvas &c, const Rect &area, const webdav::Status &s) {
    using namespace ui;
    Rect r = area;
    hrule(c, r.x, r.y, r.w);
    r.takeTop(kRule);
    drawSectionHeader(c, r.takeTop(kSectionH), "ON THE COMPUTER",
                      tailnet::configured() ? "SAME NETWORK OR TAILNET" : "SAME NETWORK");
    r.takeTop(8);
    const int x = r.x + kPadX;
    const int w = r.w - 2 * kPadX;
    const std::string url = "http://" + hostName();
    drawText(c, {x, r.takeTop(kNoteH).y, w, kNoteH}, "Finder: Go > Connect to Server, " + url,
             kNote);
    drawText(c, {x, r.takeTop(kNoteH).y, w, kNoteH}, "Windows: Map network drive, " + url, kNote);
    drawText(c, {x, r.takeTop(kNoteH).y, w, kNoteH},
             "stops when you leave, or " + std::to_string(webdav::kIdleMinutes) + " min unused",
             kNote);
    if (tailnet::configured()) {
      drawText(c, {x, r.takeTop(kNoteH).y, w, kNoteH}, tailnetNote(s), kNote);
    }
  }

  std::string drawnCoarse_;
  std::string drawnFine_;
  uint32_t drawnMs_ = 0;
  uint64_t lastTotal_ = 0;
  uint32_t lastSampleMs_ = 0;
  uint64_t rate_ = 0;
};

}  // namespace

std::unique_ptr<ui::Screen> makeWifiDrive() { return std::make_unique<WifiDriveScreen>(); }

}  // namespace apps
