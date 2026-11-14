#include "rss.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>

#include "text.h"
#include "timeutil.h"

namespace rss {
namespace {

struct Span {
  const char *b = nullptr;
  const char *e = nullptr;
};

struct Element {
  Span tag;    // inside the start tag's < >: the name and attributes
  Span inner;  // the content
  const char *end = nullptr;  // just after the element
};

constexpr size_t kTitleMax = 300;
constexpr size_t kEpisodeTextMax = 420;
constexpr size_t kChannelTextMax = 600;

const char *find(const char *b, const char *e, const char *needle) {
  const size_t n = strlen(needle);
  const char *hit = std::search(b, e, needle, needle + n);
  return hit == e ? nullptr : hit;
}

bool startsWith(const char *p, const char *e, const char *s) {
  const size_t n = strlen(s);
  return static_cast<size_t>(e - p) >= n && memcmp(p, s, n) == 0;
}

bool nameEnds(char c) {
  return c == '>' || c == '/' || c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// If p starts a CDATA section or a comment, the position after it (e if unterminated);
// otherwise nullptr.
const char *skipOpaque(const char *p, const char *e) {
  if (startsWith(p, e, "<![CDATA[")) {
    const char *end = find(p + 9, e, "]]>");
    return end ? end + 3 : e;
  }
  if (startsWith(p, e, "<!--")) {
    const char *end = find(p + 4, e, "-->");
    return end ? end + 3 : e;
  }
  return nullptr;
}

// The next "<name" start tag in [p, e), outside CDATA and comments.
const char *findTag(const char *p, const char *e, const char *name) {
  const size_t n = strlen(name);
  while (p < e) {
    const char *lt = static_cast<const char *>(memchr(p, '<', e - p));
    if (!lt) return nullptr;
    if (const char *after = skipOpaque(lt, e)) {
      p = after;
      continue;
    }
    if (static_cast<size_t>(e - lt) > n + 1 && memcmp(lt + 1, name, n) == 0 &&
        nameEnds(lt[1 + n])) {
      return lt;
    }
    p = lt + 1;
  }
  return nullptr;
}

// The first complete element `name` in [p, e).
bool element(const char *p, const char *e, const char *name, Element &out) {
  const char *lt = findTag(p, e, name);
  if (!lt) return false;
  const char *gt = static_cast<const char *>(memchr(lt, '>', e - lt));
  if (!gt) return false;
  out.tag = {lt + 1, gt};
  if (gt[-1] == '/') {
    out.inner = {gt + 1, gt + 1};
    out.end = gt + 1;
    return true;
  }
  const std::string close = std::string("</") + name + ">";
  const char *q = gt + 1;
  while (q < e) {
    const char *lt2 = static_cast<const char *>(memchr(q, '<', e - q));
    if (!lt2) return false;
    if (const char *after = skipOpaque(lt2, e)) {
      q = after;
      continue;
    }
    if (startsWith(lt2, e, close.c_str())) {
      out.inner = {gt + 1, lt2};
      out.end = lt2 + close.size();
      return true;
    }
    q = lt2 + 1;
  }
  return false;  // cut off
}

struct Named {
  const char *name;
  uint32_t cp;
};
constexpr Named kEntities[] = {
    {"amp", '&'},      {"lt", '<'},        {"gt", '>'},        {"quot", '"'},
    {"apos", '\''},    {"nbsp", ' '},      {"rsquo", 0x2019},  {"lsquo", 0x2018},
    {"rdquo", 0x201D}, {"ldquo", 0x201C},  {"mdash", 0x2014},  {"ndash", 0x2013},
    {"hellip", 0x2026}, {"copy", 0xA9},    {"eacute", 0xE9},   {"uuml", 0xFC},
};

// Decodes the entity at p (p[0] == '&') into out and returns the position after it, or copies
// the '&' and returns p + 1 when it isn't one.
const char *entity(const char *p, const char *e, std::string &out) {
  const char *semi = static_cast<const char *>(memchr(p, ';', std::min<ptrdiff_t>(e - p, 12)));
  if (semi && semi - p > 1) {
    const std::string name(p + 1, semi);
    uint32_t cp = 0;
    if (name[0] == '#') {
      cp = name.size() > 1 && (name[1] == 'x' || name[1] == 'X')
               ? strtoul(name.c_str() + 2, nullptr, 16)
               : strtoul(name.c_str() + 1, nullptr, 10);
    } else {
      for (const Named &n : kEntities) {
        if (name == n.name) cp = n.cp;
      }
    }
    if (cp) {
      text::appendUtf8(out, cp == 0xA0 ? ' ' : cp);
      return semi + 1;
    }
  }
  out += '&';
  return p + 1;
}

std::string decodeEntities(const char *p, const char *e) {
  std::string out;
  out.reserve(e - p);
  while (p < e) {
    if (*p == '&') {
      p = entity(p, e, out);
    } else {
      out += *p++;
    }
  }
  return out;
}

// Collapses runs of whitespace to one space and trims the ends.
std::string collapse(const std::string &s) {
  std::string out;
  out.reserve(s.size());
  bool space = false;
  for (const char c : s) {
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      space = !out.empty();
    } else {
      if (space) out += ' ';
      space = false;
      out += c;
    }
  }
  return out;
}

// Cuts at maxBytes on a UTF-8 boundary, with an ellipsis.
std::string shorten(std::string s, size_t maxBytes) {
  if (s.size() <= maxBytes) return s;
  size_t cut = maxBytes;
  while (cut > 0 && (static_cast<uint8_t>(s[cut]) & 0xC0) == 0x80) --cut;
  s.resize(cut);
  while (!s.empty() && s.back() == ' ') s.pop_back();
  return s + "\xE2\x80\xA6";
}

// An element's text: CDATA sections as they are, the rest with entities decoded.
std::string text(const Span &inner) {
  std::string out;
  const char *p = inner.b;
  while (p < inner.e) {
    if (startsWith(p, inner.e, "<![CDATA[")) {
      const char *end = find(p + 9, inner.e, "]]>");
      const char *stop = end ? end : inner.e;
      out.append(p + 9, stop);
      p = end ? end + 3 : inner.e;
      continue;
    }
    const char *next = find(p, inner.e, "<![CDATA[");
    const char *stop = next ? next : inner.e;
    out += decodeEntities(p, stop);
    p = stop;
  }
  return out;
}

std::string elementText(const char *b, const char *e, const char *name) {
  Element el;
  return element(b, e, name, el) ? text(el.inner) : std::string();
}

// An attribute of a start tag, entities decoded.
std::string attribute(const Span &tag, const char *name) {
  const size_t n = strlen(name);
  for (const char *p = tag.b; p + n + 2 < tag.e; ++p) {
    if ((p[-1] == ' ' || p[-1] == '\t' || p[-1] == '\n' || p[-1] == '\r') &&
        memcmp(p, name, n) == 0 && p[n] == '=' && (p[n + 1] == '"' || p[n + 1] == '\'')) {
      const char quote = p[n + 1];
      const char *v = p + n + 2;
      const char *end = static_cast<const char *>(memchr(v, quote, tag.e - v));
      if (!end) return {};
      return decodeEntities(v, end);
    }
  }
  return {};
}

int monthIndex(const char *m) {
  static const char *const kMonths[] = {"jan", "feb", "mar", "apr", "may", "jun",
                                        "jul", "aug", "sep", "oct", "nov", "dec"};
  for (int i = 0; i < 12; ++i) {
    if (strncasecmp(m, kMonths[i], 3) == 0) return i + 1;
  }
  return 0;
}

// Minutes east of UTC for an RFC 822 zone.
int zoneMinutes(const char *z) {
  while (*z == ' ') ++z;
  if (*z == '+' || *z == '-') {
    const int v = atoi(z + 1);
    const int minutes = v / 100 * 60 + v % 100;
    return *z == '-' ? -minutes : minutes;
  }
  static const struct {
    const char *name;
    int hours;
  } kZones[] = {{"EST", -5}, {"EDT", -4}, {"CST", -6}, {"CDT", -5},
                {"MST", -7}, {"MDT", -6}, {"PST", -8}, {"PDT", -7}};
  for (const auto &zone : kZones) {
    if (strncasecmp(z, zone.name, 3) == 0) return zone.hours * 60;
  }
  return 0;  // GMT, UT, UTC, Z, or unknown
}

}  // namespace

time_t parseDate(const std::string &s) {
  const char *p = s.c_str();
  if (const char *comma = strchr(p, ',')) p = comma + 1;
  int day = 0, year = 0, hour = 0, minute = 0, second = 0, used = 0;
  char mon[4] = {};
  // siscanf: newlib's integer-only sscanf. The full one drags in float parsing (about 10 KB).
  if (siscanf(p, "%d %3s %d %d:%d%n", &day, mon, &year, &hour, &minute, &used) < 5) return 0;
  p += used;
  if (*p == ':') {
    int more = 0;
    siscanf(p + 1, "%d%n", &second, &more);
    p += 1 + more;
  }
  const int month = monthIndex(mon);
  if (!month || day < 1 || day > 31) return 0;
  if (year < 100) year += 2000;
  const long days = timeutil::daysFromCivil(year, month, day);
  return static_cast<time_t>(days) * 86400 + hour * 3600 + minute * 60 + second -
         zoneMinutes(p) * 60;
}

uint32_t parseDuration(const std::string &s) {
  uint32_t total = 0;
  int parts = 0;
  const char *p = s.c_str();
  while (*p == ' ') ++p;
  while (*p) {
    char *end = nullptr;
    const double v = strtod(p, &end);
    if (end == p) break;
    total = total * 60 + static_cast<uint32_t>(v);
    ++parts;
    p = end;
    if (*p != ':') break;
    ++p;
  }
  return parts ? total : 0;
}

std::string plainText(const std::string &html, size_t maxBytes) {
  std::string out;
  out.reserve(std::min(html.size(), maxBytes + 8));
  const char *p = html.data(), *e = p + html.size();
  while (p < e && out.size() < maxBytes * 2) {
    if (*p == '<') {
      const char *gt = static_cast<const char *>(memchr(p, '>', e - p));
      if (!gt) break;
      out += ' ';  // <p>, <br>, </li>: words on either side stay apart
      p = gt + 1;
    } else if (*p == '&') {
      p = entity(p, e, out);
    } else {
      out += *p++;
    }
  }
  return shorten(collapse(out), maxBytes);
}

bool parse(const char *xml, size_t len, int maxEpisodes, Channel &out) {
  const char *b = xml, *e = xml + len;
  const char *firstItem = findTag(b, e, "item");
  const char *head = firstItem ? firstItem : e;

  // The channel's title: the first <title> that isn't inside its <image>.
  const char *p = b;
  Element el;
  while (element(p, head, "title", el)) {
    const char *image = findTag(p, el.tag.b, "image");
    Element im;
    if (image && element(image, head, "image", im) && im.end > el.tag.b) {
      p = im.end;
      continue;
    }
    out.title = shorten(collapse(text(el.inner)), kTitleMax);
    break;
  }
  std::string about = elementText(b, head, "itunes:summary");
  if (about.empty()) about = elementText(b, head, "description");
  out.description = plainText(about, kChannelTextMax);

  const char *q = firstItem;
  while (q && static_cast<int>(out.episodes.size()) < maxEpisodes) {
    Element item;
    if (!element(q, e, "item", item)) break;  // the download stopped inside it
    q = item.end;
    const char *ib = item.inner.b, *ie = item.inner.e;
    const char *enc = findTag(ib, ie, "enclosure");
    if (!enc) continue;  // not an episode with audio
    const char *encEnd = static_cast<const char *>(memchr(enc, '>', ie - enc));
    if (!encEnd) continue;
    const Span encTag{enc + 1, encEnd};
    Episode ep;
    ep.url = attribute(encTag, "url");
    if (ep.url.empty()) continue;
    ep.type = attribute(encTag, "type");
    ep.bytes = atoll(attribute(encTag, "length").c_str());
    ep.title = shorten(collapse(elementText(ib, ie, "title")), kTitleMax);
    ep.guid = collapse(elementText(ib, ie, "guid"));
    if (ep.guid.empty()) ep.guid = ep.url;
    ep.published = parseDate(collapse(elementText(ib, ie, "pubDate")));
    ep.seconds = parseDuration(elementText(ib, ie, "itunes:duration"));
    const std::string number = collapse(elementText(ib, ie, "itunes:episode"));
    if (!number.empty()) ep.number = atoi(number.c_str());
    std::string about = elementText(ib, ie, "itunes:summary");
    if (about.empty()) about = elementText(ib, ie, "description");
    if (about.empty()) about = elementText(ib, ie, "content:encoded");
    ep.description = plainText(about, kEpisodeTextMax);
    out.episodes.push_back(std::move(ep));
  }
  return !out.title.empty() || !out.episodes.empty();
}

}  // namespace rss
