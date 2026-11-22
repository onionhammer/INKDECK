#include "features.h"

#if INKDECK_AGENT  // the whole file (features.h)

#include "apps/agent_ui.h"

#include <stdio.h>
#include <time.h>

#include <algorithm>

#include "text.h"
#include "ui/type.h"

namespace apps {
namespace agentui {

bool finished(const std::string &state) {
  return state != "working" && state != "ask" && state != "idle" && state != "scheduled" &&
         state != "watch" && state != "paused" && state != "proposed";
}

std::string stateWord(const std::string &state) {
  if (state == "ask") return "ASKING";
  if (state == "working") return "WORKING";
  if (state == "idle") return "READY";
  if (state == "done") return "DONE";
  if (state == "note") return "NOTE";
  if (state == "fail") return "FAILED";
  if (state == "stopped") return "STOPPED";
  if (state == "scheduled") return "SCHEDULED";
  if (state == "watch") return "WATCHING";
  if (state == "paused") return "PAUSED";
  if (state == "proposed") return "DRAFT";
  return ui::upper(state);
}

std::string clockText(uint32_t unix) {
  if (unix == 0) return {};
  const time_t t = static_cast<time_t>(unix);
  tm local;
  localtime_r(&t, &local);
  char buf[8];
  snprintf(buf, sizeof buf, "%02d:%02d", local.tm_hour, local.tm_min);
  return buf;
}

std::string whenText(uint32_t unix) {
  if (unix == 0) return {};
  const time_t now = time(nullptr);
  const time_t t = static_cast<time_t>(unix);
  if (t >= now - 3600 && t - now < 18 * 3600) return clockText(unix);
  tm local;
  localtime_r(&t, &local);
  char buf[16];
  strftime(buf, sizeof buf, "%a %H:%M", &local);
  return ui::upper(buf);
}

std::string costText(float usd, bool notional) {
  if (usd < 0) return {};
  char buf[16];
  snprintf(buf, sizeof buf, usd < 10 ? "%s$%.2f" : "%s$%.0f", notional ? "~" : "", usd);
  return buf;
}

std::string tagFor(const agent::Row &r) {
  if (r.state == "ask") return "ASK";
  if (r.state == "working") {
    if (r.progress >= 0) return std::to_string(std::min(r.progress, 100)) + "%";
    if (r.step > 0 && r.steps > 0) return std::to_string(r.step) + "/" + std::to_string(r.steps);
    return "RUN";
  }
  if (r.state == "idle") return "READY";
  if (r.state == "done") return "DONE";
  if (r.state == "note") return "NOTE";
  if (r.state == "fail") return "FAIL";
  if (r.state == "stopped") return "STOP";
  if (r.state == "watch") return "WATCH";
  if (r.state == "paused") return "PAUSED";
  if (r.state == "proposed") return "DRAFT";
  if (r.state == "scheduled") {
    // The next run: its time within the day, else its weekday.
    const time_t now = time(nullptr);
    if (r.nextAt == 0) return "PLAN";
    if (static_cast<time_t>(r.nextAt) - now < 18 * 3600) return clockText(r.nextAt);
    const time_t t = static_cast<time_t>(r.nextAt);
    tm local;
    localtime_r(&t, &local);
    char buf[8];
    strftime(buf, sizeof buf, "%a", &local);
    return ui::upper(buf);
  }
  return ui::upper(r.state);
}

void drawCounts(ui::Canvas &c, const ui::Rect &band, int asks, int running, int planned,
                bool known, const ui::TextStyle &valueStyle, int keyTop, int valueBottom) {
  using namespace ui;
  const struct {
    const char *key;
    int value;
    bool inverted;
  } cells[3] = {{"ASKS", asks, asks > 0}, {"RUNNING", running, false}, {"PLANNED", planned, false}};
  const int w = band.w / 3;
  const Rect inner{band.x, band.y, band.w, band.h - kRule};
  for (int i = 0; i < 3; ++i) {
    const Rect cell{band.x + i * w, inner.y, i == 2 ? band.w - 2 * w : w, inner.h};
    const uint16_t ink = cells[i].inverted ? kPaper : kInk;
    if (cells[i].inverted) fill(c, cell, kInk);
    if (i > 0) vrule(c, cell.x, cell.y, cell.h, kRule);
    drawText(c, cell.x + kPadX, cell.y + keyTop + capHeight(style::kKey), cells[i].key,
             style::kKey, ink);
    drawText(c, cell.x + kPadX, cell.bottom() - valueBottom,
             known ? std::to_string(cells[i].value) : "-", valueStyle, ink);
  }
  hrule(c, band.x, band.bottom() - kRule, band.w);
}

int drawQuestion(ui::Canvas &c, const ui::Rect &r, int y, const std::string &question,
                 int maxLines) {
  using namespace ui;
  for (const std::string &line :
       wrap(style::kReading, upper(text::toAscii(question)), r.w - 2 * kPadX, maxLines)) {
    drawText(c, r.x + kPadX, y + capHeight(style::kReading), line, style::kReading);
    y += lineHeight(style::kReading);
  }
  return y;
}

void drawSentBand(ui::Canvas &c, const ui::Rect &foot, const std::string &choice) {
  using namespace ui;
  fill(c, foot, kInk);
  drawText(c, {foot.x + kPadX, foot.y, foot.w - 2 * kPadX, foot.h},
           std::string("SENT ") + glyph::kDot + " " + upper(choice), style::kLabel, Align::Left,
           kPaper);
}

int paragraph(ui::Canvas &c, const ui::Rect &area, int top, const std::string &text,
              const ui::TextStyle &style, int maxLines) {
  using namespace ui;
  int baseline = top + capHeight(style);
  for (const std::string &line : wrap(style, text, area.w - 2 * kPadX, maxLines)) {
    drawText(c, area.x + kPadX, baseline, line, style);
    baseline += lineHeight(style);
  }
  return baseline - capHeight(style);
}

std::vector<std::string> plainLines(const std::string &markdown) {
  std::vector<std::string> out;
  size_t start = 0;
  while (start <= markdown.size()) {
    size_t end = markdown.find('\n', start);
    if (end == std::string::npos) end = markdown.size();
    std::string line = markdown.substr(start, end - start);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    size_t i = 0;
    while (i < line.size() && line[i] == '#') ++i;
    if (i > 0) {
      const size_t from = line.find_first_not_of(" #");
      line = ui::upper(from == std::string::npos ? std::string() : line.substr(from));
    }
    if (line.rfind("- [ ] ", 0) == 0 || line.rfind("- [x] ", 0) == 0) line = "- " + line.substr(6);
    std::string clean;
    for (char ch : line) {
      if (ch == '*' || ch == '`') continue;
      clean += ch;
    }
    out.push_back(clean);
    start = end + 1;
  }
  while (!out.empty() && out.back().empty()) out.pop_back();
  return out;
}

std::vector<std::string> wrapLines(const std::vector<std::string> &lines, const ui::TextStyle &s,
                                   int width) {
  std::vector<std::string> out;
  for (const std::string &line : lines) {
    if (line.empty()) {
      out.emplace_back();
      continue;
    }
    for (std::string &w : ui::wrap(s, line, width, 64)) out.push_back(std::move(w));
  }
  return out;
}

int drawChoices(ui::Canvas &c, ui::Rect &r, const std::vector<agent::AskOption> &options,
                int recommend, int cursor, bool somethingElse) {
  using namespace ui;
  const int choices = static_cast<int>(options.size()) + (somethingElse ? 1 : 0);
  int drawn = 0;
  for (int i = 0; i < choices && r.h >= kChoiceH; ++i) {
    Row row;
    row.index = std::string(1, static_cast<char>('A' + std::min(i, 25)));
    if (i < static_cast<int>(options.size())) {
      row.title = upper(options[i].title);
      row.trailing = options[i].right;
      if (i == recommend) row.tag = "PICK";
    } else {
      row.title = "SOMETHING ELSE";
      row.trailing = "HOLD BOOT";
    }
    drawRow(c, r.takeTop(kChoiceH), row, i == cursor);
    ++drawn;
  }
  return drawn;
}

std::string deadlineText(const agent::Ask &ask) {
  if (ask.deadline == 0) return {};
  const char *then = ask.then == "ask_again" ? "ASKS AGAIN"
                     : ask.then == "give_up" ? "GIVES UP"
                                             : "TAKES PICK";
  return "BY " + whenText(ask.deadline) + " " + ui::glyph::kDot + " THEN " + then;
}

}  // namespace agentui
}  // namespace apps

#endif  // INKDECK_AGENT
