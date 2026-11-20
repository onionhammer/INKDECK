#include "features.h"

#if INKDECK_VOICE  // the whole file (features.h)

#include "apps/agent.h"

#include <Arduino.h>
#include <time.h>

#include <algorithm>
#include <string>
#include <vector>

#include "apps/home.h"
#include "apps/lock.h"
#include "apps/note.h"
#include "settings.h"
#include "text.h"
#include "ui/menu.h"
#include "voice_client.h"

namespace apps {
namespace {

using ui::Key;
using ui::Rect;

constexpr int kCountsH = 62;      // the three counts, rule included
constexpr int kChoiceH = 34;      // a choice under a question
constexpr int kPlanRowH = 26;     // a line of the plan's checklist
constexpr int kLogRowH = 24;      // a line of the log
constexpr int kMeterH = 10;       // the plan's segment meter
constexpr int kMaxPlanRows = 7;   // of the checklist; the rest are counted
constexpr int kScreenLineH = 18;  // a line of a herdr agent's screen
constexpr int kMaxScreenAsk = 8;  // its lines under a question

constexpr const char *kSetUpHelp =
    "Name the machine that runs inkharness in SD:/inkdeck.ini: voice_host, and voice_token if it "
    "asks for one. Then restart the board.";
constexpr const char *kNoAgentHelp =
    "This inkharness keeps no sessions. Give it an [agent] table in inkharness.toml (issue #52) "
    "and restart it. HOLD BOOT still talks, on any screen.";
constexpr const char *kEmptyHelp =
    "Nothing running. HOLD BOOT and say what you need: it keeps working on inkharness while the "
    "board sleeps, and asks here when it needs you to choose.";
constexpr const char *kLockedHelp =
    "inkharness wants its PIN before it shows what the agent is doing. Press to enter it.";

const std::string kDot = ui::glyph::kDot;

// Done, note, fail, stopped, and any state a later inkharness adds that this board doesn't know
// (docs/voice-protocol.md says to take those as finished).
bool finished(const std::string &state) {
  return state != "working" && state != "ask" && state != "idle";
}

std::string stateWord(const std::string &state) {
  if (state == "ask") return "ASKING";
  if (state == "working") return "WORKING";
  if (state == "idle") return "READY";
  if (state == "done") return "DONE";
  if (state == "note") return "NOTE";
  if (state == "fail") return "FAILED";
  if (state == "stopped") return "STOPPED";
  return ui::upper(state);
}

std::string tagFor(const voice::AgentRow &r) {
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
  return ui::upper(r.state);
}

// "14:32", or "" when the board doesn't know the time.
std::string clockText(uint32_t unix) {
  if (unix == 0) return {};
  const time_t t = static_cast<time_t>(unix);
  tm local;
  localtime_r(&t, &local);
  char buf[8];
  snprintf(buf, sizeof buf, "%02d:%02d", local.tm_hour, local.tm_min);
  return buf;
}

// Word-wrapped text from `top`; returns the y under it.
int paragraph(ui::Canvas &c, const Rect &area, int top, const std::string &text,
              const ui::TextStyle &style, int maxLines) {
  using namespace ui;
  int baseline = top + capHeight(style);
  for (const std::string &line : wrap(style, text, area.w - 2 * kPadX, maxLines)) {
    drawText(c, area.x + kPadX, baseline, line, style);
    baseline += lineHeight(style);
  }
  return baseline - capHeight(style);
}

// The result's Markdown as lines to read on the panel: headings, bullets and emphasis marks
// dropped (the Note viewer shows it properly: ● on a finished session).
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
    if (i > 0) line = ui::upper(line.substr(line.find_first_not_of(" #") == std::string::npos
                                                 ? line.size()
                                                 : line.find_first_not_of(" #")));
    if (line.rfind("- [ ] ", 0) == 0 || line.rfind("- [x] ", 0) == 0) line = "- " + line.substr(6);
    std::string clean;
    for (size_t k = 0; k < line.size(); ++k) {
      if (line[k] == '*' || line[k] == '`') continue;
      clean += line[k];
    }
    out.push_back(clean);
    start = end + 1;
  }
  while (!out.empty() && out.back().empty()) out.pop_back();
  return out;
}

// Wraps lines for the panel; an empty line stays as a gap.
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

// What the connection is doing, for a screen that has nothing better to show. Empty when it's
// up and has said what it holds.
std::string connectionNote(const voice::Status &st, const voice::Agent &a) {
  if (st.state == voice::State::Error && !a.connected) {
    return st.error.empty() ? "OFFLINE" : ui::upper(st.error);
  }
  if (!a.connected) return "CONNECTING";
  return {};
}

void openMenu(const voice::AgentRow &row, bool fromSession);

// --- A session --------------------------------------------------------------------------------

class SessionScreen : public ui::Screen {
 public:
  SessionScreen(std::string id, std::string title) : id_(std::move(id)), title_(std::move(title)) {
    voice::agentHold();
  }
  ~SessionScreen() override { voice::agentRelease(); }

  std::string tag() const override { return moduleTag("AGENT"); }
  std::string context() const override { return ui::upper(title_); }
  ui::TalkTo talkTo() const override { return {true, id_}; }

  int hints(ui::Hint *out) const override {
    const voice::Agent a = voice::agent();
    int n = 0;
    if (asking(a)) {
      out[n++] = {ui::glyph::kDial, "MOVE"};
      if (cursor_ < static_cast<int>(a.open.ask.options.size())) {
        out[n++] = {ui::glyph::kPress, "SEND"};
      }
      out[n++] = {"HOLD BOOT", "SAY"};
    } else {
      if (a.hasOpen && finished(a.open.state) && !a.open.result.empty()) {
        out[n++] = {ui::glyph::kPress, "NOTE"};
      } else {
        out[n++] = {ui::glyph::kDial, "PAGE"};
      }
      out[n++] = {ui::glyph::kHoldPress, "MENU"};
      out[n++] = {"HOLD BOOT", "TALK"};
    }
    out[n++] = {"BOOT", "BACK"};
    return n;
  }

  void onEnter() override {
    voice::agentOpen(id_);
    guardedOnTop(Guarded::Agent);
  }
  void onLeave() override { guardedLeft(Guarded::Agent); }

  bool onKey(Key key) override {
    guardedInUse(Guarded::Agent);
    notice_.clear();
    const voice::Agent a = voice::agent();
    if (asking(a)) {
      const int choices = static_cast<int>(a.open.ask.options.size()) + 1;  // + something else
      switch (key) {
        case Key::Up: cursor_ = (cursor_ + choices - 1) % choices; return true;
        case Key::Down: cursor_ = (cursor_ + 1) % choices; return true;
        case Key::Select:
          if (cursor_ < choices - 1 && sent_ != a.open.ask.id) {
            voice::agentAnswer(id_, a.open.ask.id, cursor_);
            sent_ = a.open.ask.id;
            sentTitle_ = a.open.ask.options[cursor_].title;
          }
          return true;
        default: return false;
      }
    }
    switch (key) {
      case Key::Up:
        if (page_ == 0) return false;
        --page_;
        return true;
      case Key::Down:
        if (!more_) return false;
        ++page_;
        return true;
      case Key::Select:
        if (a.hasOpen && finished(a.open.state) && !a.open.result.empty()) showNote(a.open);
        return true;
      case Key::SelectLong: {
        voice::AgentRow row;
        row.id = id_;
        row.title = a.hasOpen ? a.open.title : title_;
        row.state = a.hasOpen ? a.open.state : "";
        openMenu(row, true);
        return true;
      }
      default: return false;
    }
  }

  bool onTick() override {
    const voice::Agent a = voice::agent();
    const voice::Status st = voice::status();
    if (a.hasOpen && !a.open.title.empty()) title_ = a.open.title;
    // inkharness turned something down for this session (a stale answer, a busy agent, a
    // [herdr] policy): say so until the next key, and let the answer be sent again.
    if (a.refusals != refusalsSeen_) {
      refusalsSeen_ = a.refusals;
      if (a.refusalSession == id_) {
        notice_ = ui::upper(a.refusal);
        sent_.clear();
        return true;
      }
    }
    // A new question: start at the recommended choice.
    if (asking(a) && a.open.ask.id != askSeen_) {
      askSeen_ = a.open.ask.id;
      cursor_ = std::max(0, std::min(a.open.ask.recommend,
                                     static_cast<int>(a.open.ask.options.size()) - 1));
      return true;
    }
    return a.generation != drawnAgent_ || st.generation != drawnStatus_;
  }

  void draw(ui::Canvas &c, const Rect &body) override {
    using namespace ui;
    const voice::Agent a = voice::agent();
    const voice::Status st = voice::status();
    drawnAgent_ = a.generation;
    drawnStatus_ = st.generation;

    Rect r = body;
    const Rect head = r.takeTop(kSectionH);
    const std::string note = connectionNote(st, a);
    if (!note.empty()) {
      drawNotice(c, head, note);
    } else if (!a.unlocked) {
      drawNotice(c, head, "LOCKED BY INKHARNESS");
    } else if (!notice_.empty()) {
      drawNotice(c, head, notice_);
    } else {
      const std::string since = a.hasOpen ? clockText(a.open.started) : std::string();
      drawSectionHeader(c, head, asking(a) ? "QUESTION" : "SESSION",
                        since.empty() ? std::string() : "SINCE " + since, {asking(a)});
    }
    if (!a.hasOpen) {
      paragraph(c, r, r.y + 16, a.unlocked ? "Loading the session" + std::string(glyph::kEllipsis)
                                           : kLockedHelp,
                style::kBody, 4);
      return;
    }
    const voice::AgentSession &s = a.open;

    // The title and its state.
    int y = r.y + 12;
    for (const std::string &line : wrap(style::kRowSelected, upper(s.title), r.w - 2 * kPadX, 2)) {
      drawText(c, r.x + kPadX, y + capHeight(style::kRowSelected), line, style::kRowSelected);
      y += lineHeight(style::kRowSelected);
    }
    y += 2;
    drawWhere(c, r.x + kPadX, y + capHeight(style::kLabel), s.herdr, s.place, s.program);
    y += lineHeight(style::kLabel) + 2;
    std::string state = stateWord(s.state);
    if (s.state == "working" && s.step > 0 && s.steps > 0) {
      state += " " + kDot + " STEP " + std::to_string(s.step) + "/" + std::to_string(s.steps);
    }
    const int pillY = y + 4 + 11;
    drawTag(c, r.x + kPadX, pillY, 22, state, style::kPill,
            s.state == "ask" || s.state == "fail" ? TagKind::Inverted : TagKind::Outlined, 6);
    y += 4 + 22 + 10;
    r.takeTop(y - r.y);

    if (asking(a)) {
      drawAsk(c, r, s.ask, s.screen);
      return;
    }
    more_ = false;
    if (!s.plan.empty()) drawPlan(c, r, s);
    if (finished(s.state) && !s.result.empty()) {
      if (s.herdr && !s.screen.empty()) {
        // A herdr agent's result is its last word; what its terminal shows says more.
        r.takeTop(paragraph(c, r, r.y + 10, s.result, style::kBody, 3) + 10 - r.y);
        drawScreen(c, r, s.screen, (r.h - kSectionH - 30) / kScreenLineH);
        return;
      }
      drawResult(c, r, s.result);
      return;
    }
    // A herdr agent: what its terminal shows says more than a log.
    if (!s.screen.empty()) drawScreen(c, r, s.screen, (r.h - kSectionH - 40) / kScreenLineH);
    drawLog(c, r, s);
  }

 private:
  bool asking(const voice::Agent &a) const {
    return a.unlocked && a.hasOpen && a.open.asking && !a.open.ask.options.empty();
  }

  // "[hc] DESKTOP · CLAUDE CODE": where a session runs, and on what.
  static void drawWhere(ui::Canvas &c, int x, int baseline, bool herdr, const std::string &place,
                        const std::string &program) {
    using namespace ui;
    if (herdr) {
      x += drawTag(c, x, baseline - capHeight(style::kLabel) / 2, 15, "hc", style::kTag,
                   TagKind::Outlined, 3) +
           5;
    }
    std::string text = upper(place);
    if (!program.empty()) text += (text.empty() ? "" : " " + kDot + " ") + upper(program);
    drawText(c, x, baseline, text, style::kLabel);
  }

  // A herdr agent's last screen lines, paper on ink, as on its terminal.
  void drawScreen(ui::Canvas &c, Rect &r, const std::vector<std::string> &screen, int maxLines) {
    using namespace ui;
    const int n = std::min(static_cast<int>(screen.size()), maxLines);
    if (n <= 0) return;
    hrule(c, r.x, r.y, r.w);
    r.takeTop(kRule);
    drawSectionHeader(c, r.takeTop(kSectionH), "ITS SCREEN", "LAST " + std::to_string(n) + " LINES");
    const Rect box = r.takeTop(n * kScreenLineH + 20).inset(kPadX, 6);
    fill(c, box, kInk);
    int y = box.y + 4;
    for (int i = static_cast<int>(screen.size()) - n; i < static_cast<int>(screen.size()); ++i) {
      drawText(c, {box.x + 10, y, box.w - 20, kScreenLineH}, screen[i], style::kKey, Align::Left,
               kPaper);
      y += kScreenLineH;
    }
    r.takeTop(4);
  }

  void drawAsk(ui::Canvas &c, Rect &r, const voice::Ask &ask,
               const std::vector<std::string> &screen) {
    using namespace ui;
    int y = r.y + 4;
    // With its screen to show (a herdr agent), the question gets two lines and no detail.
    const int questionLines = screen.empty() ? 3 : 2;
    for (const std::string &line : wrap(style::kReading, upper(text::toAscii(ask.question)),
                                        r.w - 2 * kPadX, questionLines)) {
      drawText(c, r.x + kPadX, y + capHeight(style::kReading), line, style::kReading);
      y += lineHeight(style::kReading);
    }
    if (!ask.detail.empty() && screen.empty()) {
      y = paragraph(c, r, y + 6, ask.detail, style::kBody, 3);
    }
    r.takeTop(y + 10 - r.y);
    if (!screen.empty()) drawScreen(c, r, screen, kMaxScreenAsk);

    const int choices = static_cast<int>(ask.options.size()) + 1;
    hrule(c, r.x, r.y, r.w);
    r.takeTop(kRule);
    drawSectionHeader(c, r.takeTop(kSectionH), "CHOICES",
                      std::to_string(cursor_ + 1) + "/" + std::to_string(choices));
    const Rect foot = r.takeBottom(kSectionH);
    for (int i = 0; i < choices && r.h >= kChoiceH; ++i) {
      Row row;
      row.index = std::string(1, static_cast<char>('A' + std::min(i, 25)));
      if (i < choices - 1) {
        row.title = upper(ask.options[i].title);
        row.trailing = ask.options[i].right;
        if (i == ask.recommend) row.tag = "PICK";
      } else {
        row.title = "SOMETHING ELSE";
        row.trailing = "HOLD BOOT";
      }
      drawRow(c, r.takeTop(kChoiceH), row, i == cursor_);
    }
    // The chosen option's detail, where there's room.
    if (cursor_ < choices - 1 && !ask.options[cursor_].detail.empty() && r.h > 3 * kSectionH) {
      drawSectionHeader(c, r.takeTop(kSectionH), "DETAIL");
      paragraph(c, r, r.y + 10, ask.options[cursor_].detail, style::kBody,
                std::max(1, (r.h - 16) / lineHeight(style::kBody)));
    }
    if (sent_ == ask.id) {
      fill(c, foot, kInk);
      drawText(c, {foot.x + kPadX, foot.y, foot.w - 2 * kPadX, foot.h},
               "SENT " + kDot + " " + upper(sentTitle_), style::kLabel, Align::Left, kPaper);
    } else {
      dashedH(c, foot.x, foot.y, foot.w);
      const std::string text = cursor_ < choices - 1
                                   ? std::string(glyph::kPress) + " SENDS " +
                                         static_cast<char>('A' + cursor_)
                                   : "HOLD BOOT AND SAY IT";
      drawText(c, {foot.x + kPadX, foot.y, foot.w - 2 * kPadX, foot.h}, text, style::kLabel);
    }
  }

  void drawPlan(ui::Canvas &c, Rect &r, const voice::AgentSession &s) {
    using namespace ui;
    int done = 0;
    for (const voice::PlanStep &p : s.plan) done += p.state == 'd';
    const int steps = static_cast<int>(s.plan.size());
    hrule(c, r.x, r.y, r.w);
    r.takeTop(kRule);
    drawSectionHeader(c, r.takeTop(kSectionH), "PLAN",
                      std::to_string(done) + "/" + std::to_string(steps));
    const Rect meter = r.takeTop(kMeterH + 16).inset(kPadX, 8);
    drawSegments(c, meter, std::min(steps, 20), steps > 20 ? done * 20 / steps : done);
    const int shown = std::min(steps, kMaxPlanRows);
    for (int i = 0; i < shown; ++i) {
      const voice::PlanStep &p = s.plan[i];
      const Rect line = r.takeTop(kPlanRowH);
      const char *mark = p.state == 'd' ? "\xE2\x9C\x93" : p.state == 'n' ? glyph::kPlay : kDot.c_str();
      drawText(c, {line.x + kPadX, line.y, 20, line.h}, mark, style::kRowSmall);
      drawText(c, {line.x + kPadX + 24, line.y, line.w - 2 * kPadX - 24, line.h}, p.text,
               p.state == 'n' ? style::kRowSmallSelected : style::kBody);
    }
    if (steps > shown) {
      drawText(c, {r.x + kPadX, r.y, r.w - 2 * kPadX, kPlanRowH},
               "+ " + std::to_string(steps - shown) + " MORE", style::kLabel);
      r.takeTop(kPlanRowH);
    }
    r.takeTop(6);
  }

  void drawResult(ui::Canvas &c, Rect &r, const std::string &result) {
    using namespace ui;
    hrule(c, r.x, r.y, r.w);
    r.takeTop(kRule);
    const Rect head = r.takeTop(kSectionH);
    const std::vector<std::string> lines =
        wrapLines(plainLines(result), style::kBody, r.w - 2 * kPadX);
    const int per = std::max(1, (r.h - 16) / lineHeight(style::kBody));
    const int pages = std::max(1, (static_cast<int>(lines.size()) + per - 1) / per);
    page_ = std::min(page_, pages - 1);
    more_ = page_ < pages - 1;
    drawSectionHeader(c, head, "RESULT",
                      pages > 1 ? "P. " + std::to_string(page_ + 1) + "/" + std::to_string(pages)
                                : "");
    int baseline = r.y + 12 + capHeight(style::kBody);
    for (int i = page_ * per; i < static_cast<int>(lines.size()) && i < (page_ + 1) * per; ++i) {
      drawText(c, r.x + kPadX, baseline, lines[i], style::kBody);
      baseline += lineHeight(style::kBody);
    }
  }

  void drawLog(ui::Canvas &c, Rect &r, const voice::AgentSession &s) {
    using namespace ui;
    if (r.h < kSectionH + kLogRowH) return;
    hrule(c, r.x, r.y, r.w);
    r.takeTop(kRule);
    const Rect head = r.takeTop(kSectionH);
    const int per = std::max(1, (r.h - 8) / kLogRowH);
    const int count = static_cast<int>(s.log.size());
    const int pages = std::max(1, (count + per - 1) / per);
    page_ = std::min(page_, pages - 1);
    more_ = page_ < pages - 1;
    drawSectionHeader(c, head, "LOG",
                      pages > 1 ? "P. " + std::to_string(page_ + 1) + "/" + std::to_string(pages)
                                : std::to_string(count));
    if (count == 0) {
      paragraph(c, r, r.y + 10,
                s.state == "working" ? "Starting" + std::string(glyph::kEllipsis) : s.prompt,
                style::kBody, 4);
      return;
    }
    int y = r.y + 4;
    for (int i = page_ * per; i < count && i < (page_ + 1) * per; ++i) {
      const std::string at = clockText(s.log[i].at);
      const int atW = at.empty() ? 0 : textWidth(style::kLabel, "00:00") + 10;
      if (!at.empty()) drawText(c, {r.x + kPadX, y, atW, kLogRowH}, at, style::kLabel);
      drawText(c, {r.x + kPadX + atW, y, r.w - 2 * kPadX - atW, kLogRowH}, s.log[i].text,
               style::kBody);
      y += kLogRowH;
    }
  }

  void showNote(const voice::AgentSession &s) {
    Note note;
    note.title = s.title.empty() ? "Agent" : s.title;
    note.text = s.result;
    std::string err;
    if (!saveNote(note, err)) log_w("agent: note not kept: %s", err.c_str());
    ui::nav::push(makeNote(note, false, true));
  }

  std::string id_;
  std::string title_;
  int cursor_ = 0;         // the choice selected while asking
  std::string askSeen_;    // the question cursor_ was set for
  std::string sent_;       // the question answered from here, until the next one
  std::string sentTitle_;
  int page_ = 0;           // of the result or the log
  bool more_ = false;      // a page after this one
  std::string notice_;     // a refusal for this session, until the next key
  uint32_t refusalsSeen_ = voice::agent().refusals;
  uint32_t drawnAgent_ = 0;
  uint32_t drawnStatus_ = 0;
};

// --- The list ---------------------------------------------------------------------------------

enum class Group : uint8_t { NeedsYou, Running, Done };

Group groupOf(const voice::AgentRow &r) {
  if (r.state == "ask") return Group::NeedsYou;
  if (finished(r.state)) return Group::Done;
  return Group::Running;
}

const char *groupLabel(Group g) {
  switch (g) {
    case Group::NeedsYou: return "NEEDS YOU";
    case Group::Running: return "RUNNING";
    case Group::Done: return "DONE";
  }
  return "";
}

// One line of the list as drawn: a group's header or a session.
struct Item {
  bool header = false;
  Group group = Group::Running;
  int row = -1;  // into the rows, for a session
  int count = 0;  // for a header: its sessions
  int height() const { return header ? ui::kSectionH : ui::kRowTallH; }
};

class AgentList : public ui::Screen {
 public:
  AgentList() { voice::agentHold(); }
  ~AgentList() override { voice::agentRelease(); }

  std::string tag() const override { return moduleTag("AGENT"); }
  std::string context() const override {
    const voice::Agent a = voice::agent();
    if (!a.place.empty()) return a.place;
    return settings::hasVoice() ? ui::upper(settings::voiceHost()) : std::string();
  }
  ui::TalkTo talkTo() const override { return {true, ""}; }

  int hints(ui::Hint *out) const override {
    int n = 0;
    if (voice::configured()) {
      const voice::Agent a = voice::agent();
      const voice::Status st = voice::status();
      if (st.state == voice::State::Error && !a.connected) {
        out[n++] = {ui::glyph::kPress, "RETRY"};
      } else if (a.connected && a.supported && !a.unlocked) {
        if (!a.unlocking) out[n++] = {ui::glyph::kPress, "PIN"};
      } else if (a.connected && a.supported) {
        if (!a.rows.empty()) {
          out[n++] = {ui::glyph::kPress, "OPEN"};
          out[n++] = {ui::glyph::kHoldPress, "MENU"};
        }
        out[n++] = {"HOLD BOOT", "NEW"};
      }
    }
    out[n++] = {"BOOT", "BACK"};
    return n;
  }

  void onEnter() override {
    voice::agentOpen("");  // back from a session: stop following it
    guardedOnTop(Guarded::Agent);
  }
  void onLeave() override { guardedLeft(Guarded::Agent); }

  bool onKey(Key key) override {
    guardedInUse(Guarded::Agent);
    if (!voice::configured()) return false;
    const voice::Agent a = voice::agent();
    const voice::Status st = voice::status();
    const int rows = static_cast<int>(a.rows.size());
    const bool usable = a.connected && a.supported && a.unlocked;
    switch (key) {
      case Key::Up:
      case Key::Down: {
        if (!usable || rows == 0) return false;
        const int now = indexOf(a, selected_);
        const int next = std::clamp(now + (key == Key::Down ? 1 : -1), 0, rows - 1);
        if (next == now) return false;
        selected_ = a.rows[next].id;
        return true;
      }
      case Key::Select:
        if (st.state == voice::State::Error && !a.connected) {
          voice::agentRetry();
        } else if (a.connected && a.supported && !a.unlocked) {
          if (!a.unlocking) {
            ui::nav::push(makePinEntry(moduleTag("AGENT"),
                                       [](const std::string &pin) { voice::agentUnlock(pin); }));
          }
        } else if (usable && rows > 0) {
          const voice::AgentRow &row = a.rows[indexOf(a, selected_)];
          ui::nav::push(std::make_unique<SessionScreen>(row.id, row.title));
        }
        return true;
      case Key::SelectLong:
        if (!usable || rows == 0) return false;
        openMenu(a.rows[indexOf(a, selected_)], false);
        return true;
      default: return false;
    }
  }

  bool onTick() override {
    const voice::Agent a = voice::agent();
    const voice::Status st = voice::status();
    // A new session from HOLD BOOT here: select it once the list has it.
    if (!a.started.empty() && a.startedTurn != followed_ && indexOf(a, a.started, -1) >= 0) {
      selected_ = a.started;
      followed_ = a.startedTurn;
      return true;
    }
    return a.generation != drawnAgent_ || st.generation != drawnStatus_;
  }

  void draw(ui::Canvas &c, const Rect &body) override {
    using namespace ui;
    const voice::Agent a = voice::agent();
    const voice::Status st = voice::status();
    drawnAgent_ = a.generation;
    drawnStatus_ = st.generation;
    Rect r = body;

    if (!voice::configured()) {
      drawNotice(c, r.takeTop(kSectionH),
                 settings::hasVoice() ? "NO WI-FI NETWORK SET" : "NO VOICE HOST SET");
      paragraph(c, r, r.y + 12, kSetUpHelp, style::kBody, 6);
      return;
    }

    drawCounts(c, r.takeTop(kCountsH), a);
    const std::string note = connectionNote(st, a);
    if (!note.empty()) {
      drawNotice(c, r.takeTop(kSectionH), note);
      return;
    }
    if (!a.supported) {
      drawNotice(c, r.takeTop(kSectionH), "NO SESSIONS ON THIS INKHARNESS");
      paragraph(c, r, r.y + 12, kNoAgentHelp, style::kBody, 6);
      return;
    }
    if (!a.unlocked) {
      const std::string why = a.unlocking ? "UNLOCKING" + std::string(glyph::kEllipsis)
                              : a.unlockNote.empty() ? std::string("LOCKED BY INKHARNESS")
                                                     : a.unlockNote;
      drawNotice(c, r.takeTop(kSectionH), why);
      paragraph(c, r, r.y + 12, kLockedHelp, style::kBody, 4);
      return;
    }
    if (!a.synced) {
      drawSectionHeader(c, r.takeTop(kSectionH), "SESSIONS", "LOADING");
      return;
    }
    if (a.rows.empty()) {
      drawSectionHeader(c, r.takeTop(kSectionH), "SESSIONS", "0");
      paragraph(c, r, r.y + 12, kEmptyHelp, style::kBody, 6);
      return;
    }

    // The page of groups and rows that holds the selected session. Pages flip whole.
    const Rect foot = a.older > 0 ? r.takeBottom(kSectionH) : Rect{};
    const std::vector<Item> items = layout(a);
    const int sel = indexOf(a, selected_);
    int pageStart = 0, used = 0;
    for (int i = 0; i < static_cast<int>(items.size()); ++i) {
      int need = items[i].height();
      // A header goes with the row under it, or to the next page.
      if (items[i].header && i + 1 < static_cast<int>(items.size())) {
        need += items[i + 1].height();
      }
      if (used > 0 && used + need > r.h) {
        if (rowOnPage(items, pageStart, i, sel)) break;
        pageStart = i;
        used = 0;
      }
      used += items[i].height();
    }
    int y = r.y;
    for (int i = pageStart; i < static_cast<int>(items.size()); ++i) {
      const Item &it = items[i];
      if (y + it.height() > r.bottom()) break;
      if (it.header) {
        drawSectionHeader(c, {r.x, y, r.w, kSectionH}, groupLabel(it.group),
                          std::to_string(it.count), {it.group == Group::NeedsYou});
      } else {
        const voice::AgentRow &row = a.rows[it.row];
        Row line;
        line.title = upper(row.title);
        std::string sub = upper(row.place);
        if (!row.program.empty()) sub += (sub.empty() ? "" : " " + kDot + " ") + upper(row.program);
        if (!row.sub.empty()) sub += (sub.empty() ? "" : " " + kDot + " ") + row.sub;
        line.subtitle = sub;
        if (row.herdr) line.subtitleMark = "hc";
        line.tag = tagFor(row);
        if (row.unread && finished(row.state)) line.trailing = "NEW";
        drawRow(c, {r.x, y, r.w, kRowTallH}, line, it.row == sel);
      }
      y += it.height();
    }
    if (a.older > 0) {
      dashedH(c, foot.x, foot.y, foot.w);
      drawText(c, {foot.x + kPadX, foot.y, foot.w - 2 * kPadX, foot.h},
               "+ " + std::to_string(a.older) + " OLDER", style::kLabel);
    }
  }

 private:
  static int indexOf(const voice::Agent &a, const std::string &id, int fallback = 0) {
    for (int i = 0; i < static_cast<int>(a.rows.size()); ++i) {
      if (a.rows[i].id == id) return i;
    }
    return a.rows.empty() ? -1 : fallback;
  }

  static bool rowOnPage(const std::vector<Item> &items, int from, int to, int row) {
    for (int i = from; i < to; ++i) {
      if (!items[i].header && items[i].row == row) return true;
    }
    return false;
  }

  // Groups in their order, each under its header. The daemon sends them in that order already
  // (questions, then work, then finished); grouping here keeps it so whatever it sends.
  static std::vector<Item> layout(const voice::Agent &a) {
    std::vector<Item> items;
    for (Group g : {Group::NeedsYou, Group::Running, Group::Done}) {
      const size_t header = items.size();
      int count = 0;
      for (int i = 0; i < static_cast<int>(a.rows.size()); ++i) {
        if (groupOf(a.rows[i]) != g) continue;
        if (count++ == 0) items.push_back({true, g, -1, 0});
        items.push_back({false, g, i, 0});
      }
      if (count > 0) items[header].count = count;
    }
    return items;
  }

  void drawCounts(ui::Canvas &c, const Rect &band, const voice::Agent &a) {
    using namespace ui;
    const bool known = a.synced || !a.rows.empty();
    struct Cell {
      const char *key;
      int value;
      bool inverted;
    } cells[3] = {{"ASKS", a.asks, a.asks > 0}, {"RUNNING", a.running, false},
                  {"PLANNED", a.planned, false}};
    const int w = band.w / 3;
    const Rect inner{band.x, band.y, band.w, band.h - kRule};
    for (int i = 0; i < 3; ++i) {
      const Rect cell{band.x + i * w, inner.y, i == 2 ? band.w - 2 * w : w, inner.h};
      const uint16_t ink = cells[i].inverted ? kPaper : kInk;
      if (cells[i].inverted) fill(c, cell, kInk);
      if (i > 0) vrule(c, cell.x, cell.y, cell.h, kRule);
      drawText(c, cell.x + kPadX, cell.y + 10 + capHeight(style::kKey), cells[i].key, style::kKey,
               ink);
      drawText(c, cell.x + kPadX, cell.bottom() - 10,
               known ? std::to_string(cells[i].value) : "-", style::kReading, ink);
    }
    hrule(c, band.x, band.bottom() - kRule, band.w);
  }

  std::string selected_;      // the session under the cursor, by id: it follows a reorder
  uint32_t followed_ = 0;     // the turn whose new session was last selected
  uint32_t drawnAgent_ = 0;
  uint32_t drawnStatus_ = 0;
};

// HOLD ● on a session: stop it while it works or asks, delete it.
void openMenu(const voice::AgentRow &row, bool fromSession) {
  std::vector<ui::MenuItem> items;
  const std::string id = row.id;
  if (row.state == "working" || row.state == "ask") {
    items.push_back({"STOP", nullptr, [id] {
                       voice::agentStop(id);
                       ui::nav::pop();
                     }});
  }
  items.push_back({"DELETE", nullptr, [id, fromSession] {
                     voice::agentDelete(id);
                     ui::nav::pop();               // the menu
                     if (fromSession) ui::nav::pop();  // and the session, now gone
                   }});
  ui::nav::push(std::make_unique<ui::MenuScreen>(ui::upper(row.title), std::move(items)));
}

voice::Target target() {
  const ui::TalkTo to = ui::nav::talkTarget();
  return {to.agent, to.session};
}

}  // namespace

std::unique_ptr<ui::Screen> makeAgent() { return std::make_unique<AgentList>(); }

void beginAgent() {
  voice::setTarget(target);
  onPinEntered(voice::agentUnlock);
}

}  // namespace apps

#endif  // INKDECK_VOICE
