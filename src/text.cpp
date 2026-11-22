#include "text.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>

#include "ui/fonts/glyphs.h"

namespace text {
namespace {

// U+00C0..U+00FF and U+0100..U+017F, one ASCII letter each. Ligatures (Æ, ß, Þ, Œ, ...) are
// special-cased in appendAscii before these tables are consulted.
constexpr char kLatin1[] =
    "AAAAAAACEEEEIIII"
    "DNOOOOOxOUUUUYTs"
    "aaaaaaaceeeeiiii"
    "dnooooo/ouuuuyty";
constexpr char kLatinExtA[] =
    "AaAaAaCcCcCcCcDd"
    "DdEeEeEeEeEeGgGg"
    "GgGgHhHhIiIiIiIi"
    "IiIjJjKkkLlLlLlL"
    "lLlNnNnNnnNnOoOo"
    "OoOoRrRrRrSsSsSs"
    "SsTtTtTtUuUuUuUu"
    "UuUuWwYyYZzZzZzs";

}  // namespace

uint32_t nextCodepoint(const std::string &s, size_t &i) {
  const auto b0 = static_cast<uint8_t>(s[i++]);
  if (b0 < 0x80) return b0;
  int extra;
  uint32_t cp;
  if ((b0 & 0xE0) == 0xC0) {
    extra = 1;
    cp = b0 & 0x1F;
  } else if ((b0 & 0xF0) == 0xE0) {
    extra = 2;
    cp = b0 & 0x0F;
  } else if ((b0 & 0xF8) == 0xF0) {
    extra = 3;
    cp = b0 & 0x07;
  } else {
    return 0xFFFD;
  }
  while (extra-- > 0) {
    if (i >= s.size() || (static_cast<uint8_t>(s[i]) & 0xC0) != 0x80) return 0xFFFD;
    cp = (cp << 6) | (static_cast<uint8_t>(s[i++]) & 0x3F);
  }
  return cp;
}

// What terminal programs draw with (a herdr agent's screen, apps/agent.h): box-drawing lines,
// blocks, prompt arrows and spinner stars, as the nearest ASCII. 0 for anything else.
static char terminalAscii(uint32_t cp) {
  if (cp >= 0x2500 && cp <= 0x257F) {  // box drawing
    switch (cp) {
      case 0x2500: case 0x2501: case 0x2504: case 0x2505: case 0x2508: case 0x2509:
      case 0x254C: case 0x254D: case 0x2550: case 0x2574: case 0x2576: case 0x2578:
      case 0x257A: case 0x257C: case 0x257E:
        return '-';
      case 0x2502: case 0x2503: case 0x2506: case 0x2507: case 0x250A: case 0x250B:
      case 0x254E: case 0x254F: case 0x2551: case 0x2575: case 0x2577: case 0x2579:
      case 0x257B: case 0x257D: case 0x257F:
        return '|';
      default: return '+';  // corners and junctions
    }
  }
  if (cp >= 0x2580 && cp <= 0x259F) return '#';  // block elements
  switch (cp) {
    case 0x276F: case 0x23F5: case 0x25B8: case 0x25BA: case 0x2192: return '>';
    case 0x276E: case 0x23F4: case 0x25C2: case 0x25C4: case 0x2190: return '<';
    case 0x23BF: case 0x2514: return '|';
    case 0x23FA: case 0x2722: case 0x2733: case 0x2736: case 0x273B: case 0x273D:
    case 0x203B:
      return '*';
    default: return 0;
  }
}

void appendAscii(std::string &out, uint32_t cp) {
  if (cp >= 0x20 && cp < 0x7F) {
    out += static_cast<char>(cp);
    return;
  }
  switch (cp) {
    case 0x09: case 0x0A: case 0x0D: case 0xA0:
    case 0x2002: case 0x2003: case 0x2009: case 0x200A: case 0x202F:
      out += ' ';
      return;
    case 0xAD: case 0x200B: case 0x200C: case 0x200D: case 0x2060: case 0xFEFF:
      return;  // soft hyphen, zero-width chars, BOM
    case 0x2018: case 0x2019: case 0x201A: case 0x201B: case 0x2032: case 0x02BC:
    case 0x2039: case 0x203A: case 0xB4:
      out += '\'';
      return;
    case 0x201C: case 0x201D: case 0x201E: case 0x201F: case 0x2033: case 0xAB: case 0xBB:
      out += '"';
      return;
    case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2212: case 0xAC:
      out += '-';
      return;
    case 0x2014: case 0x2015:
      out += "--";
      return;
    case 0x2026: out += "..."; return;
    case 0x2022: case 0x2023: case 0x25CF: out += '*'; return;
    case 0xB7: case 0x2027: out += '.'; return;
    case 0x2020: out += '+'; return;
    case 0x2021: out += "++"; return;
    case 0x2044: out += '/'; return;
    case 0xA9: out += "(c)"; return;
    case 0xAE: out += "(R)"; return;
    case 0x2122: out += "TM"; return;
    case 0x20AC: out += "EUR"; return;
    case 0xA3: out += "GBP"; return;
    case 0xA2: out += 'c'; return;
    case 0xA5: out += "JPY"; return;
    case 0xA7: out += 'S'; return;
    case 0xB6: out += 'P'; return;
    case 0xB0: case 0xBA: out += 'o'; return;
    case 0xAA: out += 'a'; return;
    case 0xB1: out += "+/-"; return;
    case 0xB2: out += '2'; return;
    case 0xB3: out += '3'; return;
    case 0xB9: out += '1'; return;
    case 0xB5: out += 'u'; return;
    case 0xBC: out += "1/4"; return;
    case 0xBD: out += "1/2"; return;
    case 0xBE: out += "3/4"; return;
    case 0xA1: out += '!'; return;
    case 0xBF: out += '?'; return;
    case 0xC6: out += "AE"; return;
    case 0xE6: out += "ae"; return;
    case 0xDE: out += "Th"; return;
    case 0xFE: out += "th"; return;
    case 0xDF: out += "ss"; return;
    case 0x152: out += "OE"; return;
    case 0x153: out += "oe"; return;
    case 0x132: out += "IJ"; return;
    case 0x133: out += "ij"; return;
    case 0xFB00: out += "ff"; return;
    case 0xFB01: out += "fi"; return;
    case 0xFB02: out += "fl"; return;
    case 0xFB03: out += "ffi"; return;
    case 0xFB04: out += "ffl"; return;
    default: break;
  }
  if (cp >= 0xC0 && cp <= 0xFF) {
    out += kLatin1[cp - 0xC0];
  } else if (cp >= 0x100 && cp <= 0x17F) {
    out += kLatinExtA[cp - 0x100];
  } else if (const char c = terminalAscii(cp)) {
    out += c;
  } else if (cp >= 0x20) {
    out += '?';
  }
  // other C0 control characters are dropped
}

void appendUtf8(std::string &out, uint32_t cp) {
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

void appendSlots(std::string &out, uint32_t cp) {
  if (cp == 0xAD) return;           // soft hyphen (flow text gets flow::kSoftHyphen instead)
  if (cp == 0x2015) cp = 0x2014;    // horizontal bar: draw an em dash
  if (const uint8_t slot = ui::fonts::slotFor(cp)) {
    out += static_cast<char>(slot);
  } else {
    appendAscii(out, cp);  // ' ' for other spaces, "" for zero-width characters
  }
}

std::string toAscii(const std::string &utf8) {
  std::string out;
  out.reserve(utf8.size());
  for (size_t i = 0; i < utf8.size();) appendAscii(out, nextCodepoint(utf8, i));
  return out;
}

uint32_t countWords(const std::string &markdown) {
  uint32_t n = 0;
  bool in = false;
  for (size_t i = 0; i < markdown.size(); ++i) {
    if (markdown[i] == ']' && i + 1 < markdown.size() && markdown[i + 1] == '(') {
      const size_t close = markdown.find(')', i);
      if (close != std::string::npos) i = close;
      continue;
    }
    const char c = markdown[i];
    const bool space = c == ' ' || c == '\n' || c == '\t';
    if (!space && !in) ++n;
    in = !space;
  }
  return n;
}

std::string lower(std::string s) {
  for (char &c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string trim(const std::string &s, const char *chars) {
  const size_t first = s.find_first_not_of(chars);
  if (first == std::string::npos) return {};
  return s.substr(first, s.find_last_not_of(chars) - first + 1);
}

uint32_t fnv1a(const std::string &s) {
  uint32_t h = 2166136261u;
  for (const char c : s) h = (h ^ static_cast<uint8_t>(c)) * 16777619u;
  return h;
}

std::string macText(const uint8_t mac[6]) {
  char buf[18];
  snprintf(buf, sizeof buf, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3],
           mac[4], mac[5]);
  return buf;
}

uint32_t entityCodepoint(const std::string &name) {
  if (name.size() > 1 && name[0] == '#') {
    const bool hex = name[1] == 'x' || name[1] == 'X';
    return static_cast<uint32_t>(strtoul(name.c_str() + (hex ? 2 : 1), nullptr, hex ? 16 : 10));
  }
  static const struct {
    const char *name;
    uint32_t cp;
  } kNamed[] = {
      {"amp", '&'},       {"lt", '<'},         {"gt", '>'},         {"quot", '"'},
      {"apos", '\''},     {"nbsp", 0xA0},      {"shy", 0xAD},       {"mdash", 0x2014},
      {"ndash", 0x2013},  {"hellip", 0x2026},  {"lsquo", 0x2018},   {"rsquo", 0x2019},
      {"ldquo", 0x201C},  {"rdquo", 0x201D},   {"laquo", 0xAB},     {"raquo", 0xBB},
      {"copy", 0xA9},     {"reg", 0xAE},       {"trade", 0x2122},   {"bull", 0x2022},
      {"middot", 0xB7},   {"deg", 0xB0},       {"times", 0xD7},     {"eacute", 0xE9},
      {"egrave", 0xE8},   {"agrave", 0xE0},    {"ccedil", 0xE7},    {"uuml", 0xFC},
      {"ouml", 0xF6},     {"auml", 0xE4},      {"szlig", 0xDF},     {"thinsp", 0x2009},
      {"ensp", 0x2002},   {"emsp", 0x2003},    {"zwnj", 0x200C},    {"zwj", 0x200D},
      {"euro", 0x20AC},   {"pound", 0xA3},
  };
  for (const auto &e : kNamed) {
    if (name == e.name) return e.cp;
  }
  return 0;
}

std::string percentDecode(const std::string &s) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size() && isxdigit(static_cast<unsigned char>(s[i + 1])) &&
        isxdigit(static_cast<unsigned char>(s[i + 2]))) {
      out += static_cast<char>(strtoul(s.substr(i + 1, 2).c_str(), nullptr, 16));
      i += 2;
    } else {
      out += s[i];
    }
  }
  return out;
}

}  // namespace text
