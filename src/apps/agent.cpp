#include "features.h"

#if INKDECK_AGENT  // the whole file (features.h)

#include "apps/agent.h"

#include <Arduino.h>
#include <time.h>

#include <algorithm>
#include <string>
#include <vector>

#include "agent_client.h"
#include "apps/agent_panel.h"
#include "apps/agent_ui.h"
#include "apps/home.h"
#include "apps/lock.h"
#include "apps/note.h"
#include "settings.h"
#include "text.h"
#include "ui/menu.h"

namespace apps {
namespace {

using namespace agentui;
using ui::Key;
using ui::Rect;

constexpr int kCountsH = 62;      // the three counts, rule included
constexpr int kPlanRowH = 26;     // a line of the plan's checklist
constexpr int kLogRowH = 24;      // a line of the log
constexpr int kMeterH = 10;       // the plan's segment meter
constexpr int kMaxPlanRows = 7;   // of the checklist; the rest are counted
constexpr int kScreenLineH = 18;  // a line of a herdr agent's screen
constexpr int kMaxScreenAsk = 8;  // its lines under a question
constexpr int kRunRowH = 26;      // a schedule's last runs

constexpr const char *kNoAgentHelp =
    "This inkharness keeps no sessions. Give it an [agent] table in inkharness.toml (issue #52) "
    "and restart it. HOLD BOOT still talks, on any screen.";
constexpr const char *kEmptyHelp =
    "Nothing running. HOLD BOOT and say what you need: it keeps working on inkharness while the "
    "board sleeps, and asks here when it needs you to choose.";
constexpr const char *kLockedHelp =
    "inkharness wants its PIN before it shows what the agent is doing. Press to enter it.";

const std::string kDot = ui::glyph::kDot;

// What the connection is doing, for a screen that has nothing better to show. Empty when it's
// up and has said what it holds, or when an earlier list is kept and polls keep it current.
std::string connectionNote(const agent::Status &st, const agent::Sessions &a) {
  if (st.state == agent::State::Error && !a.connected) {
    return st.error.empty() ? "OFFLINE" : ui::upper(st.error);
  }
  if (!a.connected && !a.synced) return "CONNECTING";
  return {};
}

// "CHECKS BACK 14:35" when the board isn't connected and will look again then.
std::string checksBack(const agent::Sessions &a) {
  if (a.connected) return {};
  const agent::Glance g = agent::glance();
  if (g.nextCheck == 0) return {};
  return "CHECKS BACK " + whenText(g.nextCheck);
}

void openMenu(const agent::Row &row, bool fromSession, bool hasResult);

// --- A session --------------------------------------------------------------------------------

class SessionScreen : public ui::Screen {
 public:
  SessionScreen(std::string id, std::string title) : id_(std::move(id)), title_(std::move(title)) {
    agent::hold();
  }
  ~SessionScreen() override { agent::release(); }

  std::string tag() const override { return moduleTag("AGENT"); }
  std::string context() const override { return ui::upper(title_); }
  ui::TalkTo talkTo() const override { return {true, id_}; }

  int hints(ui::Hint *out) const override {
    const agent::Sessions a = agent::sessions();
    int n = 0;
    if (asking(a)) {
      out[n++] = {ui::glyph::kDial, "MOVE"};
      if (cursor_ < static_cast<int>(a.open.ask.options.size())) {
        out[n++] = {ui::glyph::kPress, "SEND"};
      }
      out[n++] = {"HOLD BOOT", "SAY"};
    } else {
      const std::string state = a.hasOpen ? a.open.state : std::string();
      if (state == "proposed" && (a.features & agent::kJobs)) {
        out[n++] = {ui::glyph::kPress, "START"};
      } else if (state == "scheduled" && (a.features & agent::kRun)) {
        out[n++] = {ui::glyph::kPress, "RUN NOW"};
      } else if (a.hasOpen && a.open.hasPanel) {
        out[n++] = {ui::glyph::kPress, "PANEL"};
      } else if (a.hasOpen && finished(state) && !a.open.result.empty()) {
        out[n++] = {ui::glyph::kPress, "NOTE"};
      } else {
        out[n++] = {ui::glyph::kDial, "PAGE"};
      }
      out[n++] = {ui::glyph::kHoldPress, "MENU"};
      out[n++] = {"HOLD BOOT", state == "scheduled" || state == "watch" ? "CHANGE" : "TALK"};
    }
    out[n++] = {"BOOT", "BACK"};
    return n;
  }

  void onEnter() override {
    agent::follow(id_);
    guardedOnTop(Guarded::Agent);
  }
  void onLeave() override { guardedLeft(Guarded::Agent); }

  bool onKey(Key key) override {
    guardedInUse(Guarded::Agent);
    notice_.clear();
    const agent::Sessions a = agent::sessions();
    if (asking(a)) {
      const int choices = static_cast<int>(a.open.ask.options.size()) + 1;  // + something else
      switch (key) {
        case Key::Up: cursor_ = (cursor_ + choices - 1) % choices; return true;
        case Key::Down: cursor_ = (cursor_ + 1) % choices; return true;
        case Key::Select:
          if (cursor_ < choices - 1 && sent_ != a.open.ask.id) {
            agent::answer(id_, a.open.ask.id, cursor_);
            sent_ = a.open.ask.id;
            sentTitle_ = a.open.ask.options[cursor_].title;
          }
          return true;
        default: return false;
      }
    }
    const std::string state = a.hasOpen ? a.open.state : std::string();
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
        if (state == "proposed" && (a.features & agent::kJobs)) {
          agent::approve(id_);
          notice_ = "STARTING";
        } else if (state == "scheduled" && (a.features & agent::kRun)) {
          agent::runNow(id_);
          notice_ = "RUNNING NOW";
        } else if (a.hasOpen && a.open.hasPanel) {
          ui::nav::push(makeAgentPanel(id_));
        } else if (a.hasOpen && finished(state) && !a.open.result.empty()) {
          showNote(a.open);
        }
        return true;
      case Key::SelectLong: {
        agent::Row row;
        row.id = id_;
        row.title = a.hasOpen ? a.open.title : title_;
        row.state = state;
        openMenu(row, true, a.hasOpen && !a.open.result.empty());
        return true;
      }
      default: return false;
    }
  }

  bool onTick() override {
    const agent::Sessions a = agent::sessions();
    const agent::Status st = agent::status();
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
    if (asking(a) && a.open.ask.id + std::to_string(a.open.ask.at) != askSeen_) {
      askSeen_ = a.open.ask.id + std::to_string(a.open.ask.at);
      cursor_ = std::max(0, std::min(a.open.ask.recommend,
                                     static_cast<int>(a.open.ask.options.size()) - 1));
      sent_.clear();
      return true;
    }
    // A panel it drew for the first time, or a new one: shown on top, once.
    if (a.hasOpen && a.open.hasPanel && a.open.panel.at != panelSeen_ && ui::nav::top() == this) {
      const bool first = panelSeen_ == 0 && !panelOpenedOnce_;
      panelSeen_ = a.open.panel.at;
      if (first) {
        panelOpenedOnce_ = true;
        ui::nav::push(makeAgentPanel(id_));
        return false;
      }
    }
    return a.generation != drawnAgent_ || st.generation != drawnStatus_;
  }

  void draw(ui::Canvas &c, const Rect &body) override {
    using namespace ui;
    const agent::Sessions a = agent::sessions();
    const agent::Status st = agent::status();
    drawnAgent_ = a.generation;
    drawnStatus_ = st.generation;

    Rect r = body;
    const Rect head = r.takeTop(kSectionH);
    const std::string note = connectionNote(st, a);
    if (!note.empty()) {
      drawNotice(c, head, note);
    } else if (!a.unlocked && a.connected) {
      drawNotice(c, head, "LOCKED BY INKHARNESS");
    } else if (!notice_.empty()) {
      drawNotice(c, head, notice_);
    } else {
      std::string right = a.hasOpen ? clockText(a.open.started) : std::string();
      if (!right.empty()) right = "SINCE " + right;
      if (a.hasOpen) {
        const std::string cost = costText(a.open.cost, a.open.costNotional);
        if (!cost.empty()) right += (right.empty() ? "" : " " + kDot + " ") + cost;
      }
      drawSectionHeader(c, head, asking(a) ? "QUESTION" : "SESSION", right, {asking(a)});
    }
    if (!a.hasOpen) {
      paragraph(c, r, r.y + 16,
                a.unlocked || !a.connected ? "Loading the session" + std::string(glyph::kEllipsis)
                                           : kLockedHelp,
                style::kBody, 4);
      return;
    }
    const agent::Session &s = a.open;
    const std::string back = checksBack(a);
    const Rect foot = back.empty() ? Rect{} : r.takeBottom(kSectionH);
    if (!back.empty()) {
      dashedH(c, foot.x, foot.y, foot.w);
      drawText(c, {foot.x + kPadX, foot.y, foot.w - 2 * kPadX, foot.h}, back, style::kLabel);
    }

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
    if (s.state == "working" && s.expectAt != 0) state += " " + kDot + " BY " + whenText(s.expectAt);
    const int pillY = y + 4 + 11;
    const bool needsYou = s.state == "ask" || s.state == "fail" || s.state == "proposed";
    drawTag(c, r.x + kPadX, pillY, 22, state, style::kPill,
            needsYou ? TagKind::Inverted : TagKind::Outlined, 6);
    y += 4 + 22 + 10;
    r.takeTop(y - r.y);

    if (asking(a)) {
      drawAsk(c, r, s.ask, s.screen);
      return;
    }
    more_ = false;
    if (s.state == "proposed") {
      drawProposed(c, r, s, (a.features & agent::kJobs) != 0);
      return;
    }
    if (s.schedule.present && s.state != "working") {
      drawSchedule(c, r, s);
      return;
    }
    if (s.watch.present) drawWatch(c, r, s.watch);
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
  bool asking(const agent::Sessions &a) const {
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

  // A herdr agent's last screen lines, plain under their header, aligned with the rest of the
  // body (an inverted block would read as selected, and ghosts most when it refreshes every 2 s).
  void drawScreen(ui::Canvas &c, Rect &r, const std::vector<std::string> &screen, int maxLines) {
    using namespace ui;
    const int n = std::min(static_cast<int>(screen.size()), maxLines);
    if (n <= 0) return;
    hrule(c, r.x, r.y, r.w);
    r.takeTop(kRule);
    drawSectionHeader(c, r.takeTop(kSectionH), "ITS SCREEN", "LAST " + std::to_string(n) + " LINES");
    const Rect lines = r.takeTop(n * kScreenLineH + 12);
    int y = lines.y + 6;
    for (int i = static_cast<int>(screen.size()) - n; i < static_cast<int>(screen.size()); ++i) {
      drawText(c, {lines.x + kPadX, y, lines.w - 2 * kPadX, kScreenLineH}, screen[i], style::kKey);
      y += kScreenLineH;
    }
    r.takeTop(4);
  }

  void drawAsk(ui::Canvas &c, Rect &r, const agent::Ask &ask,
               const std::vector<std::string> &screen) {
    using namespace ui;
    int y = r.y + 4;
    // With its screen to show (a herdr agent), the question gets two lines and no detail.
    const int questionLines = screen.empty() ? 3 : 2;
    y = drawQuestion(c, r, y, ask.question, questionLines);
    if (!ask.detail.empty() && screen.empty()) {
      y = paragraph(c, r, y + 6, ask.detail, style::kBody, 3);
    }
    r.takeTop(y + 10 - r.y);
    if (!screen.empty()) drawScreen(c, r, screen, kMaxScreenAsk);

    const int choices = static_cast<int>(ask.options.size()) + 1;
    hrule(c, r.x, r.y, r.w);
    r.takeTop(kRule);
    const std::string deadline = deadlineText(ask);
    drawSectionHeader(c, r.takeTop(kSectionH), "CHOICES",
                      std::to_string(cursor_ + 1) + "/" + std::to_string(choices));
    const Rect foot = r.takeBottom(kSectionH);
    const Rect due = deadline.empty() ? Rect{} : r.takeBottom(kSectionH);
    drawChoices(c, r, ask.options, ask.recommend, cursor_, true);
    // The chosen option's detail, where there's room.
    if (cursor_ < choices - 1 && !ask.options[cursor_].detail.empty() && r.h > 3 * kSectionH) {
      drawSectionHeader(c, r.takeTop(kSectionH), "DETAIL");
      paragraph(c, r, r.y + 10, ask.options[cursor_].detail, style::kBody,
                std::max(1, (r.h - 16) / lineHeight(style::kBody)));
    }
    if (!deadline.empty()) {
      drawText(c, {due.x + kPadX, due.y, due.w - 2 * kPadX, due.h}, deadline, style::kLabel);
    }
    if (sent_ == ask.id) {
      drawSentBand(c, foot, sentTitle_);
    } else {
      dashedH(c, foot.x, foot.y, foot.w);
      const std::string text = cursor_ < choices - 1
                                   ? std::string(glyph::kPress) + " SENDS " +
                                         static_cast<char>('A' + cursor_)
                                   : "HOLD BOOT AND SAY IT";
      drawText(c, {foot.x + kPadX, foot.y, foot.w - 2 * kPadX, foot.h}, text, style::kLabel);
    }
  }

  // A job talk filed: what it asks for, and how to start it.
  void drawProposed(ui::Canvas &c, Rect &r, const agent::Session &s, bool canStart) {
    using namespace ui;
    hrule(c, r.x, r.y, r.w);
    r.takeTop(kRule);
    drawSectionHeader(c, r.takeTop(kSectionH), "THE JOB", "FILED BY TALK");
    const int y = paragraph(c, r, r.y + 12, s.prompt, style::kReading, 8);
    r.takeTop(y + 16 - r.y);
    paragraph(c, r, r.y,
              canStart ? "Nothing runs until you start it. HOLD BOOT to add to it as you start it."
                       : "This inkharness can't start filed jobs.",
              style::kBody, 3);
  }

  // A schedule: its time large, its days, the next run, the prompt, what the board does with a
  // result, and its last runs (failures inverted).
  void drawSchedule(ui::Canvas &c, Rect &r, const agent::Session &s) {
    using namespace ui;
    const agent::ScheduleView &v = s.schedule;
    hrule(c, r.x, r.y, r.w);
    r.takeTop(kRule);
    drawSectionHeader(c, r.takeTop(kSectionH), "SCHEDULE",
                      v.next != 0 && s.state == "scheduled" ? "NEXT " + whenText(v.next) : "");
    // The time, 64 px.
    const Rect clock = r.takeTop(84);
    drawText(c, clock.x + kPadX, clock.y + 12 + capHeight(style::kReadout), v.at, style::kReadout);
    // The week, a cell a day, filled on the days it runs.
    const Rect week = r.takeTop(40);
    const char *letters = "MTWTFSS";
    const int cell = (week.w - 2 * kPadX) / 7;
    for (int i = 0; i < 7; ++i) {
      const Rect box{week.x + kPadX + i * cell, week.y + 4, cell - 4, 30};
      const bool on = i < static_cast<int>(v.days.size()) && v.days[i] != '.';
      if (on) {
        fill(c, box, kInk);
      } else {
        frame(c, box, 2);
      }
      drawText(c, box, std::string(1, letters[i]), style::kKey, Align::Center, on ? kPaper : kInk);
    }
    r.takeTop(6);
    drawSectionHeader(c, r.takeTop(kSectionH), "EACH RUN");
    int y = paragraph(c, r, r.y + 10, v.prompt, style::kBody, 3);
    r.takeTop(y + 8 - r.y);
    drawSectionHeader(c, r.takeTop(kSectionH), "ON THE BOARD");
    std::vector<std::string> board;
    if (v.wake) {
      const uint32_t took = v.runs.empty() ? 60 : std::max<uint32_t>(60, v.runs.front().secs);
      board.push_back("WAKE THE BOARD ~" + clockText(v.next + took));
    }
    if (v.note) board.push_back("SHOW IT AS A NOTE");
    if (v.speak) board.push_back("SAY IT, ON USB ONLY");
    if (board.empty()) board.push_back("ON THE LIST, AND A CHIRP");
    for (const std::string &line : board) {
      if (r.h < kRunRowH) break;
      drawText(c, {r.x + kPadX, r.y, r.w - 2 * kPadX, kRunRowH}, line, style::kBody);
      r.takeTop(kRunRowH);
    }
    r.takeTop(4);
    if (r.h < kSectionH + kRunRowH) return;
    drawSectionHeader(c, r.takeTop(kSectionH), "LAST RUNS", std::to_string(v.runs.size()));
    if (v.runs.empty()) {
      paragraph(c, r, r.y + 8, "None yet.", style::kBody, 1);
      return;
    }
    for (const agent::RunLine &run : v.runs) {
      if (r.h < kRunRowH) break;
      const Rect line = r.takeTop(kRunRowH);
      drawText(c, {line.x + kPadX, line.y, 150, line.h}, whenText(run.at), style::kLabel);
      const bool failed = run.state == "fail";
      drawTag(c, line.x + kPadX + 150, line.centerY(), 18, stateWord(run.state), style::kTag,
              failed ? TagKind::Inverted : TagKind::Outlined, 5);
      drawText(c, {line.x + kPadX + 260, line.y, line.w - 2 * kPadX - 260, line.h},
               std::to_string(run.secs) + " S", style::kLabel, Align::Right);
    }
  }

  // A watch: what it waits for, its readings, the next look.
  void drawWatch(ui::Canvas &c, Rect &r, const agent::WatchView &w) {
    using namespace ui;
    hrule(c, r.x, r.y, r.w);
    r.takeTop(kRule);
    drawSectionHeader(c, r.takeTop(kSectionH), "WATCH",
                      w.met ? "MET" : "EVERY " + std::to_string(w.every) + " MIN", {w.met});
    int y = paragraph(c, r, r.y + 10, upper(w.condition), style::kRowSmallSelected, 2);
    r.takeTop(y + 6 - r.y);
    if (!w.values.empty()) {
      const Rect spark = r.takeTop(48).inset(kPadX, 4);
      drawSparkline(c, spark, w.values.data(), static_cast<int>(w.values.size()));
    }
    char now[24];
    snprintf(now, sizeof now, "%g", w.values.empty() ? 0.0 : static_cast<double>(w.values.back()));
    std::string line = w.values.empty() ? std::string("NO READING YET") : "NOW " + std::string(now);
    if (!w.met && w.next != 0) line += " " + kDot + " NEXT " + whenText(w.next);
    drawText(c, {r.x + kPadX, r.y, r.w - 2 * kPadX, kRunRowH}, line, style::kLabel);
    r.takeTop(kRunRowH + 4);
  }

  void drawPlan(ui::Canvas &c, Rect &r, const agent::Session &s) {
    using namespace ui;
    int done = 0;
    for (const agent::PlanStep &p : s.plan) done += p.state == 'd';
    const int steps = static_cast<int>(s.plan.size());
    hrule(c, r.x, r.y, r.w);
    r.takeTop(kRule);
    drawSectionHeader(c, r.takeTop(kSectionH), "PLAN",
                      std::to_string(done) + "/" + std::to_string(steps));
    const Rect meter = r.takeTop(kMeterH + 16).inset(kPadX, 8);
    drawSegments(c, meter, std::min(steps, 20), steps > 20 ? done * 20 / steps : done);
    const int shown = std::min(steps, kMaxPlanRows);
    for (int i = 0; i < shown; ++i) {
      const agent::PlanStep &p = s.plan[i];
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

  void drawLog(ui::Canvas &c, Rect &r, const agent::Session &s) {
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

  void showNote(const agent::Session &s) { showAgentNote(s.title, s.result); }

  std::string id_;
  std::string title_;
  int cursor_ = 0;         // the choice selected while asking
  std::string askSeen_;    // the question (id and when it was asked) cursor_ was set for
  std::string sent_;       // the question answered from here, until the next one
  std::string sentTitle_;
  int page_ = 0;           // of the result or the log
  bool more_ = false;      // a page after this one
  std::string notice_;     // a refusal for this session, until the next key
  uint32_t refusalsSeen_ = agent::sessions().refusals;
  uint32_t panelSeen_ = 0;
  bool panelOpenedOnce_ = false;
  uint32_t drawnAgent_ = 0;
  uint32_t drawnStatus_ = 0;
};

// --- The list ---------------------------------------------------------------------------------

enum class Group : uint8_t { NeedsYou, Running, Scheduled, Done };

Group groupOf(const agent::Row &r) {
  if (r.state == "ask" || r.state == "proposed") return Group::NeedsYou;
  if (r.state == "scheduled" || r.state == "watch") return Group::Scheduled;
  if (finished(r.state)) return Group::Done;
  return Group::Running;
}

const char *groupLabel(Group g) {
  switch (g) {
    case Group::NeedsYou: return "NEEDS YOU";
    case Group::Running: return "RUNNING";
    case Group::Scheduled: return "SCHEDULED";
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
  AgentList() { agent::hold(); }
  ~AgentList() override { agent::release(); }

  std::string tag() const override { return moduleTag("AGENT"); }
  std::string context() const override {
    const agent::Sessions a = agent::sessions();
    if (!a.place.empty()) return a.place;
    return settings::hasAgent() ? ui::upper(settings::agentHost()) : std::string();
  }
  ui::TalkTo talkTo() const override { return {true, ""}; }

  int hints(ui::Hint *out) const override {
    int n = 0;
    if (agent::configured()) {
      const agent::Sessions a = agent::sessions();
      const agent::Status st = agent::status();
      const bool known = a.connected || a.synced;
      if (st.state == agent::State::Error && !known) {
        out[n++] = {ui::glyph::kPress, "RETRY"};
      } else if (a.connected && a.supported && !a.unlocked) {
        if (!a.unlocking) out[n++] = {ui::glyph::kPress, "PIN"};
      } else if (known && a.supported) {
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
    agent::follow("");  // back from a session: stop following it
    guardedOnTop(Guarded::Agent);
  }
  void onLeave() override { guardedLeft(Guarded::Agent); }

  bool onKey(Key key) override {
    guardedInUse(Guarded::Agent);
    if (!agent::configured()) return false;
    const agent::Sessions a = agent::sessions();
    const agent::Status st = agent::status();
    const int rows = static_cast<int>(a.rows.size());
    const bool known = a.connected || a.synced;
    const bool usable = known && a.supported && a.unlocked;
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
        if (st.state == agent::State::Error && !known) {
          agent::retry();
        } else if (a.connected && a.supported && !a.unlocked) {
          if (!a.unlocking) {
            ui::nav::push(makePinEntry(moduleTag("AGENT"),
                                       [](const std::string &pin) { agent::unlock(pin); }));
          }
        } else if (usable && rows > 0) {
          const agent::Row &row = a.rows[indexOf(a, selected_)];
          ui::nav::push(std::make_unique<SessionScreen>(row.id, row.title));
        }
        return true;
      case Key::SelectLong:
        if (!usable || rows == 0) return false;
        openMenu(a.rows[indexOf(a, selected_)], false, false);
        return true;
      default: return false;
    }
  }

  bool onTick() override {
    const agent::Sessions a = agent::sessions();
    const agent::Status st = agent::status();
    // A new session from HOLD BOOT here: open it once the agent sheet has gone and this is on
    // top again (a turn said into an existing session isn't one: that session was on screen).
    if (!a.started.empty() && a.startedTurn != followed_ && a.startedTurn == st.turn &&
        st.agentTurn && st.turnSession.empty() && ui::nav::top() == this) {
      followed_ = a.startedTurn;
      selected_ = a.started;
      const int row = indexOf(a, a.started, -1);
      ui::nav::push(std::make_unique<SessionScreen>(a.started, row >= 0 ? a.rows[row].title : ""));
      return false;
    }
    const agent::Glance g = agent::glance();
    const bool changed = a.generation != drawnAgent_ || st.generation != drawnStatus_ ||
                         g.nextCheck != drawnCheck_;
    return changed;
  }

  void draw(ui::Canvas &c, const Rect &body) override {
    using namespace ui;
    const agent::Sessions a = agent::sessions();
    const agent::Status st = agent::status();
    drawnAgent_ = a.generation;
    drawnStatus_ = st.generation;
    drawnCheck_ = agent::glance().nextCheck;
    Rect r = body;

    if (!agent::configured()) {
      drawNotice(c, r.takeTop(kSectionH),
                 settings::hasAgent() ? "NO WI-FI NETWORK SET" : "NO AGENT HOST SET");
      paragraph(c, r, r.y + 12, kSetUpHelp, style::kBody, 6);
      return;
    }

    drawCounts(c, r.takeTop(kCountsH), a);
    const std::string note = connectionNote(st, a);
    if (!note.empty()) {
      drawNotice(c, r.takeTop(kSectionH), note);
      return;
    }
    if (!a.supported && a.connected) {
      drawNotice(c, r.takeTop(kSectionH), "NO SESSIONS ON THIS INKHARNESS");
      paragraph(c, r, r.y + 12, kNoAgentHelp, style::kBody, 6);
      return;
    }
    if (!a.unlocked && (a.connected || a.locked)) {
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
    const std::string back = checksBack(a);
    if (a.rows.empty()) {
      drawSectionHeader(c, r.takeTop(kSectionH), "SESSIONS", "0");
      paragraph(c, r, r.y + 12, kEmptyHelp, style::kBody, 6);
      return;
    }

    // The page of groups and rows that holds the selected session. Pages flip whole.
    const bool foot = a.older > 0 || !back.empty();
    const Rect footRect = foot ? r.takeBottom(kSectionH) : Rect{};
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
        const agent::Row &row = a.rows[it.row];
        Row line;
        line.title = upper(row.title);
        std::string sub = upper(row.place);
        if (!row.program.empty()) sub += (sub.empty() ? "" : " " + kDot + " ") + upper(row.program);
        if (!row.sub.empty()) sub += (sub.empty() ? "" : " " + kDot + " ") + row.sub;
        line.subtitle = sub;
        if (row.herdr) line.subtitleMark = "hc";
        line.tag = tagFor(row);
        // What needs you, and what finished that you haven't opened, stands out until you do.
        line.tagInverted = row.state == "ask" || row.state == "proposed" ||
                           (row.unread && finished(row.state));
        drawRow(c, {r.x, y, r.w, kRowTallH}, line, it.row == sel);
      }
      y += it.height();
    }
    if (foot) {
      dashedH(c, footRect.x, footRect.y, footRect.w);
      const Rect text{footRect.x + kPadX, footRect.y, footRect.w - 2 * kPadX, footRect.h};
      if (a.older > 0) drawText(c, text, "+ " + std::to_string(a.older) + " OLDER", style::kLabel);
      if (!back.empty()) drawText(c, text, back, style::kLabel, Align::Right);
    }
  }

 private:
  static int indexOf(const agent::Sessions &a, const std::string &id, int fallback = 0) {
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
  // (questions and jobs, then work, then schedules and watches, then finished); grouping here
  // keeps it so whatever it sends.
  static std::vector<Item> layout(const agent::Sessions &a) {
    std::vector<Item> items;
    for (Group g : {Group::NeedsYou, Group::Running, Group::Scheduled, Group::Done}) {
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

  void drawCounts(ui::Canvas &c, const Rect &band, const agent::Sessions &a) {
    using namespace ui;
    agentui::drawCounts(c, band, a.asks, a.running, a.planned, a.synced || !a.rows.empty(),
                        style::kReading, 10, 10);
  }

  std::string selected_;      // the session under the cursor, by id: it follows a reorder
  uint32_t followed_ = 0;     // the turn whose new session was last opened
  uint32_t drawnAgent_ = 0;
  uint32_t drawnStatus_ = 0;
  uint32_t drawnCheck_ = 0;
};

// HOLD ● on a session: what can be done with it in its state, as far as this inkharness can.
void openMenu(const agent::Row &row, bool fromSession, bool hasResult) {
  std::vector<ui::MenuItem> items;
  const std::string id = row.id;
  const std::string title = row.title;
  const std::string &st = row.state;
  const uint32_t features = agent::sessions().features;
  auto add = [&](const char *label, std::function<void()> act) {
    items.push_back({label, nullptr, [act] {
                       act();
                       ui::nav::pop();
                     }});
  };
  if (st == "proposed" && (features & agent::kJobs)) add("START", [id] { agent::approve(id); });
  if ((st == "scheduled" || st == "watch") && (features & agent::kRun)) {
    add(st == "watch" ? "LOOK NOW" : "RUN NOW", [id] { agent::runNow(id); });
  }
  const bool pausable = st == "working" || st == "ask" || st == "scheduled" || st == "watch";
  if (pausable && (features & agent::kPause)) add("PAUSE", [id] { agent::pause(id); });
  if (st == "paused" && (features & agent::kPause)) add("RESUME", [id] { agent::resume(id); });
  if (fromSession && hasResult) {
    items.push_back({"SHOW AS A NOTE", nullptr, [id, title] {
                       ui::nav::pop();
                       const agent::Sessions a = agent::sessions();
                       if (a.hasOpen && a.open.id == id) showAgentNote(title, a.open.result);
                     }});
  }
  if (st == "working" || st == "ask" || st == "scheduled" || st == "watch" || st == "paused") {
    add("STOP", [id] { agent::stop(id); });
  }
  items.push_back({"DELETE", nullptr, [id, fromSession] {
                     agent::remove(id);
                     ui::nav::pop();                   // the menu
                     if (fromSession) ui::nav::pop();  // and the session, now gone
                   }});
  ui::nav::push(std::make_unique<ui::MenuScreen>(ui::upper(row.title), std::move(items)));
}

agent::Target target() {
  const ui::TalkTo to = ui::nav::talkTarget();
  return {to.agent, to.session};
}

// The status bar's ?n: questions waiting, as the last list said (counts only: no PIN needed).
int asksWaiting() {
  const agent::Glance g = agent::glance();
  return g.known ? g.asks : 0;
}

// Titles may go on the face and into NVS unless the PIN guards the Agent (agent_face_details
// says otherwise).
bool contentAllowed() { return !guardApplies(Guarded::Agent) || settings::agentFaceDetails(); }

}  // namespace

std::unique_ptr<ui::Screen> makeAgent() { return std::make_unique<AgentList>(); }

std::unique_ptr<ui::Screen> makeAgentSession(const std::string &id, const std::string &title) {
  return std::make_unique<SessionScreen>(id, title);
}

void showAgentNote(const std::string &title, const std::string &markdown) {
  Note note;
  note.title = title.empty() ? "Agent" : title;
  note.text = markdown;
  std::string err;
  if (!saveNote(note, err)) log_w("agent: note not kept: %s", err.c_str());
  ui::nav::push(makeNote(note, false, true));
}

void beginAgent() {
  agent::setTarget(target);
  agent::setContentPolicy(contentAllowed);
  onPinEntered(agent::unlock);
  ui::nav::onAsks(asksWaiting);
}

}  // namespace apps

#endif  // INKDECK_AGENT
