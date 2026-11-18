#include "apps/music.h"

#include <Arduino.h>
#include <stdio.h>

#include <vector>

#include "apps/files.h"
#include "apps/home.h"
#include "audio.h"
#include "pmic.h"
#include "trackinfo.h"

namespace apps {
namespace {

using ui::Key;
using ui::Rect;
namespace fonts = ui::fonts;

constexpr int kVolumeStep = 5;
// While playing, the time and playhead redraw this often. Each redraw is a 0.6 s partial
// refresh, and every 10th partial is a full one, so this trades liveliness for flashing.
constexpr uint32_t kLiveRedrawMs = 10000;
constexpr int kQueueRows = 4;

// Layout from the Music artboard.
constexpr int kPadX = 20;
constexpr int kPadY = 18;
constexpr int kWaveH = 88;
constexpr int kChainRowH = 34;
constexpr int kChainH = ui::kRule + 10 + 2 * kChainRowH + 6 + 12;
constexpr int kQueueH = ui::kRule + ui::kSectionH + kQueueRows * ui::kRowTallH;

constexpr ui::TextStyle kHeader{&fonts::kMono13Bold, 2};       // "// NOW PLAYING"
constexpr ui::TextStyle kBigTitle{&fonts::kMono50ExtraBold, -1};
constexpr ui::TextStyle kSmallTitle{&fonts::kMono38ExtraBold, -1};
constexpr ui::TextStyle kArtist{&fonts::kMono15Medium, 1};
constexpr ui::TextStyle kTime{&fonts::kMono40ExtraBold, -1};
constexpr ui::TextStyle kTimeTotal{&fonts::kMono18Medium, 0};
constexpr ui::TextStyle kTimeLeft{&fonts::kMono18Bold, 0};
constexpr ui::TextStyle kStatePill{&fonts::kMono13ExtraBold, 1};
constexpr ui::TextStyle kChainKey{&fonts::kMono13Medium, 1};
constexpr ui::TextStyle kChainValue{&fonts::kMono13Bold, 0};

std::string clock(uint32_t ms) {
  const uint32_t s = ms / 1000;
  char buf[16];
  if (s >= 3600) {
    snprintf(buf, sizeof buf, "%lu:%02lu:%02lu", (unsigned long)(s / 3600),
             (unsigned long)(s / 60 % 60), (unsigned long)(s % 60));
  } else {
    snprintf(buf, sizeof buf, "%02lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
  }
  return buf;
}

std::string baseName(const std::string &path) {
  std::string name = path.substr(path.rfind('/') + 1);
  const size_t dot = name.rfind('.');
  return dot == std::string::npos ? name : name.substr(0, dot);
}

class MusicScreen : public ui::Screen {
 public:
  std::string tag() const override { return moduleTag("MUSIC"); }
  bool isPlayer() const override { return true; }
  std::string context() const override {
    const std::string path = audio::status().path;
    if (path.empty()) return "SD:/";
    return displayPath(path.substr(0, path.rfind('/') + 1));  // "SD:/x/", "NFS:/Media/x/"
  }
  bool contextIsPath() const override { return true; }

  int hints(ui::Hint *out) const override {
    const audio::Status s = audio::status();
    if (s.path.empty()) {
      out[0] = {"BOOT", "BACK"};
      return 1;
    }
    out[0] = {ui::glyph::kDial, "VOL"};
    out[1] = {ui::glyph::kPress, s.state == audio::State::Playing ? "PAUSE" : "PLAY"};
    out[2] = {ui::glyph::kHoldPress, "SKIP"};
    out[3] = {"BOOT", "BACK"};
    return 4;
  }

  bool onKey(Key key) override {
    switch (key) {
      case Key::Up: audio::changeVolume(+kVolumeStep); return true;
      case Key::Down: audio::changeVolume(-kVolumeStep); return true;
      case Key::Select: audio::togglePause(); break;
      case Key::SelectLong: audio::next(); break;
      default: return false;
    }
    delay(50);  // commands are asynchronous: let the audio task apply it before the redraw
    return true;
  }

  bool onTick() override {
    const audio::Status s = audio::status();
    if (s.generation != drawnGeneration_ || trackinfo::generation() != drawnInfo_) return true;
    return s.state == audio::State::Playing && millis() - drawnAt_ >= kLiveRedrawMs;
  }

  void draw(ui::Canvas &c, const Rect &body) override {
    const audio::Status s = audio::status();
    drawnGeneration_ = s.generation;
    drawnInfo_ = trackinfo::generation();
    drawnAt_ = millis();

    // What the background task should look at: this track (with its waveform), the next few.
    const std::vector<std::string> queue = audio::queue(s.track, kQueueRows);
    for (size_t i = 0; i < queue.size(); ++i) trackinfo::request(queue[i], i == 0);
    trackinfo::Info info;
    const bool haveInfo = !s.path.empty() && trackinfo::get(s.path, info);

    Rect r = body;
    const Rect chain = r.takeBottom(kChainH);
    const Rect queueArea = r.takeBottom(kQueueH);
    drawNowPlaying(c, r, s, haveInfo ? &info : nullptr);
    drawQueue(c, queueArea, s, queue);
    drawChain(c, chain, s, haveInfo ? &info : nullptr);
  }

 private:
  static void drawNowPlaying(ui::Canvas &c, const Rect &area, const audio::Status &s,
                             const trackinfo::Info *info) {
    using namespace ui;
    const Rect p{area.x + kPadX, area.y + kPadY, area.w - 2 * kPadX, area.h - 2 * kPadY};

    // "// NOW PLAYING ............ TRK 01/04"
    char trk[24];
    snprintf(trk, sizeof trk, "TRK %02d/%02d", s.trackCount ? s.track + 1 : 0, s.trackCount);
    drawText(c, {p.x, p.y, p.w, 17}, "// NOW PLAYING", kHeader);
    drawText(c, {p.x, p.y, p.w, 17}, trk, kHeader, Align::Right);
    int y = p.y + 17 + 12;

    // Title: up to two lines at 50 px, else up to two at 38 px.
    const std::string title = upper(s.path.empty() ? "Nothing playing" : s.title);
    const TextStyle *style = &kBigTitle;
    int pitch = 48;
    std::vector<std::string> lines = wrap(kBigTitle, title, p.w, 0);
    bool fits = lines.size() <= 2;
    for (const auto &l : lines) {
      fits = fits && l.find(glyph::kEllipsis) == std::string::npos;  // a word too long for a line
    }
    if (!fits) {
      style = &kSmallTitle;
      pitch = 38;
      lines = wrap(kSmallTitle, title, p.w, 2);
    }
    y += capHeight(*style);
    for (size_t i = 0; i < lines.size(); ++i) {
      drawText(c, p.x, y + static_cast<int>(i) * pitch, lines[i], *style);
    }
    y += static_cast<int>(lines.size() - 1) * pitch + 12;
    if (!s.artist.empty()) {
      y += capHeight(kArtist);
      drawText(c, p.x, y, ellipsize(kArtist, upper(s.artist), p.w), kArtist);
    }
    y += 14;

    // Waveform overview with the playhead.
    const uint32_t duration = info && info->meta.durationMs ? info->meta.durationMs : s.durationMs;
    const float played = duration ? std::min(1.0f, float(s.elapsedMs) / duration) : 0.0f;
    drawWave(c, {p.x, y, p.w, kWaveH}, info ? info->wave : std::vector<uint8_t>(), played);
    y += kWaveH + 12;

    // "01:12 / 03:05 ............ −01:53"
    if (!s.path.empty()) {
      const int base = y + capHeight(kTime);
      int x = drawText(c, p.x, base, clock(s.elapsedMs), kTime);
      if (duration) {
        drawText(c, x + 8, base, "/ " + clock(duration), kTimeTotal);
        const std::string left =
            std::string("\xE2\x88\x92") + clock(duration > s.elapsedMs ? duration - s.elapsedMs : 0);
        drawText(c, p.right() - textWidth(kTimeLeft, left), base, left, kTimeLeft);
      }
    }

    // Bottom: play state, then the volume meter.
    const int volY = p.bottom() - 18;
    const int pillCenter = volY - 8 - 13;
    const char *state = s.state == audio::State::Playing  ? "\xE2\x96\xB6 PLAYING"
                        : s.state == audio::State::Paused ? "II PAUSED"
                                                          : "\xE2\x96\xA0 STOPPED";
    drawTag(c, p.x, pillCenter, 26, state, kStatePill, TagKind::Inverted, 9);

    const std::string pct = std::to_string(s.volume) + "%";
    const int volLabelW = textWidth(style::kLabel, "VOL");
    const int pctW = textWidth(style::kLabel, "100%");
    const int base = baselineCentered(style::kLabel, volY, 18);
    drawText(c, p.x, base, "VOL", style::kLabel);
    drawText(c, p.right() - textWidth(style::kLabel, pct), base, pct, style::kLabel);
    drawSegments(c, {p.x + volLabelW + 10, volY, p.w - volLabelW - pctW - 20, 18}, 10,
                 (s.volume + 5) / 10);
  }

  // 72 bars (solid = played, dithered = to come) round a 3 px playhead, between dashed rules.
  static void drawWave(ui::Canvas &c, const Rect &band, const std::vector<uint8_t> &wave,
                       float played) {
    using namespace ui;
    dashedH(c, band.x, band.y, band.w);
    dashedH(c, band.x, band.bottom() - 1, band.w);
    constexpr int kBars = trackinfo::kWaveBins, kBarW = 4, kGap = 2, kHeadW = 3;
    const int total = kBars * (kBarW + kGap) + kHeadW;
    int x = band.x + (band.w - total) / 2;
    const int head = static_cast<int>(played * kBars + 0.5f);
    for (int i = 0; i <= kBars; ++i) {
      if (i == head) {
        fill(c, {x, band.y, kHeadW, band.h}, kInk);
        x += kHeadW + kGap;
      }
      if (i == kBars) break;
      const int level = wave.empty() ? 0 : wave[i];
      const int h = (4 + level * (band.h - 12) / 255) & ~1;
      const Rect bar{x, band.centerY() - h / 2, kBarW, h};
      if (i < head) {
        fill(c, bar, kInk);
      } else {
        dither50(c, bar);
      }
      x += kBarW + kGap;
    }
  }

  static void drawQueue(ui::Canvas &c, const Rect &area, const audio::Status &s,
                        const std::vector<std::string> &queue) {
    using namespace ui;
    Rect r = area;
    hrule(c, r.x, r.y, r.w);
    r.takeTop(kRule);
    drawSectionHeader(c, r.takeTop(kSectionH), "QUEUE",
                      std::to_string(s.trackCount) + (s.trackCount == 1 ? " TRACK" : " TRACKS"));
    for (size_t i = 0; i < queue.size(); ++i) {
      trackinfo::Info info;
      Row row;
      char index[12];
      snprintf(index, sizeof index, "%02d", s.track + static_cast<int>(i) + 1);
      row.index = index;
      if (trackinfo::get(queue[i], info)) {
        row.title = info.meta.title;
        row.subtitle = info.meta.artist;
        if (info.meta.durationMs) row.trailing = clock(info.meta.durationMs);
      } else {
        row.title = baseName(queue[i]);
      }
      if (row.subtitle.empty()) row.subtitle = glyph::kEmDash;
      drawRow(c, r.takeTop(kRowTallH), row, i == 0);
    }
  }

  // CODEC / FILE / AMP / RAIL, two columns.
  static void drawChain(ui::Canvas &c, const Rect &area, const audio::Status &s,
                        const trackinfo::Info *info) {
    using namespace ui;
    hrule(c, area.x, area.y, area.w);
    char rate[16] = "\xE2\x80\x94";  // —
    const uint32_t sampleRate = s.sampleRate ? s.sampleRate : info ? info->meta.sampleRate : 0;
    if (sampleRate) snprintf(rate, sizeof rate, "%.3gk/16", sampleRate / 1000.0);
    const int kbps = info && info->meta.vbr ? info->meta.bitrateKbps : s.bitrateKbps;
    std::string file = "MP3";
    if (kbps) file += std::string(" ") + glyph::kDot + " " + std::to_string(kbps) + " kbps";
    if (info && info->meta.vbr) file += " VBR";
    const std::string dot = std::string(" ") + glyph::kDot + " ";
    const struct {
      const char *key;
      std::string value;
    } cells[] = {
        {"CODEC", "ES8311" + dot + rate},
        {"FILE", file},
        {"AMP", "NS4150B" + dot + (audio::outputOn() ? "on" : "off")},
        {"RAIL", "ALDO2" + dot + (audio::outputOn() ? "on" : "off")},  // follows the output
    };
    const Rect inner{area.x + kPadX, area.y + kRule + 10, area.w - 2 * kPadX, 2 * kChainRowH + 6};
    const int colW = (inner.w - 16) / 2;
    for (int i = 0; i < 4; ++i) {
      const int x = inner.x + (i % 2) * (colW + 16);
      const int top = inner.y + (i / 2) * (kChainRowH + 6);
      const int keyBase = top + capHeight(kChainKey) + 2;
      drawText(c, x, keyBase, cells[i].key, kChainKey);
      drawText(c, x, keyBase + 6 + capHeight(kChainValue),
               ellipsize(kChainValue, cells[i].value, colW), kChainValue);
    }
  }

  uint32_t drawnGeneration_ = 0;
  uint32_t drawnInfo_ = 0;
  uint32_t drawnAt_ = 0;

  friend class PlayerSheet;
};

// The player sheet (HOLD BOOT while something plays): what's playing and its controls, over the
// lower half of whatever is on screen.
constexpr int kSheetMinH = 300;
constexpr int kSheetPad = 14;
constexpr int kSheetWaveMaxH = 88;
constexpr int kSheetPillH = 26;

// The sheet while on top: HOLD BOOT there closes it.
const ui::Screen *sheetOnTop = nullptr;

// The folder a track is in ("Up First from NPR" for a podcast episode), for a track with no
// artist.
std::string folderName(const std::string &path) {
  const size_t slash = path.rfind('/');
  if (slash == std::string::npos || slash == 0) return {};
  const size_t start = path.rfind('/', slash - 1);
  return path.substr(start == std::string::npos ? 0 : start + 1, slash - start - 1);
}

class PlayerSheet : public ui::Screen {
 public:
  std::string tag() const override { return {}; }  // the screen below supplies the chrome
  bool overlay() const override { return true; }
  bool allowsTalk() const override { return false; }  // HOLD BOOT closes it (openPlayer())

  int hints(ui::Hint *out) const override {
    const audio::Status s = audio::status();
    if (s.path.empty()) {
      out[0] = {"BOOT", "CLOSE"};
      return 1;
    }
    out[0] = {ui::glyph::kPress, s.state == audio::State::Playing ? "PAUSE" : "PLAY"};
    out[1] = {ui::glyph::kHoldPress, "NEXT"};
    out[2] = {ui::glyph::kDial, "VOL"};
    out[3] = {"BOOT", "CLOSE"};
    return 4;
  }

  void onEnter() override { sheetOnTop = this; }
  void onLeave() override { sheetOnTop = nullptr; }

  bool onKey(Key key) override {
    switch (key) {
      case Key::Up: audio::changeVolume(+kVolumeStep); return true;
      case Key::Down: audio::changeVolume(-kVolumeStep); return true;
      case Key::Select: audio::togglePause(); break;
      case Key::SelectLong: audio::next(); break;
      default: return false;  // BOOT closes (the navigator pops)
    }
    delay(50);  // commands are asynchronous: let the audio task apply it before the redraw
    return true;
  }

  bool onTick() override {
    const audio::Status s = audio::status();
    if (s.generation != drawnGeneration_ || trackinfo::generation() != drawnInfo_) return true;
    return s.state == audio::State::Playing && millis() - drawnAt_ >= kLiveRedrawMs;
  }

  void draw(ui::Canvas &c, const Rect &body) override {
    using namespace ui;
    const audio::Status s = audio::status();
    drawnGeneration_ = s.generation;
    drawnInfo_ = trackinfo::generation();
    drawnAt_ = millis();
    if (!s.path.empty()) trackinfo::request(s.path, true);  // the waveform and a true duration
    trackinfo::Info info;
    const bool haveInfo = !s.path.empty() && trackinfo::get(s.path, info);

    // Over a screen without chrome (the desk face), the sheet shows its keys itself.
    Hint keys[kMaxHints];
    const Screen *under = nav::base();
    const int keyCount = under && !under->chrome() ? hints(keys) : 0;
    Rect r = drawSheet(c, body, kSheetMinH, keys, keyCount);

    char trk[24];
    snprintf(trk, sizeof trk, "TRK %02d/%02d", s.track + 1, s.trackCount);
    drawSectionHeader(c, r.takeTop(kSectionH), "NOW PLAYING", s.trackCount ? trk : "");
    const Rect p{r.x + kPadX, r.y + kSheetPad, r.w - 2 * kPadX, r.h - 2 * kSheetPad};

    // Title: one line at 38 px if it fits, else up to two at 22.
    const std::string title = upper(s.path.empty() ? "Nothing playing" : s.title);
    const TextStyle *style = &kSmallTitle;
    std::vector<std::string> lines = wrap(kSmallTitle, title, p.w, 0);
    if (lines.size() != 1 || lines[0].find(glyph::kEllipsis) != std::string::npos) {
      style = &style::kReading;
      lines = wrap(style::kReading, title, p.w, 2);
    }
    int y = p.y + capHeight(*style);
    for (size_t i = 0; i < lines.size(); ++i) {
      drawText(c, p.x, y + static_cast<int>(i) * lineHeight(*style), lines[i], *style);
    }
    y += static_cast<int>(lines.size() - 1) * lineHeight(*style) + 12;

    // The artist, or for a track without one (a podcast episode) its folder: the show.
    const std::string by = !s.artist.empty() ? s.artist : folderName(s.path);
    if (!by.empty()) {
      y += capHeight(kArtist);
      drawText(c, p.x, y, ellipsize(kArtist, upper(by), p.w), kArtist);
    }
    y += 14;

    // The waveform, as tall as there's room for, the times right under it, and the play state
    // and volume along the bottom.
    const int pillTop = p.bottom() - kSheetPillH;
    const int timeH = capHeight(kTimeLeft);
    const uint32_t duration = haveInfo && info.meta.durationMs ? info.meta.durationMs : s.durationMs;
    const int waveH = std::min(kSheetWaveMaxH, pillTop - 16 - timeH - 12 - y);
    if (waveH >= 24) {
      const float played = duration ? std::min(1.0f, float(s.elapsedMs) / duration) : 0.0f;
      MusicScreen::drawWave(c, {p.x, y, p.w, waveH}, haveInfo ? info.wave : std::vector<uint8_t>(),
                            played);
      y += waveH + 12;
    }
    const int timeBase = y + timeH;

    if (!s.path.empty()) {
      int x = drawText(c, p.x, timeBase, clock(s.elapsedMs), kTimeLeft);
      if (duration) {
        drawText(c, x + 8, timeBase, "/ " + clock(duration), kTimeTotal);
        const std::string left = std::string("\xE2\x88\x92") +  // −
                                 clock(duration > s.elapsedMs ? duration - s.elapsedMs : 0);
        drawText(c, p.right() - textWidth(kTimeLeft, left), timeBase, left, kTimeLeft);
      }
    }

    const char *state = s.state == audio::State::Playing  ? "\xE2\x96\xB6 PLAYING"
                        : s.state == audio::State::Paused ? "II PAUSED"
                                                          : "\xE2\x96\xA0 STOPPED";
    const int pillW = drawTag(c, p.x, pillTop + kSheetPillH / 2, kSheetPillH, state, kStatePill,
                              TagKind::Inverted, 9);

    // "VOL ▮▮▮▮▮▯▯▯▯▯ 60%" to the right of the state.
    const std::string pct = std::to_string(s.volume) + "%";
    const int volX = p.x + pillW + 24;
    const int volLabelW = textWidth(style::kLabel, "VOL");
    const int pctW = textWidth(style::kLabel, "100%");
    const int volY = pillTop + (kSheetPillH - 18) / 2;
    const int base = baselineCentered(style::kLabel, volY, 18);
    drawText(c, volX, base, "VOL", style::kLabel);
    drawText(c, p.right() - textWidth(style::kLabel, pct), base, pct, style::kLabel);
    drawSegments(c, {volX + volLabelW + 10, volY, p.right() - volX - volLabelW - pctW - 20, 18}, 10,
                 (s.volume + 5) / 10);
  }

 private:
  uint32_t drawnGeneration_ = 0;
  uint32_t drawnInfo_ = 0;
  uint32_t drawnAt_ = 0;
};

}  // namespace

std::unique_ptr<ui::Screen> makeMusic() { return std::make_unique<MusicScreen>(); }

bool openPlayer() {
  ui::Screen *top = ui::nav::top();
  if (top && top == sheetOnTop) {  // HOLD BOOT on the sheet: closed, as BOOT does
    ui::nav::pop();
    return true;
  }
  if (!audio::playing() || ui::nav::locked()) return false;
  if (top && top->offFace()) return false;
  if (top && top->isPlayer()) return true;  // the full player shows all of it already
  // Close menus and sheets first, so the sheet isn't drawn over one hidden under it.
  while (ui::nav::top() && ui::nav::top()->overlay()) ui::nav::pop();
  ui::nav::push(std::make_unique<PlayerSheet>());
  return true;
}

}  // namespace apps
