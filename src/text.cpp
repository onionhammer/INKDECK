#include "text.h"

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
  if (cp == 0xAD) return;           // soft hyphen: no hyphenation yet
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

}  // namespace text
