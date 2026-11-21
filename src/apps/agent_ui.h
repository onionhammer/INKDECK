// What the Agent's screens share (apps/agent.h, the ask sheet, panels, the Agent face): words
// for states and tags, times, paragraphs, the result's Markdown as plain lines, and the list of
// choices under a question. Only in a build with INKDECK_AGENT (features.h).
#pragma once
#include <stdint.h>

#include <string>
#include <vector>

#include "agent_client.h"
#include "ui/components.h"

namespace apps {
namespace agentui {

// Done, note, fail, stopped, and any state a later inkharness adds that this board doesn't
// know (docs/voice-protocol.md says to take those as finished).
bool finished(const std::string &state);
// "WORKING", "ASKING", "SCHEDULED", "DRAFT"...
std::string stateWord(const std::string &state);
// A row's tag: ASK, 4/7 or 41%, READY, 07:00 or TUE (a schedule's next run), WATCH, PAUSED,
// DRAFT, DONE, NOTE, FAIL, STOP.
std::string tagFor(const agent::Row &r);
// "14:32", or "" when the board doesn't know the time.
std::string clockText(uint32_t unix);
// "14:32" today (or within the next 18 hours), else "TUE 07:00".
std::string whenText(uint32_t unix);
// "$0.04", "~$0.04" when notional; "" for none.
std::string costText(float usd, bool notional);

// Word-wrapped text from `top`; returns the y under it.
int paragraph(ui::Canvas &c, const ui::Rect &area, int top, const std::string &text,
              const ui::TextStyle &style, int maxLines);
// The result's Markdown as lines to read on the panel: headings, bullets and emphasis marks
// dropped (the Note viewer shows it properly).
std::vector<std::string> plainLines(const std::string &markdown);
// Wraps lines for the panel; an empty line stays as a gap.
std::vector<std::string> wrapLines(const std::vector<std::string> &lines, const ui::TextStyle &s,
                                   int width);

// The choices under a question, a row each (A, B, ...), from the top of `r`, which shrinks by
// what they take: PICK on the recommended one, its right value, the cursor's row inverted, and
// with somethingElse a last row SOMETHING ELSE · HOLD BOOT. Returns how many fitted.
constexpr int kChoiceH = 34;
int drawChoices(ui::Canvas &c, ui::Rect &r, const std::vector<agent::AskOption> &options,
                int recommend, int cursor, bool somethingElse);
// "BY 15:30 · THEN TAKES PICK": what a question's deadline does, or "".
std::string deadlineText(const agent::Ask &ask);

}  // namespace agentui
}  // namespace apps
