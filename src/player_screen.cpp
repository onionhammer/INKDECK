#include "player_screen.h"

#include <stdio.h>

#include <string>
#include <vector>

#include "audio.h"
#include "fonts.h"
#include "screen.h"
#include "typeset.h"

namespace {

constexpr int kPadX = 24;

std::string clock(uint32_t ms) {
  const uint32_t s = ms / 1000;
  char buf[16];
  snprintf(buf, sizeof buf, "%lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
  return buf;
}

// Greedy word wrap into at most maxLines lines; the last line is ellipsized.
std::vector<std::string> wrap(const GFXfont *font, const std::string &s, int width, int maxLines) {
  std::vector<std::string> lines;
  std::string line;
  size_t i = 0;
  while (i < s.size()) {
    const size_t sp = s.find(' ', i);
    const std::string word = s.substr(i, sp == std::string::npos ? std::string::npos : sp - i);
    i = sp == std::string::npos ? s.size() : sp + 1;
    const std::string candidate = line.empty() ? word : line + " " + word;
    if (line.empty() || gfx::textWidth(font, candidate) <= width) {
      line = candidate;
    } else {
      lines.push_back(line);
      line = word;
    }
  }
  if (!line.empty()) lines.push_back(line);
  if (static_cast<int>(lines.size()) > maxLines) {
    std::string rest = lines[maxLines - 1];
    for (size_t k = maxLines; k < lines.size(); ++k) rest += " " + lines[k];
    lines.resize(maxLines);
    lines.back() = rest;
  }
  for (auto &l : lines) l = gfx::ellipsize(font, l, width);
  return lines;
}

void centered(GFXcanvas1 &c, int baseline, const std::string &s, const GFXfont *font) {
  gfx::drawText(c, (screen::kWidth - gfx::textWidth(font, s)) / 2, baseline, s, font);
}

}  // namespace

void drawNowPlaying(GFXcanvas1 &c) {
  const int w = screen::kWidth;
  const int h = screen::kHeight;
  const audio::Status s = audio::status();

  c.fillRect(0, 0, w, 52, gfx::kBlack);
  gfx::drawText(c, kPadX, 35, "Now playing", fonts::kUiBold, gfx::kWhite);

  int y = 170;
  if (s.state == audio::State::Stopped && s.title.empty()) {
    centered(c, y, "Nothing playing", fonts::kUi);
  } else {
    for (const auto &line : wrap(fonts::kUiLarge, s.title, w - 2 * kPadX, 3)) {
      centered(c, y, line, fonts::kUiLarge);
      y += fonts::kUiLarge->yAdvance;
    }
    if (!s.artist.empty()) {
      y += 6;
      centered(c, y, gfx::ellipsize(fonts::kUi, s.artist, w - 2 * kPadX), fonts::kUi);
      y += fonts::kUi->yAdvance;
    }
  }

  // State, track, time.
  y = 470;
  const char *state = s.state == audio::State::Playing  ? "Playing"
                      : s.state == audio::State::Paused ? "Paused"
                                                        : "Stopped";
  centered(c, y, state, fonts::kUiBold);
  y += 40;
  if (s.trackCount > 0) {
    centered(c, y, "Track " + std::to_string(s.track + 1) + " of " + std::to_string(s.trackCount),
             fonts::kUi);
    y += 36;
    std::string time = clock(s.elapsedMs);
    if (s.durationMs) time += " / ~" + clock(s.durationMs);
    centered(c, y, time, fonts::kUi);
  }

  // Volume bar.
  y = 640;
  const int barW = w - 2 * kPadX - 60;
  gfx::drawText(c, kPadX, y + 18, "Vol", fonts::kUiSmall);
  c.drawRect(kPadX + 40, y, barW, 24, gfx::kBlack);
  c.fillRect(kPadX + 42, y + 2, (barW - 4) * s.volume / 100, 20, gfx::kBlack);
  const std::string vol = std::to_string(s.volume);
  gfx::drawText(c, w - kPadX - gfx::textWidth(fonts::kUiSmall, vol), y + 18, vol, fonts::kUiSmall);

  // Key hints.
  const int fy = h - 64;
  c.drawFastHLine(0, fy, w, gfx::kBlack);
  centered(c, fy + 24, "Wheel: volume   Press: pause   Hold: next", fonts::kUiSmall);
  centered(c, fy + 50, "BOOT: back (keeps playing)   Hold BOOT: stop", fonts::kUiSmall);
}
