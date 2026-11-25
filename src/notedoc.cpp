#include "notedoc.h"

#include <ctype.h>
#include <stdlib.h>
#include <strings.h>

#include <algorithm>

#include "text.h"
#include "typeset.h"
#include "ui/draw.h"
#include "ui/fonts/glyphs.h"
#include "ui/fonts/jbmono.h"
#include "ui/packed_fonts.h"
#include "ui/type.h"

namespace {

const GFXfont *const kTitleFont = &ui::fonts::kMono28ExtraBold;
constexpr int kTitleLineH = 36;
constexpr int kTitleMaxLines = 3;
constexpr int kTitleRuleGap = 10;  // the title's last line to its rule
constexpr int kTitleRuleH = 3;
constexpr int kAfterTitle = 22;  // the rule to the first block
constexpr int kBylineGap = 12;   // the rule to the byline
constexpr int kBylineH = 17;
constexpr int kAfterByline = 18;  // the byline to the first block
constexpr ui::TextStyle kBylineStyle{&ui::fonts::kMono13Bold, 1};
const GFXfont *const kHeadingFont = &ui::fonts::kMono15ExtraBold;
constexpr int kHeadingTracking = 1;
constexpr int kHeadingDashGap = 12;  // a heading to its dashed rule
constexpr int kQuoteIndent = 20;
constexpr int kQuoteBarW = 3;

constexpr uint8_t kBulletSlot = ui::fonts::slotFor(0x2022);     // •
constexpr uint8_t kDashSlot = ui::fonts::slotFor(0x2013);       // – a bullet one level in
constexpr uint8_t kCheckedSlot = ui::fonts::slotFor(0x25A0);    // ■
constexpr uint8_t kUncheckedSlot = ui::fonts::slotFor(0x25A1);  // □
constexpr uint8_t kEllipsisSlot = ui::fonts::slotFor(0x2026);   // …
// A space no line breaks at ("175 °C", "23 × 13 cm"): measured and drawn as a space.
constexpr char kNoBreak = static_cast<char>(0xA0);
constexpr char kDegree = static_cast<char>(0xB0);
constexpr char kTimes = static_cast<char>(0xD7);

int glyphAdvance(const GFXfont *f, char c) { return gfx::advance(f, c == kNoBreak ? ' ' : c); }

bool hasGlyph(const GFXfont *f, uint8_t slot) { return ui::fonts::glyph(f, slot).xAdvance > 0; }

int capHeight(const GFXfont *f) { return -ui::fonts::glyph(f, 'H').yOffset; }

// Appends cp to flow text in font's slots: its glyph if the font has one, else a stand-in or
// ASCII. Emoji and other pictographs go. Never a control byte.
void appendChar(std::string &out, uint32_t cp, const GFXfont *font) {
  static const char *const kFractions[] = {"1/7", "1/9", "1/10", "1/3", "2/3", "1/5", "2/5", "3/5",
                                           "4/5", "1/6", "5/6",  "1/8", "3/8", "5/8", "7/8"};
  switch (cp) {
    case 0x2713: case 0x2714: case 0x2611: case 0x2612: case 0x2705:  // ✓ ✔ ☑ ☒ ✅
      cp = 0x25A0;  // ■
      break;
    case 0x2610:  // ☐
      cp = 0x25A1;  // □
      break;
    case 0x25CF: case 0x25CB: case 0x25E6: case 0x25AA: case 0x25AB: case 0x2219:
    case 0x2043: case 0x25C6: case 0x25C7:  // ● ○ ◦ ▪ ▫ ∙ ⁃ ◆ ◇
      cp = 0x2022;  // •
      break;
    case 0x25B8: case 0x25BA: case 0x27A4:  // ▸ ► ➤
      cp = 0x25B6;  // ▶
      break;
    case 0x2794: case 0x279C: case 0x27A1: case 0x21D2:  // ➔ ➜ ➡ ⇒
      cp = 0x2192;  // →
      break;
    case 0x2015:
      cp = 0x2014;  // a horizontal bar: an em dash
      break;
    case 0x2717: case 0x2718: case 0x274C: out += 'x'; return;  // ✗ ✘ ❌
    case 0x2605: case 0x2606: out += '*'; return;               // ★ ☆
    case 0x2248: out += '~'; return;                             // ≈
    case 0x2264: out += "<="; return;
    case 0x2265: out += ">="; return;
    case 0x2260: out += "!="; return;
    case 0x2103: appendChar(out, 0xB0, font); out += 'C'; return;  // ℃
    case 0x2109: appendChar(out, 0xB0, font); out += 'F'; return;  // ℉
    case 0xAD: return;                                              // soft hyphen
    default: break;
  }
  if (cp >= 0x2150 && cp <= 0x215E) {  // ⅐ … ⅞
    if (!out.empty() && isdigit(static_cast<unsigned char>(out.back()))) out += ' ';  // 1 1/3
    out += kFractions[cp - 0x2150];
    return;
  }
  if ((cp >= 0x1F000 && cp <= 0x1FAFF) || (cp >= 0x2600 && cp <= 0x27BF) ||
      (cp >= 0x2B00 && cp <= 0x2BFF) || (cp >= 0xFE00 && cp <= 0xFE0F) || cp == 0x200D ||
      cp == 0x20E3 || cp >= 0xE0000) {
    return;  // emoji, their modifiers and joiners
  }
  const uint8_t slot = ui::fonts::slotFor(cp);
  if (slot && hasGlyph(font, slot)) {
    out += static_cast<char>(slot);
  } else if ((cp == 0xBC || cp == 0xBD || cp == 0xBE) && !out.empty() &&
             isdigit(static_cast<unsigned char>(out.back()))) {
    out += ' ';  // "1½" as "1 1/2"
    text::appendAscii(out, cp);
  } else {
    text::appendAscii(out, cp);  // ' ' for other spaces, nothing for control characters
  }
}

// A run of at least `run` c's after `from` on the same line, after something other than a
// space: what closes emphasis that opens there.
bool hasCloser(const std::string &s, size_t from, char c, size_t run) {
  for (size_t k = from; k < s.size() && s[k] != '\n';) {
    if (s[k] != c) {
      ++k;
      continue;
    }
    size_t n = 1;
    while (k + n < s.size() && s[k + n] == c) ++n;
    if (n >= run && s[k - 1] != ' ') return true;
    k += n;
  }
  return false;
}

// Appends the inline Markdown in s as flow text: style bytes for bold and italic, the marks of
// code, strikethrough, links and images dropped, HTML tags dropped (<br> breaks the line) and
// entities decoded. '\n' stays a line break.
void inlineFlow(std::string &out, const std::string &s, const GFXfont *font, uint8_t &style) {
  const size_t n = s.size();
  auto setStyle = [&](uint8_t st) {
    if (st == style) return;
    style = st;
    out += static_cast<char>(flow::kStyleBase + st);
  };
  auto at = [&](size_t k) { return k < n ? s[k] : ' '; };
  auto wordChar = [](char c) { return isalnum(static_cast<unsigned char>(c)) || (c & 0x80); };
  size_t i = 0;
  while (i < n) {
    const char c = s[i];
    if (c == '\n') {
      out += '\n';
      ++i;
      continue;
    }
    if (c == '\\' && i + 1 < n && ispunct(static_cast<unsigned char>(s[i + 1]))) {
      out += s[i + 1];
      i += 2;
      continue;
    }
    if (c == '`') {  // code: plain text
      ++i;
      continue;
    }
    if (c == '~' && at(i + 1) == '~') {  // ~~strikethrough~~: plain text
      i += 2;
      continue;
    }
    if (c == '*' || c == '_') {
      size_t run = 1;
      while (i + run < n && s[i + run] == c) ++run;
      const char before = i > 0 ? s[i - 1] : ' ';
      const char after = at(i + run);
      const bool loose = before == ' ' && after == ' ';                    // "2 * 3"
      const bool inWord = c == '_' && wordChar(before) && wordChar(after);  // snake_case
      const uint8_t bits = (run >= 2 ? flow::kBold : 0) | (run == 1 || run >= 3 ? flow::kItalic : 0);
      // Closing what's open, or opening with a closer later on the line: else it's a "*".
      if (!loose && !inWord && ((style & bits) == bits || hasCloser(s, i + run, c, run))) {
        setStyle(style ^ bits);
      } else {
        out.append(run, c);
      }
      i += run;
      continue;
    }
    if (c == '[' || (c == '!' && at(i + 1) == '[')) {  // [text](url), ![alt](url): the text
      const size_t open = c == '!' ? i + 1 : i;
      const size_t close = s.find(']', open + 1);
      if (close != std::string::npos && at(close + 1) == '(') {
        const size_t end = s.find(')', close + 2);
        if (end != std::string::npos) {
          inlineFlow(out, s.substr(open + 1, close - open - 1), font, style);
          i = end + 1;
          continue;
        }
      }
    }
    if (c == '<') {  // <https://...>: the address; <tag>: gone
      const size_t close = s.find('>', i + 1);
      if (close != std::string::npos && close - i < 300) {
        const std::string inner = s.substr(i + 1, close - i - 1);
        if (inner.rfind("http://", 0) == 0 || inner.rfind("https://", 0) == 0 ||
            inner.rfind("mailto:", 0) == 0) {
          for (size_t k = 0; k < inner.size();) appendChar(out, text::nextCodepoint(inner, k), font);
          i = close + 1;
          continue;
        }
        size_t k = !inner.empty() && inner[0] == '/' ? 1 : 0;
        if (k < inner.size() && isalpha(static_cast<unsigned char>(inner[k])) &&
            inner.find('<') == std::string::npos) {
          std::string name;
          while (k < inner.size() && isalnum(static_cast<unsigned char>(inner[k]))) {
            name += static_cast<char>(tolower(static_cast<unsigned char>(inner[k++])));
          }
          if (name == "br") out += '\n';
          i = close + 1;
          continue;
        }
      }
    }
    if (c == '&') {  // &amp; &#8531; &#x2153;
      const size_t semi = s.find(';', i + 1);
      if (semi != std::string::npos && semi - i <= 10) {
        uint32_t cp = text::entityCodepoint(text::lower(s.substr(i + 1, semi - i - 1)));
        if (cp == 0xA0) cp = ' ';
        if (cp) {
          appendChar(out, cp, font);
          i = semi + 1;
          continue;
        }
      }
    }
    appendChar(out, text::nextCodepoint(s, i), font);
  }
}

// Leading spaces (a tab counts 4); start gets where the rest begins.
int indentOf(const std::string &line, size_t &start) {
  int n = 0;
  size_t i = 0;
  for (; i < line.size(); ++i) {
    if (line[i] == ' ') {
      ++n;
    } else if (line[i] == '\t') {
      n += 4;
    } else {
      break;
    }
  }
  start = i;
  return n;
}

std::string trim(const std::string &s) { return text::trim(s); }

// Three or more of one of - * _ =, with spaces between or not, and nothing else.
bool isRule(const std::string &s) {
  char mark = 0;
  int count = 0;
  for (const char c : s) {
    if (c == ' ') continue;
    if ((c != '-' && c != '*' && c != '_' && c != '=') || (mark && c != mark)) return false;
    mark = c;
    ++count;
  }
  return count >= 3;
}

// A table's |---|:--:| line, under its header row.
bool isTableRule(const std::string &s) {
  for (const char c : s) {
    if (c != '|' && c != '-' && c != ':' && c != ' ') return false;
  }
  return s.find('-') != std::string::npos;
}

// "| a | b |" -> "a · b".
std::string tableCells(const std::string &s) {
  std::string out, cell;
  auto endCell = [&] {
    const std::string t = trim(cell);
    cell.clear();
    if (t.empty()) return;
    if (!out.empty()) out += " \xC2\xB7 ";
    out += t;
  };
  for (size_t i = 1; i < s.size(); ++i) {
    if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == '|') {
      cell += '|';
      ++i;
    } else if (s[i] == '|') {
      endCell();
    } else {
      cell += s[i];
    }
  }
  endCell();
  return out;
}

// "- ", "* ", "+ " or "• ": the length of the mark and its space, else 0.
size_t bulletMark(const std::string &s) {
  if (s.size() >= 2 && (s[0] == '-' || s[0] == '*' || s[0] == '+') && s[1] == ' ') return 2;
  if (s.rfind("\xE2\x80\xA2 ", 0) == 0) return 4;
  return 0;
}

// "12. " or "12) ": the length of the mark and its space, with "12." in number; else 0.
size_t numberMark(const std::string &s, std::string &number) {
  size_t i = 0;
  while (i < s.size() && i < 3 && isdigit(static_cast<unsigned char>(s[i]))) ++i;
  if (i == 0 || i + 1 >= s.size() || (s[i] != '.' && s[i] != ')') || s[i + 1] != ' ') return 0;
  number = s.substr(0, i) + ".";
  return i + 2;
}

// Anything that draws: not only spaces, line breaks and style bytes.
bool hasVisible(const std::string &s, size_t from, size_t to) {
  for (size_t i = from; i < to; ++i) {
    if (s[i] != ' ' && s[i] != '\n' && !flow::isStyle(s[i])) return true;
  }
  return false;
}

std::string oneLine(std::string s) {
  for (char &c : s) {
    if (c == '\n' || c == '\r' || c == '\t') c = ' ';
  }
  return trim(s);
}

// Draws flow text src[from, to) with its baseline at y, a run of spaces as one; returns the x
// after it. faceFor(style) gives each glyph's font; tracking adds px after each.
template <typename FaceFor>
int drawFlow(GFXcanvas1 &canvas, int x, int baseline, const std::string &src, uint32_t from,
             uint32_t to, uint8_t style, int tracking, FaceFor faceFor) {
  bool space = false;
  for (uint32_t p = from; p < to; ++p) {
    const char c = src[p];
    if (flow::isStyle(c)) {
      style = c - flow::kStyleBase;
      continue;
    }
    if (c == ' ') {
      if (!space) x += gfx::advance(faceFor(style), ' ') + tracking;
      space = true;
      continue;
    }
    space = false;
    if (c == kNoBreak) {
      x += gfx::advance(faceFor(style), ' ') + tracking;
      continue;
    }
    x = gfx::drawChar(canvas, x, baseline, c, faceFor(style), gfx::kBlack) + tracking;
  }
  return x;
}

}  // namespace

bool NoteDoc::sameText(const std::string &a, const std::string &b) {
  auto squash = [](const std::string &s) {
    std::string out;
    for (const char c : s) {
      if (c == '*' || c == '_' || c == '#') continue;
      out += static_cast<char>(tolower(static_cast<unsigned char>(c)));
    }
    return trim(out);
  };
  return squash(a) == squash(b);
}

void NoteDoc::addBlock(Kind kind, uint8_t level, bool checked, const std::string &marker,
                       const std::string &raw) {
  Block b{kind, level, checked, static_cast<uint32_t>(text_.size()), 0, marker};
  if (kind == Kind::Rule || kind == Kind::Picture) {
    b.end = b.start;
    blocks_.push_back(std::move(b));
    return;
  }
  const GFXfont *font = kind == Kind::Heading ? kHeadingFont : fonts::kReader[0].face[0];
  if (kind == Kind::Code) {  // as it is, marks and all
    for (size_t i = 0; i < raw.size();) appendChar(text_, text::nextCodepoint(raw, i), font);
  } else {
    uint8_t style = 0;
    inlineFlow(text_, kind == Kind::Heading ? ui::upper(raw) : raw, font, style);
  }
  b.end = static_cast<uint32_t>(text_.size());
  // A degree sign stays with its number, and a × with both sides of it.
  for (uint32_t i = b.start; i < b.end; ++i) {
    if (text_[i] != ' ') continue;
    if ((i + 1 < b.end && (text_[i + 1] == kDegree || text_[i + 1] == kTimes)) ||
        (i > b.start && text_[i - 1] == kTimes)) {
      text_[i] = kNoBreak;
    }
  }
  if (!hasVisible(text_, b.start, b.end) || blocks_.size() >= UINT16_MAX) {
    text_.resize(b.start);
    return;
  }
  blocks_.push_back(std::move(b));
}

void NoteDoc::parse(const std::string &title, const std::string &markdown) {
  title_.clear();
  text_.clear();
  blocks_.clear();
  lines_.clear();
  pages_.clear();

  struct Raw {
    Kind kind;
    uint8_t level;
    bool checked;
    std::string marker;
    std::string text;
  };
  std::vector<Raw> raws;
  bool open = false;   // the last block takes the next line, as a line break in it
  bool fence = false;  // inside ``` ... ```
  size_t pos = 0;
  while (pos <= markdown.size()) {
    size_t eol = markdown.find('\n', pos);
    if (eol == std::string::npos) eol = markdown.size();
    std::string line = markdown.substr(pos, eol - pos);
    pos = eol + 1;
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
      line.pop_back();
    }
    size_t start;
    const int indent = indentOf(line, start);
    const std::string body = line.substr(start);
    const bool fenceMark = body.rfind("```", 0) == 0 || body.rfind("~~~", 0) == 0;
    if (fence) {
      if (fenceMark) {
        fence = false;
      } else {
        raws.push_back({Kind::Code, 0, false, "", body});
      }
      open = false;
      continue;
    }
    if (fenceMark) {
      fence = true;
      open = false;
      continue;
    }
    if (body.empty()) {
      open = false;
      continue;
    }
    // A picture, on its line alone: ![alt](inkimg:N).
    if (body.rfind("![", 0) == 0 && body.back() == ')') {
      const size_t at = body.find("](inkimg:");
      if (at != std::string::npos) {
        const std::string n = body.substr(at + 9, body.size() - at - 10);
        if (!n.empty() && n.find_first_not_of("0123456789") == std::string::npos) {
          raws.push_back({Kind::Picture, 0, false, n, ""});
          open = false;
          continue;
        }
      }
    }

    size_t hashes = 0;
    while (hashes < body.size() && body[hashes] == '#') ++hashes;
    if (hashes >= 1 && hashes <= 6 && (hashes == body.size() || body[hashes] == ' ')) {
      std::string h = trim(body.substr(hashes));
      while (!h.empty() && h.back() == '#') h.pop_back();  // "## Steps ##"
      raws.push_back({Kind::Heading, static_cast<uint8_t>(hashes), false, "", trim(h)});
      open = false;
      continue;
    }
    if (isRule(body)) {
      raws.push_back({Kind::Rule, 0, false, "", ""});
      open = false;
      continue;
    }
    if (body[0] == '>') {
      std::string rest = body.substr(1);
      if (!rest.empty() && rest[0] == ' ') rest.erase(0, 1);
      if (open && raws.back().kind == Kind::Quote) {
        raws.back().text += '\n' + rest;
      } else {
        raws.push_back({Kind::Quote, 0, false, "", rest});
      }
      open = true;
      continue;
    }
    if (body[0] == '|') {
      if (!isTableRule(body)) {
        raws.push_back({Kind::Row, 0, false, "", tableCells(body)});
      } else if (!raws.empty() && raws.back().kind == Kind::Row) {
        raws.back().text = "**" + raws.back().text + "**";  // the header row, over the rule
      }
      open = false;
      continue;
    }
    const uint8_t level = indent >= 2 ? 1 : 0;
    if (const size_t m = bulletMark(body)) {
      std::string rest = body.substr(m);
      const bool box = rest.size() >= 3 && rest[0] == '[' && rest[2] == ']' &&
                       (rest[1] == ' ' || rest[1] == 'x' || rest[1] == 'X') &&
                       (rest.size() == 3 || rest[3] == ' ');
      if (box) {
        raws.push_back({Kind::Check, level, rest[1] != ' ', "", trim(rest.substr(3))});
      } else {
        raws.push_back({Kind::Bullet, level, false, "", rest});
      }
      open = true;
      continue;
    }
    std::string number;
    if (const size_t m = numberMark(body, number)) {
      raws.push_back({Kind::Number, level, false, number, body.substr(m)});
      open = true;
      continue;
    }
    if (open && !raws.empty()) {
      raws.back().text += '\n' + body;
    } else {
      raws.push_back({Kind::Paragraph, 0, false, "", body});
      open = true;
    }
  }

  // The title: as given, or a first heading. A first heading that repeats it goes.
  std::string t = oneLine(title);
  size_t first = 0;
  if (!raws.empty() && raws[0].kind == Kind::Heading && (t.empty() || sameText(raws[0].text, t))) {
    if (t.empty()) t = raws[0].text;
    first = 1;
  }
  std::string flowTitle;
  uint8_t style = 0;
  inlineFlow(flowTitle, ui::upper(t), kTitleFont, style);
  for (const char c : flowTitle) {
    if (c == '\n') {
      title_ += ' ';
    } else if (!flow::isStyle(c)) {
      title_ += c;
    }
  }
  if (!hasVisible(title_, 0, title_.size())) title_.clear();

  for (size_t i = first; i < raws.size(); ++i) {
    addBlock(raws[i].kind, raws[i].level, raws[i].checked, raws[i].marker, raws[i].text);
  }
}

void NoteDoc::setByline(const std::string &left, const std::string &right) {
  bylineLeft_ = left;
  bylineRight_ = right;
}

template <typename FaceFor>
void NoteDoc::wrap(const std::string &src, uint32_t from, uint32_t to, int width, int tracking,
                   FaceFor faceFor, std::vector<Span> &out) {
  out.clear();
  auto glyphW = [&](char c, uint8_t s) { return glyphAdvance(faceFor(s), c) + tracking; };
  uint8_t style = 0;
  uint32_t pos = from;
  while (pos < to) {
    // Spaces and style bytes before the line's first word.
    while (pos < to && (src[pos] == ' ' || flow::isStyle(src[pos]))) {
      if (flow::isStyle(src[pos])) style = src[pos] - flow::kStyleBase;
      ++pos;
    }
    if (pos >= to) break;
    if (src[pos] == '\n') {
      ++pos;
      continue;
    }
    Span line{pos, pos, style};
    int w = 0;
    bool any = false;
    while (pos < to && src[pos] != '\n') {
      // The next word, and the style after it.
      uint32_t end = pos;
      uint8_t s = style;
      int ww = 0;
      while (end < to && src[end] != ' ' && src[end] != '\n') {
        const char c = src[end++];
        if (flow::isStyle(c)) {
          s = c - flow::kStyleBase;
        } else {
          ww += glyphW(c, s);
        }
      }
      const int space = any ? glyphW(' ', style) : 0;
      if (w + space + ww <= width) {
        w += space + ww;
        style = s;
        pos = line.end = end;
        any = true;
      } else if (!any) {
        // Too long for a line of its own (an address): as much of it as fits, after its last
        // hyphen or slash in there if it has one, else anywhere; at least a glyph.
        uint32_t cut = pos, afterDash = 0;
        uint8_t dashStyle = style;
        int cw = 0;
        bool one = false;
        while (cut < end) {
          const char c = src[cut];
          if (flow::isStyle(c)) {
            style = c - flow::kStyleBase;
            ++cut;
            continue;
          }
          const int gw = glyphW(c, style);
          if (one && cw + gw > width) break;
          cw += gw;
          ++cut;
          one = true;
          if ((c == '-' || c == '/') && cut < end) {
            afterDash = cut;
            dashStyle = style;
          }
        }
        if (afterDash && cut < end) {
          cut = afterDash;
          style = dashStyle;
        }
        pos = line.end = cut;
        break;
      } else {
        break;
      }
      while (pos < to && src[pos] == ' ') ++pos;
    }
    out.push_back(line);
  }
}

void NoteDoc::layout(const FontFamily &body, int width, int height) {
  body_ = &body;
  width_ = width;
  height_ = height;
  lines_.clear();
  pages_.assign(1, 0);
  const GFXfont *regular = body.face[0];
  const int lh = body.lineHeight ? body.lineHeight : regular->yAdvance;
  markerGap_ = gfx::advance(regular, ' ') * 3 / 2;
  markerW_ = gfx::textWidth(body.face[flow::kBold], "00.") + markerGap_;

  int y = 0;
  auto newPage = [&] {
    pages_.push_back(static_cast<uint32_t>(lines_.size()));
    y = 0;
  };
  // Puts a line h px tall gap px under the last, or at the top of the next page (without the
  // gap) if it doesn't fit.
  auto place = [&](Line line, int gap, int h) {
    if (y > 0 && y + gap + h > height_) newPage();
    if (y == 0) gap = 0;
    line.y = static_cast<int16_t>(y + gap);
    lines_.push_back(line);
    y += gap + h;
  };
  std::vector<Span> spans;

  if (!title_.empty()) {
    wrap(title_, 0, static_cast<uint32_t>(title_.size()), width_, 0,
         [](uint8_t) { return kTitleFont; }, spans);
    const size_t n = std::min<size_t>(spans.size(), kTitleMaxLines);
    for (size_t i = 0; i < n; ++i) {
      place({spans[i].start, spans[i].end, 0, 0, 0, 0, LineKind::Title, i == 0,
             i + 1 == n && spans.size() > n},
            0, kTitleLineH);
    }
    place({0, 0, 0, 0, 0, 0, LineKind::TitleRule, false, false}, kTitleRuleGap, kTitleRuleH);
  }
  const bool byline = !bylineLeft_.empty() || !bylineRight_.empty();
  if (byline) place({0, 0, 0, 0, 0, 0, LineKind::Byline, false, false}, kBylineGap, kBylineH);

  const FontFamily *family = body_;
  auto bodyFace = [family](uint8_t s) { return family->face[s & 3]; };
  Kind prev = Kind::Paragraph;
  for (size_t i = 0; i < blocks_.size(); ++i) {
    const Block &b = blocks_[i];
    int gap = lh / 2;
    if (i == 0) {
      gap = byline ? kAfterByline : title_.empty() ? 0 : kAfterTitle;
    } else if (prev == Kind::Heading) {
      gap = lh / 4;
    } else if (b.kind == Kind::Heading) {
      gap = lh;
    } else if (isItem(b.kind) && isItem(prev)) {
      gap = lh / 4;
    } else if ((b.kind == Kind::Row || b.kind == Kind::Code) && b.kind == prev) {
      gap = 0;
    }
    prev = b.kind;
    const uint16_t index = static_cast<uint16_t>(i);

    if (b.kind == Kind::Rule) {
      place({b.start, b.start, 0, 0, index, 0, LineKind::Rule, true, false}, gap, lh / 2);
      continue;
    }
    if (b.kind == Kind::Picture) {
      int w = 0, h = 0;
      if (!pictureFor(b, w, h)) continue;  // it didn't come: nothing
      place({b.start, b.start, static_cast<int16_t>((width_ - w) / 2), 0, index, 0,
             LineKind::Picture, true, false},
            gap, h);
      continue;
    }
    if (b.kind == Kind::Heading) {
      wrap(text_, b.start, b.end, width_, kHeadingTracking, [](uint8_t) { return kHeadingFont; },
           spans);
      // With the first line of what follows it, or at the top of the next page.
      const int need = gap + static_cast<int>(spans.size()) * lh + lh / 4 + lh;
      if (y > 0 && y + need > height_) newPage();
      for (size_t k = 0; k < spans.size(); ++k) {
        place({spans[k].start, spans[k].end, 0, 0, index, 0, LineKind::Heading, k == 0, false},
              k == 0 ? gap : 0, lh);
      }
      continue;
    }
    int x = 0;
    if (isItem(b.kind)) {
      x = markerW_ * (1 + b.level);
    } else if (b.kind == Kind::Quote) {
      x = kQuoteIndent;
    }
    wrap(text_, b.start, b.end, width_ - x, 0, bodyFace, spans);
    for (size_t k = 0; k < spans.size(); ++k) {
      place({spans[k].start, spans[k].end, static_cast<int16_t>(x), 0, index, spans[k].style,
             LineKind::Text, k == 0, false},
            k == 0 ? gap : 0, lh);
    }
  }
}

const article::Picture *NoteDoc::pictureFor(const Block &b, int &w, int &h) const {
  const uint32_t index = static_cast<uint32_t>(atoi(b.marker.c_str()));
  for (const article::Picture &p : pictures_) {
    if (p.index != index || !p.width || !p.height) continue;
    if (p.bits.size() < static_cast<size_t>((p.width + 7) / 8) * p.height) return nullptr;
    const float scale = std::min({1.0f, static_cast<float>(width_) / p.width,
                                  static_cast<float>(height_) / p.height});
    w = std::max(1, static_cast<int>(p.width * scale));
    h = std::max(1, static_cast<int>(p.height * scale));
    return &p;
  }
  return nullptr;
}

void NoteDoc::drawMarker(GFXcanvas1 &canvas, const Block &b, int right, int baseline) const {
  const GFXfont *font = body_->face[0];
  std::string mark;
  if (b.kind == Kind::Number) {
    mark = b.marker;
    font = body_->face[flow::kBold];
  } else if (b.kind == Kind::Check) {
    mark = static_cast<char>(b.checked ? kCheckedSlot : kUncheckedSlot);
  } else {
    mark = static_cast<char>(b.level ? kDashSlot : kBulletSlot);
  }
  gfx::drawText(canvas, right - gfx::textWidth(font, mark), baseline, mark, font);
}

void NoteDoc::drawPage(GFXcanvas1 &canvas, int page, int x, int y) const {
  if (!body_ || lines_.empty()) return;
  page = std::max(0, std::min(page, pageCount() - 1));
  const size_t from = pages_[page];
  const size_t to = page + 1 < static_cast<int>(pages_.size()) ? pages_[page + 1] : lines_.size();
  const GFXfont *regular = body_->face[0];
  const int lh = body_->lineHeight ? body_->lineHeight : regular->yAdvance;
  // Capitals centred in their line, as the Reader's typesetter does.
  const int bodyBase = (lh + capHeight(regular)) / 2;
  const int headBase = (lh + capHeight(kHeadingFont)) / 2;
  const int titleBase = (kTitleLineH + capHeight(kTitleFont)) / 2;
  const FontFamily *family = body_;
  auto bodyFace = [family](uint8_t s) { return family->face[s & 3]; };
  auto titleFace = [](uint8_t) { return kTitleFont; };
  auto headFace = [](uint8_t) { return kHeadingFont; };

  for (size_t i = from; i < to; ++i) {
    const Line &line = lines_[i];
    const int top = y + line.y;
    switch (line.kind) {
      case LineKind::Title: {
        const int end =
            drawFlow(canvas, x, top + titleBase, title_, line.start, line.end, 0, 0, titleFace);
        if (line.more) {
          gfx::drawChar(canvas, end, top + titleBase, static_cast<char>(kEllipsisSlot), kTitleFont,
                        gfx::kBlack);
        }
        break;
      }
      case LineKind::TitleRule:
        canvas.fillRect(x, top, width_, kTitleRuleH, gfx::kBlack);
        break;
      case LineKind::Byline: {
        const int base = ui::baselineCentered(kBylineStyle, top, kBylineH);
        const int rightW = bylineRight_.empty() ? 0 : ui::textWidth(kBylineStyle, bylineRight_);
        if (rightW) ui::drawText(canvas, x + width_ - rightW, base, bylineRight_, kBylineStyle);
        const int leftW = width_ - (rightW ? rightW + 16 : 0);
        if (!bylineLeft_.empty() && leftW > 0) {
          ui::drawText(canvas, x, base, ui::ellipsize(kBylineStyle, bylineLeft_, leftW),
                       kBylineStyle);
        }
        break;
      }
      case LineKind::Rule:
        ui::dashedH(canvas, x, top + lh / 4, width_);
        break;
      case LineKind::Picture: {
        int w = 0, h = 0;
        const article::Picture *p = pictureFor(blocks_[line.block], w, h);
        if (!p) break;
        const int left = x + line.x;
        if (w == p->width && h == p->height) {
          canvas.drawBitmap(left, top, p->bits.data(), p->width, p->height, gfx::kBlack);
          break;
        }
        // Scaled down: the nearest pixel.
        const int row = (p->width + 7) / 8;
        for (int dy = 0; dy < h; ++dy) {
          const uint8_t *src = p->bits.data() + static_cast<size_t>(dy * p->height / h) * row;
          for (int dx = 0; dx < w; ++dx) {
            const int sx = dx * p->width / w;
            if (src[sx / 8] & (0x80 >> (sx % 8))) canvas.drawPixel(left + dx, top + dy, gfx::kBlack);
          }
        }
        break;
      }
      case LineKind::Heading: {
        const int end = drawFlow(canvas, x, top + headBase, text_, line.start, line.end, 0,
                                 kHeadingTracking, headFace);
        // A dashed rule to the right edge after the heading's last line.
        const bool last = i + 1 >= lines_.size() || lines_[i + 1].block != line.block ||
                          lines_[i + 1].kind != LineKind::Heading;
        const int dashFrom = end + kHeadingDashGap;
        if (last && dashFrom < x + width_) {
          ui::dashedH(canvas, dashFrom, top + headBase - capHeight(kHeadingFont) / 2,
                      x + width_ - dashFrom);
        }
        break;
      }
      case LineKind::Text: {
        const Block &b = blocks_[line.block];
        drawFlow(canvas, x + line.x, top + bodyBase, text_, line.start, line.end, line.style, 0,
                 bodyFace);
        if (line.first && isItem(b.kind)) {
          drawMarker(canvas, b, x + line.x - markerGap_, top + bodyBase);
        }
        if (b.kind == Kind::Quote) canvas.fillRect(x, top, kQuoteBarW, lh, gfx::kBlack);
        break;
      }
    }
  }
}

uint32_t NoteDoc::pageOffset(int page) const {
  if (page <= 0 || page >= static_cast<int>(pages_.size())) return 0;
  for (size_t i = pages_[page]; i < lines_.size(); ++i) {
    if (lines_[i].kind != LineKind::Title && lines_[i].kind != LineKind::TitleRule &&
        lines_[i].kind != LineKind::Byline) {
      return lines_[i].start;
    }
  }
  return 0;
}

int NoteDoc::pageAt(uint32_t offset) const {
  int best = 0;
  for (int p = 1; p < static_cast<int>(pages_.size()); ++p) {
    if (pageOffset(p) <= offset) best = p;
  }
  return best;
}
