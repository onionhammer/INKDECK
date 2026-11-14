#include "textdoc.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <stdio.h>
#include <strings.h>

#include <algorithm>

#include "text.h"
#include "typeset.h"

namespace {

constexpr size_t kSniffBytes = 4096;
constexpr int kTabStop = 4;
constexpr uint16_t kMaxLineGlyphs = 4096;  // a line of zero-width glyphs still ends somewhere
constexpr size_t kChunk = 32 * 1024;
constexpr size_t kHeadroom = 1536 * 1024;  // PSRAM left for the line table and everything else

// Windows-1252 at 0x80-0x9F (0: no character there).
constexpr uint16_t kCp1252[32] = {
    0x20AC, 0,      0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
    0x2039, 0x0152, 0,      0x017D, 0,      0,      0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
    0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0,      0x017E, 0x0178};

// Bytes at the end of a chunk that begin a character the next chunk finishes.
size_t incompleteTail(const std::string &buf, bool utf16, bool le) {
  const size_t n = buf.size();
  if (utf16) {
    size_t tail = n % 2;
    if (n - tail >= 2) {  // a high surrogate waits for its partner
      const auto a = static_cast<uint8_t>(buf[n - tail - 2]);
      const auto b = static_cast<uint8_t>(buf[n - tail - 1]);
      const uint32_t u = le ? (b << 8 | a) : (a << 8 | b);
      if (u >= 0xD800 && u < 0xDC00) tail += 2;
    }
    return tail;
  }
  for (size_t back = 1; back <= 3 && back <= n; ++back) {  // UTF-8 (harmless for CP1252)
    const auto c = static_cast<uint8_t>(buf[n - back]);
    if ((c & 0xC0) == 0x80) continue;  // a continuation byte
    const size_t need = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
    return need > back ? back : 0;
  }
  return 0;
}

bool utf16Bom(const std::string &s) {
  return s.size() >= 2 &&
         ((s[0] == '\xFF' && s[1] == '\xFE') || (s[0] == '\xFE' && s[1] == '\xFF'));
}

// Decoded characters to flow text: line breaks and tabs by hand, the rest through the fonts'
// glyph slots (text::appendSlots). Stops (full) before out would outgrow its capacity.
struct Emitter {
  std::string &out;
  int column = 0;   // glyphs since the last line break, for tab stops
  bool cr = false;  // the last character was '\r': a '\n' after it is the same break
  bool full = false;

  void put(uint32_t cp) {
    if (out.size() + 8 > out.capacity()) {  // a character adds at most 4 (a tab, "...")
      full = true;
      return;
    }
    const bool lf = cp == '\n';
    if (lf && cr) {
      cr = false;
      return;
    }
    cr = cp == '\r';
    if (lf || cr || cp == 0x0B || cp == 0x0C || cp == 0x85 || cp == 0x2028 || cp == 0x2029) {
      out += '\n';
      column = 0;
    } else if (cp == '\t') {
      do {
        out += ' ';
      } while (++column % kTabStop);
    } else if (cp >= 0x20 && cp != 0x7F && (cp < 0x80 || cp >= 0xA0)) {
      const size_t before = out.size();
      text::appendSlots(out, cp);
      column += static_cast<int>(out.size() - before);
    }  // other control characters: dropped
  }
};

// Mostly valid UTF-8 (a stray bad byte is forgiven; a cut-off last character too).
bool isUtf8(const std::string &s) {
  size_t bad = 0, multi = 0;
  for (size_t i = 0; i < s.size();) {
    const size_t at = i;
    const uint32_t cp = text::nextCodepoint(s, i);
    if (i - at > 1) ++multi;
    if (cp == 0xFFFD && !(i - at == 3 && s.compare(at, 3, "\xEF\xBF\xBD") == 0)) {
      if (i >= s.size() && s.size() - at < 4) break;  // the 4 MB cut landed mid-character
      ++bad;
    }
  }
  return bad == 0 || bad * 100 <= multi;
}

std::string extensionOf(const std::string &path) {
  const std::string name = path.substr(path.rfind('/') + 1);
  const size_t dot = name.rfind('.');
  return dot == std::string::npos || dot == 0 ? std::string() : name.substr(dot + 1);
}

// [begin, end) of each line of s.
template <typename Fn>
void forEachLine(const std::string &s, Fn fn) {
  for (size_t i = 0; i <= s.size();) {
    size_t e = s.find('\n', i);
    if (e == std::string::npos) e = s.size();
    fn(i, e);
    i = e + 1;
  }
}

bool blank(const std::string &s, size_t b, size_t e) {
  for (size_t i = b; i < e; ++i) {
    if (s[i] != ' ') return false;
  }
  return true;
}

bool startsWith(const std::string &s, size_t b, size_t e, const char *prefix) {
  const size_t n = strlen(prefix);
  return e - b >= n && s.compare(b, n, prefix) == 0;
}

// Markdown lines that stand on their own: headings, table rows, code fences.
bool ownLine(const std::string &s, size_t b, size_t e) {
  return startsWith(s, b, e, "#") || startsWith(s, b, e, "|") || startsWith(s, b, e, "```");
}

// A line that starts a block: one of those, a list item or a quote.
bool startsBlock(const std::string &s, size_t b, size_t e) {
  if (ownLine(s, b, e)) return true;
  for (const char *p : {"- ", "* ", "+ ", "> "}) {
    if (startsWith(s, b, e, p)) return true;
  }
  size_t i = b;
  while (i < e && i - b < 3 && s[i] >= '0' && s[i] <= '9') ++i;  // "1. " or "12) "
  return i > b && i + 1 < e && (s[i] == '.' || s[i] == ')') && s[i + 1] == ' ';
}

}  // namespace

bool TextDoc::proseName(const std::string &path) {
  const std::string ext = extensionOf(path);
  for (const char *e : {"", "txt", "text", "md", "markdown"}) {
    if (strcasecmp(ext.c_str(), e) == 0) return true;
  }
  return false;
}

bool TextDoc::looksLikeText(const std::string &path) {
  FILE *f = fopen(path.c_str(), "rb");
  if (!f) return false;
  std::string head(kSniffBytes, '\0');
  head.resize(fread(&head[0], 1, head.size(), f));
  fclose(f);
  if (utf16Bom(head)) return true;
  size_t controls = 0;
  for (const char ch : head) {
    const auto c = static_cast<uint8_t>(ch);
    if (c == 0) return false;
    if (c < 0x20 && c != '\t' && c != '\n' && c != '\r' && c != 0x0B && c != 0x0C && c != 0x1B) {
      ++controls;
    }
  }
  return controls * 100 <= head.size();
}

bool TextDoc::open(const std::string &path, std::string &err) {
  text_ = std::string();
  lines_ = std::vector<Line>();
  if (!looksLikeText(path)) {
    err = "not a text file";
    return false;
  }
  FILE *f = fopen(path.c_str(), "rb");
  if (!f) {
    err = "can't read the file";
    return false;
  }
  fseek(f, 0, SEEK_END);
  const long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  fileBytes_ = size > 0 ? static_cast<uint64_t>(size) : 0;
  // Room for the text: the file and a quarter (tabs widen), within kMaxBytes and what PSRAM
  // has left. The text stops where the room ends, so the string never grows.
  const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  size_t room = static_cast<size_t>(std::min<uint64_t>(fileBytes_ + fileBytes_ / 4 + 64,
                                                       kMaxBytes));
  room = std::min(room, largest > kHeadroom ? largest - kHeadroom : 0);
  if (fileBytes_ > 0 && room < std::min<uint64_t>(fileBytes_ + 64, 4096)) {  // not even a page
    fclose(f);
    err = "not enough memory";
    return false;
  }
  const uint32_t t0 = millis();
  text_.reserve(room);
  const bool ok = decode(f);
  fclose(f);
  if (!ok) {
    text_ = std::string();
    err = "can't read the file";
    return false;
  }
  reflow(path);
  log_i("textdoc: %s: %llu of %llu bytes -> %u glyphs, %s%s in %lu ms", path.c_str(),
        static_cast<unsigned long long>(bytesUsed_), static_cast<unsigned long long>(fileBytes_),
        static_cast<unsigned>(text_.size()),
        encoding_ == Encoding::Utf8 ? "UTF-8" : encoding_ == Encoding::Utf16 ? "UTF-16" : "CP1252",
        reflowed_ ? ", reflowed" : "", static_cast<unsigned long>(millis() - t0));
  return true;
}

// Reads the file a chunk at a time, so it's never in memory twice. A character cut by the end
// of a chunk waits for the next one. The encoding is decided from the first chunk.
bool TextDoc::decode(FILE *f) {
  Emitter out{text_};
  std::string buf;
  bool first = true, le = false;
  size_t pos = 0;  // raw bytes consumed
  bytesUsed_ = 0;
  for (;;) {
    const size_t carry = buf.size();
    buf.resize(carry + kChunk);
    const size_t n = fread(&buf[carry], 1, kChunk, f);
    buf.resize(carry + n);
    if (n < kChunk && ferror(f)) return false;
    const bool eof = n < kChunk;
    size_t i = 0;
    if (first) {
      first = false;
      if (utf16Bom(buf)) {
        encoding_ = Encoding::Utf16;
        le = buf[0] == '\xFF';
        i = 2;
      } else {
        // A UTF-8 byte-order mark decodes to nothing (U+FEFF is zero-width).
        encoding_ = isUtf8(buf) ? Encoding::Utf8 : Encoding::Cp1252;
      }
    }
    const size_t end =
        eof ? buf.size() : buf.size() - incompleteTail(buf, encoding_ == Encoding::Utf16, le);
    auto unit = [&](size_t k) {
      const auto a = static_cast<uint8_t>(buf[k]), b = static_cast<uint8_t>(buf[k + 1]);
      return static_cast<uint32_t>(le ? (b << 8 | a) : (a << 8 | b));
    };
    while (i < end && !out.full) {
      const size_t at = i;
      uint32_t cp;
      if (encoding_ == Encoding::Utf8) {
        cp = text::nextCodepoint(buf, i);
      } else if (encoding_ == Encoding::Cp1252) {
        const auto c = static_cast<uint8_t>(buf[i++]);
        cp = c >= 0x80 && c < 0xA0 ? kCp1252[c - 0x80] : c;  // the rest is Latin-1
      } else {
        if (i + 1 >= end) break;  // an odd last byte
        cp = unit(i);
        i += 2;
        if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < end) {
          const uint32_t lo = unit(i);
          if (lo >= 0xDC00 && lo < 0xE000) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            i += 2;
          }
        }
      }
      out.put(cp);
      if (out.full) i = at;  // that character didn't fit
    }
    pos += i;
    if (out.full || eof) break;
    buf.erase(0, end);
  }
  bytesUsed_ = out.full ? pos : fileBytes_;
  while (!text_.empty() && (text_.back() == '\n' || text_.back() == ' ')) text_.pop_back();
  return true;
}

void TextDoc::reflow(const std::string &path) {
  reflowed_ = false;
  if (!proseName(path)) return;
  // Lengths of the non-blank lines, and how many spaces they hold (prose has words).
  std::vector<uint16_t> lengths;
  size_t spaces = 0;
  forEachLine(text_, [&](size_t b, size_t e) {
    if (blank(text_, b, e)) return;
    lengths.push_back(static_cast<uint16_t>(std::min<size_t>(e - b, UINT16_MAX)));
    spaces += std::count(text_.begin() + b, text_.begin() + e, ' ');
  });
  if (lengths.size() < 8 || spaces < lengths.size() * 3) return;
  std::vector<uint16_t> sorted = lengths;
  std::nth_element(sorted.begin(), sorted.begin() + sorted.size() * 95 / 100, sorted.end());
  const size_t wrapAt = sorted[sorted.size() * 95 / 100];
  if (wrapAt < 40 || wrapAt > 120) return;
  const size_t full = wrapAt * 3 / 4;  // a line this long was probably wrapped, not ended
  const size_t fullLines = std::count_if(lengths.begin(), lengths.end(),
                                         [&](uint16_t n) { return n >= full; });
  if (fullLines * 2 < lengths.size()) return;
  reflowed_ = true;

  // In place: a join writes one byte or none for the line break it removes, so the write
  // position w never passes the line being read. What the next decision needs about a line is
  // noted before the line moves.
  struct Info {
    size_t length;
    bool blank, ownLine, hardBreak, opensBlock, fence;
  };
  size_t w = 0;
  bool inFence = false, havePrev = false;
  Info prev{};
  forEachLine(text_, [&](size_t b, size_t e) {
    const Info cur{e - b,
                   blank(text_, b, e),
                   ownLine(text_, b, e),
                   e - b >= 2 && text_.compare(e - 2, 2, "  ") == 0,  // Markdown's hard break
                   text_[b] == ' ' || startsBlock(text_, b, e),
                   startsWith(text_, b, e, "```")};
    if (havePrev) {
      const bool join = !inFence && prev.length >= full && !prev.blank && !prev.ownLine &&
                        !prev.hardBreak && !cur.blank && !cur.opensBlock;
      if (!join) {
        text_[w++] = '\n';
      } else {
        while (w > 0 && text_[w - 1] == ' ') --w;
        // "well-" + "known": keep the hyphen, no space.
        const bool hyphen =
            w >= 2 && text_[w - 1] == '-' && isalpha(static_cast<uint8_t>(text_[w - 2]));
        if (!hyphen) text_[w++] = ' ';
      }
    }
    if (cur.fence) inFence = !inFence;
    memmove(&text_[w], &text_[b], e - b);
    w += e - b;
    prev = cur;
    havePrev = true;
  });
  text_.resize(w);
}

int TextDoc::width(uint32_t from, uint32_t to) const {
  int w = 0;
  for (uint32_t i = from; i < to; ++i) w += gfx::advance(font_, text_[i]);
  return w;
}

void TextDoc::layout(const GFXfont *font, int width) {
  font_ = font;
  width_ = width;
  lines_.clear();
  if (text_.empty()) return;
  forEachLine(text_, [&](size_t b, size_t e) {
    wrapParagraph(static_cast<uint32_t>(b), static_cast<uint32_t>(e));
  });
}

// Greedy: as many glyphs as fit, broken at the last space (dropped) or after the last hyphen
// that fits, or mid-word when there's neither.
void TextDoc::wrapParagraph(uint32_t begin, uint32_t end) {
  if (begin == end) {
    lines_.push_back({begin, 0, false});
    return;
  }
  uint32_t start = begin;
  while (start < end) {
    int w = 0;
    uint32_t space = 0, hyphen = 0;  // break candidates after start (0: none)
    uint32_t i = start;
    for (; i < end && i - start < kMaxLineGlyphs; ++i) {
      const char c = text_[i];
      const int a = gfx::advance(font_, c);
      if (w + a > width_ && i > start) break;
      w += a;
      if (c == ' ' && i > start) space = i;
      if (c == '-' && i + 1 < end && text_[i + 1] != ' ') hyphen = i + 1;
    }
    if (i >= end) {
      lines_.push_back({start, static_cast<uint16_t>(end - start), false});
      return;
    }
    if (text_[i] == ' ') {  // the overflow is a space: break there
      lines_.push_back({start, static_cast<uint16_t>(i - start), true});
      start = i + 1;
    } else if (space && space >= hyphen) {
      lines_.push_back({start, static_cast<uint16_t>(space - start), true});
      start = space + 1;
    } else if (hyphen) {
      lines_.push_back({start, static_cast<uint16_t>(hyphen - start), true});
      start = hyphen;
    } else {
      lines_.push_back({start, static_cast<uint16_t>(i - start), false});
      start = i;
    }
  }
}

int TextDoc::lineAt(uint32_t offset) const {
  const auto it = std::upper_bound(lines_.begin(), lines_.end(), offset,
                                   [](uint32_t o, const Line &l) { return o < l.start; });
  return it == lines_.begin() ? 0 : static_cast<int>(it - lines_.begin()) - 1;
}

void TextDoc::drawLine(GFXcanvas1 &canvas, int i, int x, int baseline, bool justify) const {
  const Line &l = lines_[i];
  uint32_t b = l.start, e = l.start + l.length;
  int extra = 0, gaps = 0;
  uint32_t first = b;  // leading spaces (an indent) keep their width
  if (justify && l.wraps) {
    while (e > b && text_[e - 1] == ' ') --e;
    while (first < e && text_[first] == ' ') ++first;
    gaps = static_cast<int>(std::count(text_.begin() + first, text_.begin() + e, ' '));
    extra = width_ - width(b, e);
    // The Reader's rule (Typesetter::drawLine): lines with a few gaps always justify, like a
    // printed page; a line of two or three words only within reason.
    if (gaps == 0 || extra <= 0 || (gaps < 3 && extra > gaps * 4 * gfx::advance(font_, ' '))) {
      extra = 0;
    }
  }
  int gap = 0;
  for (uint32_t k = b; k < e; ++k) {
    const char c = text_[k];
    if (c == ' ' && extra && k >= first) {
      x += gfx::advance(font_, ' ') + extra * (gap + 1) / gaps - extra * gap / gaps;
      ++gap;
    } else {
      x = gfx::drawChar(canvas, x, baseline, c, font_, gfx::kBlack);
    }
  }
}
