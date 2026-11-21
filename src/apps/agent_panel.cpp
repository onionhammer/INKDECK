#include "features.h"

#if INKDECK_AGENT  // the whole file (features.h)

#include "apps/agent_panel.h"

#include <algorithm>
#include <string>
#include <vector>

#include "agent_client.h"
#include "apps/agent_ui.h"
#include "apps/home.h"
#include "apps/lock.h"

namespace apps {
namespace {

using namespace agentui;
using ui::Key;
using ui::Rect;

constexpr int kStatH = 56;
constexpr int kKvH = 24;
constexpr int kMeterH = 46;
constexpr int kSparkH = 76;
constexpr int kKeyW = 150;  // a kv line's key column

const std::string kDot = ui::glyph::kDot;

bool known(const agent::PanelBlock &b) {
  return b.kind == "section" || b.kind == "stat" || b.kind == "kv" || b.kind == "rows" ||
         b.kind == "meter" || b.kind == "spark" || b.kind == "text";
}

// The text block's lines, as drawn.
std::vector<std::string> textLines(const agent::PanelBlock &b, int width) {
  return wrapLines(plainLines(b.text), ui::style::kBody, width);
}

// How tall a block draws at this width.
int heightOf(const agent::PanelBlock &b, int width) {
  using namespace ui;
  if (b.kind == "section") return kSectionH;
  if (b.kind == "stat") return kStatH;
  if (b.kind == "kv") return static_cast<int>(b.items.size()) * kKvH + 8;
  if (b.kind == "rows") {
    int h = 0;
    for (const agent::PanelItem &i : b.items) h += i.b.empty() ? kRowH : kRowTallH;
    return h;
  }
  if (b.kind == "meter") return kMeterH;
  if (b.kind == "spark") return kSparkH;
  if (b.kind == "text") {
    return static_cast<int>(textLines(b, width - 2 * kPadX).size()) * lineHeight(style::kBody) + 16;
  }
  return 0;
}

void drawBlock(ui::Canvas &c, const Rect &r, const agent::PanelBlock &b) {
  using namespace ui;
  if (b.kind == "section") {
    drawSectionHeader(c, r, upper(b.label), upper(b.right));
  } else if (b.kind == "stat") {
    const int n = std::max(1, std::min(static_cast<int>(b.items.size()), 4));
    const int w = r.w / n;
    for (int i = 0; i < n; ++i) {
      const Rect cell{r.x + i * w, r.y, i == n - 1 ? r.w - (n - 1) * w : w, r.h};
      drawStat(c, cell.inset(kPadX / 2, 4), upper(b.items[i].a), b.items[i].b);
    }
  } else if (b.kind == "kv") {
    int y = r.y + 4;
    for (const agent::PanelItem &i : b.items) {
      drawText(c, {r.x + kPadX, y, kKeyW, kKvH}, upper(i.a), style::kKey);
      drawText(c, {r.x + kPadX + kKeyW, y, r.w - 2 * kPadX - kKeyW, kKvH}, i.b, style::kBody);
      y += kKvH;
    }
  } else if (b.kind == "rows") {
    int y = r.y;
    for (const agent::PanelItem &i : b.items) {
      Row row;
      row.title = upper(i.a);
      row.subtitle = i.b;
      row.trailing = i.c;
      row.tag = upper(i.d);
      const int h = i.b.empty() ? kRowH : kRowTallH;
      drawRow(c, {r.x, y, r.w, h}, row, false);
      y += h;
    }
  } else if (b.kind == "meter") {
    const Rect line{r.x + kPadX, r.y + 4, r.w - 2 * kPadX, 18};
    drawText(c, line, upper(b.label), style::kKey);
    if (!b.right.empty()) drawText(c, line, upper(b.right), style::kLabel, Align::Right);
    const float f = b.max > 0 ? std::clamp(b.value / b.max, 0.0f, 1.0f) : 0;
    drawProgress(c, {r.x + kPadX, r.y + 26, r.w - 2 * kPadX, 14}, f);
  } else if (b.kind == "spark") {
    const Rect line{r.x + kPadX, r.y + 4, r.w - 2 * kPadX, 18};
    drawText(c, line, upper(b.label), style::kKey);
    if (!b.right.empty()) drawText(c, line, upper(b.right), style::kLabel, Align::Right);
    drawSparkline(c, {r.x + kPadX, r.y + 26, r.w - 2 * kPadX, 44}, b.values.data(),
                  static_cast<int>(b.values.size()));
  } else if (b.kind == "text") {
    int baseline = r.y + 8 + capHeight(style::kBody);
    for (const std::string &line : textLines(b, r.w - 2 * kPadX)) {
      drawText(c, r.x + kPadX, baseline, line, style::kBody);
      baseline += lineHeight(style::kBody);
    }
  }
}

class PanelScreen : public ui::Screen {
 public:
  explicit PanelScreen(std::string id) : id_(std::move(id)) { agent::hold(); }
  ~PanelScreen() override { agent::release(); }

  std::string tag() const override { return moduleTag("AGENT"); }
  std::string context() const override {
    const agent::Sessions a = agent::sessions();
    return panelOf(a) ? ui::upper(a.open.panel.title) : std::string("PANEL");
  }
  ui::TalkTo talkTo() const override { return {true, id_}; }

  int hints(ui::Hint *out) const override {
    const agent::Sessions a = agent::sessions();
    int n = 0;
    const agent::PanelBlock *choice = panelOf(a) ? choiceOf(a.open.panel) : nullptr;
    if (choice) {
      out[n++] = {ui::glyph::kDial, "CHOOSE"};
      out[n++] = {ui::glyph::kPress, "SEND"};
    } else {
      out[n++] = {ui::glyph::kDial, "SCROLL"};
    }
    out[n++] = {"HOLD BOOT", "TALK"};
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
    const agent::Sessions a = agent::sessions();
    if (!panelOf(a)) return false;
    const agent::PanelBlock *choice = choiceOf(a.open.panel);
    const int options = choice ? static_cast<int>(choice->items.size()) : 0;
    switch (key) {
      case Key::Up:
        if (choice && cursor_ > 0) {
          --cursor_;
        } else if (first_ > 0) {
          --first_;
        } else {
          return false;
        }
        return true;
      case Key::Down:
        if (choice && cursor_ < options - 1) {
          ++cursor_;
        } else if (more_) {
          ++first_;
        } else {
          return false;
        }
        return true;
      case Key::Select:
        if (!choice || cursor_ >= options || sentAt_ == a.open.panel.at) return false;
        agent::panelChoice(id_, choice->id, cursor_);
        sentAt_ = a.open.panel.at;
        sentTitle_ = choice->items[cursor_].a;
        return true;
      default: return false;
    }
  }

  bool onTick() override {
    const agent::Sessions a = agent::sessions();
    if (panelOf(a) && a.open.panel.at != seenAt_) {
      seenAt_ = a.open.panel.at;
      const agent::PanelBlock *choice = choiceOf(a.open.panel);
      cursor_ = choice && choice->recommend >= 0
                    ? std::min(choice->recommend, static_cast<int>(choice->items.size()) - 1)
                    : 0;
      return true;
    }
    return a.generation != drawn_;
  }

  void draw(ui::Canvas &c, const Rect &body) override {
    using namespace ui;
    const agent::Sessions a = agent::sessions();
    drawn_ = a.generation;
    Rect r = body;
    if (!panelOf(a)) {
      drawSectionHeader(c, r.takeTop(kSectionH), "PANEL", "LOADING");
      return;
    }
    const agent::Panel &p = a.open.panel;
    // The choice, pinned at the foot.
    const agent::PanelBlock *choice = choiceOf(p);
    if (choice) {
      const int options = static_cast<int>(choice->items.size());
      const int footH = kSectionH + options * kChoiceH + kSectionH + kRule;
      Rect foot = r.takeBottom(std::min(footH, r.h / 2));
      hrule(c, foot.x, foot.y, foot.w);
      foot.takeTop(kRule);
      drawSectionHeader(c, foot.takeTop(kSectionH), upper(choice->question), "", {true});
      const Rect line = foot.takeBottom(kSectionH);
      std::vector<agent::AskOption> opts;
      for (const agent::PanelItem &i : choice->items) opts.push_back({i.a, i.c, ""});
      drawChoices(c, foot, opts, choice->recommend, cursor_, false);
      if (sentAt_ == p.at) {
        fill(c, line, kInk);
        drawText(c, {line.x + kPadX, line.y, line.w - 2 * kPadX, line.h},
                 "SENT " + kDot + " " + upper(sentTitle_), style::kLabel, Align::Left, kPaper);
      } else {
        dashedH(c, line.x, line.y, line.w);
        drawText(c, {line.x + kPadX, line.y, line.w - 2 * kPadX, line.h},
                 std::string(glyph::kPress) + " SENDS " + static_cast<char>('A' + cursor_),
                 style::kLabel);
      }
    }
    // The rest, from the first block shown, as far as it goes.
    std::vector<const agent::PanelBlock *> blocks;
    for (const agent::PanelBlock &b : p.blocks) {
      if (known(b)) blocks.push_back(&b);
    }
    first_ = std::max(0, std::min(first_, static_cast<int>(blocks.size()) - 1));
    more_ = false;
    int y = r.y;
    for (int i = first_; i < static_cast<int>(blocks.size()); ++i) {
      const int h = heightOf(*blocks[i], r.w);
      if (y + h > r.bottom()) {
        more_ = true;
        break;
      }
      drawBlock(c, {r.x, y, r.w, h}, *blocks[i]);
      y += h;
    }
    if (more_ || first_ > 0) {
      const Rect mark{r.x, r.bottom() - 20, r.w, 20};
      fill(c, mark, kPaper);
      dashedH(c, mark.x, mark.y, mark.w);
      std::string text = first_ > 0 ? std::string("\xE2\x96\xB2 ") + std::to_string(first_) : "";
      if (more_) text += (text.empty() ? "" : "   ") + std::string("\xE2\x96\xBC MORE");
      drawText(c, {mark.x + kPadX, mark.y, mark.w - 2 * kPadX, mark.h}, text, style::kLabel,
               Align::Right);
    }
  }

 private:
  bool panelOf(const agent::Sessions &a) const {
    return a.hasOpen && a.open.id == id_ && a.open.hasPanel;
  }
  static const agent::PanelBlock *choiceOf(const agent::Panel &p) {
    for (const agent::PanelBlock &b : p.blocks) {
      if (b.kind == "choice" && !b.id.empty() && !b.items.empty()) return &b;
    }
    return nullptr;
  }

  std::string id_;
  int first_ = 0;        // the first block shown
  bool more_ = false;    // blocks below the last shown
  int cursor_ = 0;       // the choice's option
  uint32_t seenAt_ = 0;  // the panel the cursor was set for
  uint32_t sentAt_ = 0;  // the panel whose choice was sent from here
  std::string sentTitle_;
  uint32_t drawn_ = 0;
};

}  // namespace

std::unique_ptr<ui::Screen> makeAgentPanel(const std::string &session) {
  return std::make_unique<PanelScreen>(session);
}

}  // namespace apps

#endif  // INKDECK_AGENT
