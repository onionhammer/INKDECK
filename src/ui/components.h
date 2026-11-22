// Shared UI components: the chrome every portrait screen wears (status bar, section header,
// key-hint bar) and the reusable pieces inside screens (list rows, tags, meters, button,
// stat cells). Geometry and type follow docs/ui-design.md and the design canvas it links.
//
// Components draw into a Rect they're given and never decide layout beyond it.
#pragma once
#include <functional>
#include <string>

#include "ui/draw.h"
#include "ui/type.h"

namespace ui {

// --- Chrome geometry ("Shared chrome" in docs/ui-design.md). Heights include their 3 px rule.
constexpr int kStatusBarH = 36;
constexpr int kSectionH = 30;
constexpr int kHintBarH = 34;
constexpr int kPadX = 14;  // side padding of chrome bands and list rows
constexpr int kRowH = 34;  // single-line list row
constexpr int kRowTallH = 48;  // two-line list row

// --- Status bar: inverted module tag, context text, then [▶/‖] [?n] [WIFI] [temp] battery % and
// clock.
enum class Playback : uint8_t { None, Playing, Paused };
struct StatusInfo {
  std::string tag;      // "01 READER"
  std::string context;  // "~/home", a path, a book title
  bool contextIsPath = false;  // too long: drop the start ("…/b/c") instead of the end
  std::string temp;     // optional, e.g. "21.4°C"
  int battery = -1;     // percent; -1 = no battery reading
  bool usb = false;     // on USB power (shown when there's no battery reading)
  std::string clock;    // "14:32", or "--:--" when the time isn't known; empty: none
  bool wifi = false;    // the radio is on: "WIFI" before the rest
  Playback playback = Playback::None;  // music or a podcast: an inverted ▶ or ‖ first
  int asks = 0;  // agent questions waiting: an inverted ?n after the ▶ (issue #52)
};
void drawStatusBar(Canvas &c, const Rect &r, const StatusInfo &info);
int drawBattery(Canvas &c, int x, int centerY, int percent);  // returns its width
// The inverted ▶ or ‖ box, 22 x 20, left edge at x; returns its width (0 for None).
int drawPlaybackMark(Canvas &c, int x, int centerY, Playback playback);

// --- Section header: "// LABEL" on the left, a count or address on the right, rule below.
struct SectionOptions {
  bool inverted = false;  // white on black: the focused panel of a screen with several
  bool prefix = true;     // the "// " before the label (off for "$ i2cdetect -y 0")
};
void drawSectionHeader(Canvas &c, const Rect &r, const std::string &label,
                       const std::string &right = "", SectionOptions options = {});

// --- Notice: an inverted band the height of a section header, "! text". For a one-off
// message (an error opening a file) in place of the header.
void drawNotice(Canvas &c, const Rect &r, const std::string &text);

// An empty list's body (Files, the Library): `title` in the title style, then `text` wrapped
// under it (four lines at most).
void drawEmpty(Canvas &c, const Rect &r, const char *title, const std::string &text);

// --- Key-hint bar: up to 4 key caps with verbs, spread across the bottom band.
struct Hint {
  const char *cap;   // "▲▼", "●", "HOLD ●", "BOOT"
  const char *verb;  // "MOVE", "OPEN", ...
};
constexpr int kMaxHints = 4;
void drawHintBar(Canvas &c, const Rect &r, const Hint *hints, int count);

// --- Sheet: the panel an overlay rises in from the bottom of the body (the agent and player
// sheets): half the body but at least minH, its top rule casting a 6 px 50 % dither shadow up
// onto the screen below it. Returns the area under the rule. With hints, a key-hint bar along its
// bottom too, for a screen below without chrome (the navigator draws the hints only in chrome).
Rect drawSheet(Canvas &c, const Rect &body, int minH, const Hint *hints = nullptr, int count = 0);
int drawKeycap(Canvas &c, int x, int centerY, const char *cap);  // returns its width
int keycapWidth(const char *cap);

// --- Tags and pills: a word in a box, inverted (ink fill) or outlined (2 px border).
enum class TagKind : uint8_t { Inverted, Outlined };
// Draws at x, vertically centred on centerY; returns the width. `color` is the ink colour
// (pass kPaper to draw on an inverted background).
int drawTag(Canvas &c, int x, int centerY, int h, const std::string &text, const TextStyle &s,
            TagKind kind, int padX, uint16_t color = kInk);
int tagWidth(const std::string &text, const TextStyle &s, int padX);

// --- List rows. Selected rows are inverted with a ▶ marker; others get a dashed rule under.
struct Row {
  std::string index;     // "01", drawn after the marker column; optional
  std::string title;
  std::string subtitle;  // optional second line (use kRowTallH)
  std::string subtitleMark;  // optional small outlined tag before the subtitle ("hc": a herdr agent)
  std::string tag;       // optional boxed tag, right-aligned ("SD", "DIR")
  bool tagInverted = false;  // the tag filled instead of outlined: it needs you ("1 ASK")
  std::string trailing;  // optional plain text, right-aligned ("3:05")
};
void drawRow(Canvas &c, const Rect &r, const Row &row, bool selected);

// Cursor + paging for a list. Pages flip whole (e-paper friendly) instead of scrolling a row
// at a time.
class ListCursor {
 public:
  void reset(int count, int cursor = 0);
  // Clamps at the ends, or wraps round (a dial over a short list). True if it moved. While the
  // dial is held long enough (input::lastKeyFast()), a list longer than a page moves a page at a
  // time, landing on the first row of the next or previous page.
  bool move(int delta, bool wrap = false);
  void set(int cursor);
  int cursor() const { return cursor_; }
  int count() const { return count_; }
  int pageStart(int rowsPerPage) const;  // also remembers the page size for move()
  std::string label() const;  // "03/10"

 private:
  int count_ = 0;
  int cursor_ = 0;
  mutable int pageRows_ = 0;  // rows per page at the last draw; 0 until drawn
};

// Draws the page of rows containing the cursor. rowAt(i) builds row i.
template <typename RowAt>
void drawList(Canvas &c, const Rect &r, int rowH, const ListCursor &cursor, RowAt rowAt) {
  const int perPage = r.h / rowH;
  const int first = cursor.pageStart(perPage);
  for (int i = first; i < cursor.count() && i < first + perPage; ++i) {
    drawRow(c, {r.x, r.y + (i - first) * rowH, r.w, rowH}, rowAt(i), i == cursor.cursor());
  }
}

// --- Meters.
// Bordered bar: solid for the done part, 50 % dither for the rest (reader progress).
void drawProgress(Canvas &c, const Rect &r, float fraction);
// Row of outlined segments, the first `filled` solid (volume).
void drawSegments(Canvas &c, const Rect &r, int segments, int filled, int gap = 3);
// Bars for `values`, oldest on the left, scaled between their least and greatest (a span under
// minSpan isn't blown up to full height), on a 2 px axis along the bottom.
void drawSparkline(Canvas &c, const Rect &r, const float *values, int count, float minSpan = 0);

// --- Button: inverted box with a dithered drop shadow offset 6 px ("● OPEN").
// Draws with its right edge at `right` and top at `top`; returns the box (without shadow).
Rect drawButton(Canvas &c, int right, int top, const std::string &glyph,
                const std::string &label);

// --- An off face's foot band (the closed book's cover, the Agent face): a paper rule, then ink
// with "HOLD [PWR] TO RESUME" on the left and `right` on the right, in paper.
void drawResumeBand(Canvas &c, const Rect &band, const std::string &right);

// "HOLD [PWR] TO BOOT", the off faces' way back: "HOLD", the PWR key outlined, then `verb`,
// along `line` (24 px tall), in `color`. Returns the x it ends at.
int drawHoldPwr(Canvas &c, const Rect &line, const char *verb, uint16_t color = kInk);

// A dot-matrix field (the frogs on Sleep and the News face): cols x rows cells of `cell` px
// from (x0, y0). at(x, y) is each cell's character ('\0' outside): '#' ink, '+' a 50 % dither;
// an empty cell is an unlit dot, or the 25 % shadow of a lit cell up and left of it. Other
// characters stay paper.
void drawDotField(Canvas &c, int x0, int y0, int cols, int rows, int cell,
                  const std::function<char(int, int)> &at);

// --- A row of four stat cells, key over value, dashed rules between (the PMIC on Sensors,
// the Level's and the Recorder's footers).
struct Stat {
  const char *key;
  std::string value;
};
void drawStatRow(Canvas &c, const Rect &r, const Stat (&cells)[4]);

// --- Stat cell: small key over a bold value, dashed rule under (Home preview, Music footer).
void drawStat(Canvas &c, const Rect &r, const std::string &key, const std::string &value);

}  // namespace ui
