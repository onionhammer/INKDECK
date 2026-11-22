#include "apps/recorder.h"

#include <Arduino.h>
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>

#include <algorithm>
#include <atomic>
#include <vector>

#include "apps/home.h"
#include "audio.h"
#include "paths.h"
#include "pmic.h"
#include "sdcard.h"
#include "ui/menu.h"

namespace apps {
namespace {

using ui::Key;
using ui::Rect;
using ui::TextStyle;

constexpr TextStyle kTimeOf{&ui::fonts::kMono18Medium, 0};      // "/ 03:51"
constexpr TextStyle kCellValue{&ui::fonts::kMono17ExtraBold, 0};

constexpr int kPanelH = 228;    // state, timer, waveform, meter (under the section header)
constexpr int kCellsH = 50;
constexpr int kNewRowH = 40;
constexpr int kBars = audio::kRecordBins;  // 72 bars, 4 px wide, 2 px apart
constexpr int kWaveH = 76;
constexpr int kMeterSegments = 20;  // -60..0 dBFS, 3 dB each
constexpr uint32_t kBytesPerSecond = audio::kRecordRate * 2;

struct Memo {
  std::string name;  // file name
  int64_t size = 0;
};

std::string path(const Memo &m) { return std::string(kRecordingsDir) + "/" + m.name; }

uint32_t memoMs(const Memo &m) {
  return m.size > 44 ? static_cast<uint32_t>((m.size - 44) * 1000 / kBytesPerSecond) : 0;
}

std::string clockText(uint32_t ms) {
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

std::string megabytes(int64_t bytes) {
  char buf[16];
  snprintf(buf, sizeof buf, "%.1f MB", bytes / 1048576.0);
  return buf;
}

// "2026-11-13 091502.wav" -> its local time; false for other names.
bool memoTime(const std::string &name, tm &t) {
  int y, mo, d, h, mi, s;
  // siscanf: newlib's integer-only sscanf (the full one pulls in float parsing).
  if (siscanf(name.c_str(), "%4d-%2d-%2d %2d%2d%2d", &y, &mo, &d, &h, &mi, &s) != 6) return false;
  t = tm{};
  t.tm_year = y - 1900;
  t.tm_mon = mo - 1;
  t.tm_mday = d;
  t.tm_hour = h;
  t.tm_min = mi;
  t.tm_sec = s;
  t.tm_isdst = -1;
  mktime(&t);  // fills in the weekday
  return true;
}

// "13 NOV · 09:15", or the file name without ".wav".
std::string memoTitle(const Memo &m) {
  tm t;
  if (!memoTime(m.name, t)) return m.name.substr(0, m.name.size() - 4);
  char buf[32];
  strftime(buf, sizeof buf, "%d %b \xC2\xB7 %H:%M", &t);
  return ui::upper(buf);
}

std::string memoSubtitle(const Memo &m) {
  tm t;
  std::string day;
  if (memoTime(m.name, t)) {
    char buf[8];
    strftime(buf, sizeof buf, "%a", &t);
    day = ui::upper(buf) + " \xC2\xB7 ";
  }
  return day + megabytes(m.size);
}


// The memos, newest first (the names sort by time).
std::vector<Memo> scanMemos() {
  std::vector<Memo> out;
  DIR *d = opendir(kRecordingsDir);
  if (!d) return out;
  while (dirent *e = readdir(d)) {
    if (e->d_type == DT_DIR || !paths::hasSuffix(e->d_name, ".wav")) continue;
    Memo m;
    m.name = e->d_name;
    struct stat st;
    if (stat(path(m).c_str(), &st) == 0) m.size = st.st_size;
    out.push_back(std::move(m));
  }
  closedir(d);
  std::sort(out.begin(), out.end(), [](const Memo &a, const Memo &b) { return a.name > b.name; });
  return out;
}

// A memo's waveform: the loudest sample in 32 ms read at each of kBars points, -60..0 dBFS as
// 0..255. Cheap: 72 short reads, not the whole file.
std::vector<uint8_t> memoWaveform(const Memo &m) {
  std::vector<uint8_t> bins;
  FILE *f = fopen(path(m).c_str(), "rb");
  if (!f) return bins;
  const int64_t data = std::max<int64_t>(0, m.size - 44);
  int16_t buf[512];
  for (int i = 0; i < kBars; ++i) {
    const long at = 44 + static_cast<long>(data * i / kBars) / 2 * 2;
    size_t n = 0;
    if (fseek(f, at, SEEK_SET) == 0) n = fread(buf, sizeof buf[0], 512, f);
    int peak = 0;
    for (size_t k = 0; k < n; ++k) peak = std::max(peak, std::abs(static_cast<int>(buf[k])));
    const float db = peak > 0 ? 20 * log10f(peak / 32768.0f) : -96;
    bins.push_back(static_cast<uint8_t>(std::clamp((db + 60) / 60, 0.0f, 1.0f) * 255));
  }
  fclose(f);
  return bins;
}

// Free space on the card, counted once in the background (FatFs can take seconds on a big card).
std::atomic<int64_t> freeBytes{-1};
std::atomic<bool> counting{false};

void countFree() {
  if (counting.exchange(true)) return;
  xTaskCreate(
      [](void *) {
        freeBytes = sdcard::mounted() ? static_cast<int64_t>(sdcard::freeBytes()) : -1;
        counting = false;
        vTaskDelete(nullptr);
      },
      "sdfree", 4096, nullptr, 1, nullptr);
}

std::string freeTime() {
  const int64_t f = freeBytes.load();
  if (f < 0) return "--";
  const int64_t hours = f / kBytesPerSecond / 3600;
  return hours >= 1 ? std::to_string(hours) + " H" : std::to_string(f / kBytesPerSecond / 60) + " MIN";
}

class RecorderScreen : public ui::Screen {
 public:
  std::string tag() const override { return moduleTag("RECORDER"); }
  std::string context() const override { return "SD:/Recordings/"; }
  bool contextIsPath() const override { return true; }
  bool allowsTalk() const override { return false; }  // the mic is ours (HOLD BOOT would end a memo)

  int hints(ui::Hint *out) const override {
    if (audio::recording()) {
      out[0] = {ui::glyph::kDial, "GAIN"};
      out[1] = {ui::glyph::kPress, "STOP"};
      out[2] = {ui::glyph::kHoldPress, audio::recordStatus().paused ? "RESUME" : "PAUSE"};
      out[3] = {"BOOT", "BACK"};
      return 4;
    }
    if (playing()) {
      out[0] = {ui::glyph::kDial, "VOL"};
      out[1] = {ui::glyph::kPress, "STOP"};
      out[2] = {"BOOT", "BACK"};
      return 3;
    }
    int n = 0;
    out[n++] = {ui::glyph::kDial, "MEMO"};
    if (cursor_ == 0) {
      out[n++] = {ui::glyph::kPress, "REC"};
    } else {
      out[n++] = {ui::glyph::kPress, "PLAY"};
      out[n++] = {ui::glyph::kHoldPress, "DELETE"};
    }
    out[n++] = {"BOOT", "BACK"};
    return n;
  }

  void onEnter() override {
    if (sdcard::mounted()) mkdir(kRecordingsDir, 0775);
    rescan();
    countFree();
    seen_ = audio::recordStatus().generation;
  }

  void onLeave() override {
    if (audio::recording()) audio::stopRecording();
  }

  bool onKey(Key key) override {
    notice_.clear();
    if (audio::recording()) {
      switch (key) {
        case Key::Up: audio::changeMicGain(+6); return true;
        case Key::Down: audio::changeMicGain(-6); return true;
        case Key::Select: audio::stopRecording(); return true;
        case Key::SelectLong: audio::togglePauseRecording(); return true;
        default: return false;
      }
    }
    if (playing()) {
      switch (key) {
        case Key::Up: audio::changeVolume(+5); return true;
        case Key::Down: audio::changeVolume(-5); return true;
        case Key::Select: audio::stop(); return true;
        default: return false;
      }
    }
    const int rows = static_cast<int>(memos_.size()) + 1;
    switch (key) {
      case Key::Up:
        cursor_ = (cursor_ + rows - 1) % rows;
        return true;
      case Key::Down:
        cursor_ = (cursor_ + 1) % rows;
        return true;
      case Key::Select:
        if (cursor_ == 0) {
          startRecording();
        } else {
          audio::play({path(memos_[cursor_ - 1])}, 0);
          delay(50);  // let the audio task open it before the redraw asks
        }
        return true;
      case Key::SelectLong:
        if (cursor_ > 0) confirmDelete(cursor_ - 1);
        return cursor_ > 0;
      default: return false;
    }
  }

  bool onTick() override {
    const audio::RecordStatus r = audio::recordStatus();
    if (r.generation != seen_) {  // started, paused, resumed or finished
      seen_ = r.generation;
      if (!r.on) {
        if (!r.error.empty()) notice_ = r.error;
        rescan();
        cursor_ = memos_.empty() ? 0 : 1;  // the memo just made
        countFree();
      }
      return true;
    }
    if (r.on) return r.elapsedMs / 1000 != drawnSecond_;  // the timer, once a second
    const audio::Status s = audio::status();
    if (s.generation != drawnAudio_) return true;
    if (playing()) return s.elapsedMs / 2000 != drawnSecond_;  // the playhead, every 2 s
    return freeBytes.load() != drawnFree_;
  }

  void draw(ui::Canvas &c, const Rect &body) override {
    using namespace ui;
    const audio::RecordStatus rec = audio::recordStatus();
    const audio::Status play = audio::status();
    const bool isPlaying = playing();
    drawnAudio_ = play.generation;
    drawnFree_ = freeBytes.load();
    drawnSecond_ = rec.on ? rec.elapsedMs / 1000 : play.elapsedMs / 2000;

    Rect r = body;
    // Section header: inverted while recording, a notice after an error.
    const Rect head = r.takeTop(kSectionH);
    if (!notice_.empty()) {
      drawNotice(c, head, notice_);
    } else if (!sdcard::mounted()) {
      drawNotice(c, head, "NO SD CARD");
    } else {
      std::string label = "READY";
      if (rec.on) {
        label = rec.paused ? "RECORDING \xC2\xB7 PAUSED" : "RECORDING";
      } else if (isPlaying) {
        label = "PLAYING \xC2\xB7 " + playingTitle(play.path);
      }
      drawSectionHeader(c, head, label, "16 kHz \xC2\xB7 MONO \xC2\xB7 WAV", {rec.on});
    }

    drawPanel(c, r.takeTop(kPanelH), rec, play, isPlaying);

    // Stat cells.
    hrule(c, r.x, r.y, r.w);
    r.takeTop(kRule);
    const Rect cells = r.takeTop(kCellsH);
    const Memo *sel = selected();
    std::string size = "--";
    if (rec.on) {
      size = megabytes(rec.bytes);
    } else if (isPlaying || sel) {
      size = isPlaying ? megabytes(playingSize(play.path)) : megabytes(sel->size);
    }
    const pmic::Detail d = rail();
    const std::pair<const char *, std::string> stats[4] = {
        {"GAIN", std::to_string(rec.gainDb) + " dB"},
        {"SIZE", size},
        {"FREE", freeTime()},
        {"MIC", d.ok ? (d.aldo2.on ? "ALDO2" : "OFF") : "ALDO2"},
    };
    const int cellW = cells.w / 4;
    for (int i = 0; i < 4; ++i) {
      const int x = cells.x + i * cellW;
      drawText(c, x + kPadX, cells.y + 8 + capHeight(style::kKey), stats[i].first, style::kKey);
      drawText(c, {x + kPadX, cells.y + 26, cellW - 2 * kPadX + 6, 24}, stats[i].second,
               kCellValue);
      if (i < 3) dashedV(c, x + cellW - 1, cells.y, cells.h);
    }

    // Memos.
    hrule(c, r.x, r.y, r.w);
    r.takeTop(kRule);
    uint32_t totalMs = 0;
    for (const Memo &m : memos_) totalMs += memoMs(m);
    drawSectionHeader(c, r.takeTop(kSectionH), "MEMOS",
                      std::to_string(memos_.size()) + " \xC2\xB7 " + clockText(totalMs));
    drawNewRow(c, r.takeTop(kNewRowH), cursor_ == 0 && !rec.on);
    const int perPage = r.h / kRowTallH;
    const int m = std::max(0, cursor_ - 1);
    const int first = perPage > 0 ? m / perPage * perPage : 0;
    for (int i = first; i < static_cast<int>(memos_.size()) && i < first + perPage; ++i) {
      Row row;
      char index[12];  // room for any int, so the compiler can see nothing is cut off
      snprintf(index, sizeof index, "%02d", i + 1);
      row.index = index;
      row.title = memoTitle(memos_[i]);
      row.subtitle = memoSubtitle(memos_[i]);
      row.trailing = clockText(memoMs(memos_[i]));
      drawRow(c, {r.x, r.y + (i - first) * kRowTallH, r.w, kRowTallH}, row, i + 1 == cursor_);
    }
  }

 private:
  bool playing() const {
    const audio::Status s = audio::status();
    return s.state != audio::State::Stopped && s.path.rfind(kRecordingsDir, 0) == 0;
  }

  const Memo *selected() const {
    return cursor_ > 0 && cursor_ <= static_cast<int>(memos_.size()) ? &memos_[cursor_ - 1]
                                                                      : nullptr;
  }

  std::string playingTitle(const std::string &p) const {
    for (const Memo &m : memos_) {
      if (path(m) == p) return memoTitle(m);
    }
    return "MEMO";
  }

  int64_t playingSize(const std::string &p) const {
    for (const Memo &m : memos_) {
      if (path(m) == p) return m.size;
    }
    return 0;
  }

  // PMIC rails, read at most every 10 s (the full read is about 30 registers).
  pmic::Detail rail() const {
    if (!railAt_ || millis() - railAt_ > 10000) {
      rail_ = pmic::detail();
      railAt_ = millis();
    }
    return rail_;
  }

  void rescan() {
    memos_ = scanMemos();
    cursor_ = std::min(cursor_, static_cast<int>(memos_.size()));
    waveFor_.clear();
  }

  void startRecording() {
    if (!sdcard::mounted()) {
      notice_ = "NO SD CARD";
      return;
    }
    mkdir(kRecordingsDir, 0775);
    const time_t now = time(nullptr);
    tm t;
    localtime_r(&now, &t);
    char name[40];
    strftime(name, sizeof name, "%Y-%m-%d %H%M%S", &t);
    std::string p = std::string(kRecordingsDir) + "/" + name + ".wav";
    struct stat st;
    for (int n = 2; stat(p.c_str(), &st) == 0; ++n) {  // a second memo in the same second
      p = std::string(kRecordingsDir) + "/" + name + "-" + std::to_string(n) + ".wav";
    }
    audio::record(p);
    delay(50);  // let the audio task start before the redraw asks
  }

  void confirmDelete(int index) {
    const std::string target = path(memos_[index]);
    const std::string title = "DELETE " + memoTitle(memos_[index]) + "?";
    ui::nav::push(std::make_unique<ui::MenuScreen>(
        title, std::vector<ui::MenuItem>{
                   {"CANCEL", nullptr, [] { ui::nav::pop(); }},
                   {"DELETE", nullptr,
                    [this, target] {
                      if (remove(target.c_str()) != 0) notice_ = "CAN'T DELETE THAT MEMO";
                      rescan();
                      countFree();
                      ui::nav::pop();
                    }},
               }));
  }

  void drawPanel(ui::Canvas &c, const Rect &area, const audio::RecordStatus &rec,
                 const audio::Status &play, bool isPlaying) {
    using namespace ui;
    const Rect p{area.x + 20, area.y + 16, area.w - 40, area.h - 30};
    // Timer and state pill.
    std::string time = "00:00", of;
    if (rec.on) {
      time = clockText(rec.elapsedMs);
    } else if (isPlaying) {
      time = clockText(play.elapsedMs);
      of = "/ " + clockText(play.durationMs);
    } else if (const Memo *m = selected()) {
      time = clockText(memoMs(*m));
    }
    const int base = p.y + capHeight(style::kBig);
    const int x = drawText(c, p.x, base, time, style::kBig);
    if (!of.empty()) drawText(c, x + 8, base, of, kTimeOf);
    const char *pill = rec.on ? (rec.paused ? "PAUSED" : "\xE2\x97\x8F REC")
                       : isPlaying ? "\xE2\x96\xB6 PLAY" : "READY";
    const bool inverted = rec.on || isPlaying;
    const int pw = tagWidth(pill, style::kPill, 10);
    drawTag(c, p.right() - pw, base - 15, 30, pill, style::kPill,
            inverted ? TagKind::Inverted : TagKind::Outlined, 10);

    // Waveform.
    const Rect wave{p.x, base + 16, p.w, kWaveH};
    dashedH(c, wave.x, wave.y, wave.w);
    dashedH(c, wave.x, wave.bottom() - 1, wave.w);
    std::vector<uint8_t> bins;
    int head = 0;  // bars before the playhead
    if (rec.on) {
      bins = rec.bins;
      head = static_cast<int>(bins.size());
    } else {
      const Memo *m = nullptr;
      if (isPlaying) {
        for (const Memo &x : memos_) {
          if (path(x) == play.path) m = &x;
        }
      } else {
        m = selected();
      }
      if (m) {
        if (waveFor_ != m->name) {
          wave_ = memoWaveform(*m);
          waveFor_ = m->name;
        }
        bins = wave_;
        head = isPlaying && play.durationMs
                   ? static_cast<int>(uint64_t(play.elapsedMs) * kBars / play.durationMs)
                   : 0;
      }
    }
    const int mid = wave.centerY();
    for (int i = 0; i < kBars; ++i) {
      const int bx = wave.x + i * 6 + (i >= head && head > 0 ? 3 : 0);
      if (bx + 4 > wave.right()) break;
      const int h = i < static_cast<int>(bins.size())
                        ? std::max(2, (6 + bins[i] * (kWaveH - 10) / 255) / 2 * 2)
                        : 2;
      const Rect bar{bx, mid - h / 2, 4, h};
      if (i < head || (!isPlaying && !rec.on && i < static_cast<int>(bins.size()))) {
        fill(c, bar, kInk);
      } else if (i < static_cast<int>(bins.size())) {
        dither50(c, bar);
      } else {
        fill(c, {bx, mid - 1, 4, 2}, kInk);  // not recorded yet: the baseline
      }
    }
    if (head > 0) fill(c, {wave.x + head * 6, wave.y, 3, wave.h}, kInk);  // playhead / now

    // The meter: the mic's level (IN, -60..0 dBFS, 3 dB a segment) while recording or with NEW
    // RECORDING selected; the playback volume (VOL, 5 % a segment, what ▲▼ sets while playing)
    // while a memo plays or is selected.
    const Rect meter{p.x, wave.bottom() + 12, p.w, 18};
    const bool volume = !rec.on && (isPlaying || selected());
    int lit = 0;
    char value[16] = "-- dB";
    if (volume) {
      lit = std::clamp(play.volume / 5, 0, kMeterSegments);
      snprintf(value, sizeof value, "%d%%", play.volume);
    } else if (rec.on) {
      lit = std::clamp(static_cast<int>((rec.levelDb + 60) / 3), 0, kMeterSegments);
      if (rec.levelDb > -96) snprintf(value, sizeof value, "%d dB", static_cast<int>(rec.levelDb));
    }
    drawText(c, meter.x, meter.y + 14, volume ? "VOL" : "IN", style::kLabel);
    drawText(c, {meter.right() - 60, meter.y, 60, meter.h}, value, style::kLabel, Align::Right);
    drawSegments(c, {meter.x + 36, meter.y, meter.w - 36 - 70, meter.h}, kMeterSegments, lit);
  }

  void drawNewRow(ui::Canvas &c, const Rect &r, bool selected) const {
    using namespace ui;
    const uint16_t ink = selected ? kPaper : kInk;
    if (selected) {
      fill(c, r, kInk);
      drawText(c, r.x + kPadX, baselineCentered(style::kRowIndex, r.y, r.h), glyph::kPlay,
               style::kRowIndex, kPaper);
    } else {
      dashedH(c, r.x, r.bottom() - 1, r.w);
    }
    const TextStyle &label = selected ? style::kRowSelected : style::kRow;
    const int x = drawText(c, r.x + kPadX + 28, baselineCentered(label, r.y, r.h), glyph::kPress,
                           label, ink);
    drawText(c, x + 10, baselineCentered(label, r.y, r.h), "NEW RECORDING", label, ink);
    drawText(c, {r.right() - kPadX - 120, r.y, 120, r.h}, freeTime() + " FREE", style::kLabel,
             Align::Right, ink);
  }

  std::vector<Memo> memos_;
  int cursor_ = 0;  // 0 = NEW RECORDING, then the memos
  std::string notice_;
  uint32_t seen_ = 0;
  uint32_t drawnAudio_ = 0;
  uint32_t drawnSecond_ = 0;
  int64_t drawnFree_ = -2;
  std::vector<uint8_t> wave_;
  std::string waveFor_;
  mutable pmic::Detail rail_;
  mutable uint32_t railAt_ = 0;
};

}  // namespace

std::unique_ptr<ui::Screen> makeRecorder() { return std::make_unique<RecorderScreen>(); }

MemoSummary memoSummary() {
  MemoSummary s;
  if (!sdcard::mounted()) return s;
  const std::vector<Memo> memos = scanMemos();
  s.count = static_cast<int>(memos.size());
  if (!memos.empty()) s.last = memoTitle(memos[0]) + " \xC2\xB7 " + clockText(memoMs(memos[0]));
  return s;
}

}  // namespace apps
