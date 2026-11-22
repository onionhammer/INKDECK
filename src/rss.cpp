#include "rss.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <algorithm>
#include <initializer_list>

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
    {"zwnj", 0x200C},  {"zwj", 0x200D},    {"shy", 0xAD},      {"thinsp", 0x2009},
    {"ensp", 0x2002},  {"emsp", 0x2003},   {"bull", 0x2022},   {"middot", 0xB7},
    {"trade", 0x2122}, {"reg", 0xAE},      {"deg", 0xB0},      {"times", 0xD7},
    {"euro", 0x20AC},  {"pound", 0xA3},    {"laquo", 0xAB},    {"raquo", 0xBB},
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

// --- News ------------------------------------------------------------------------------------

namespace {

constexpr size_t kSummaryMax = 400;
constexpr size_t kOverrun = 4096;  // one paragraph may run this far past the body's limit
constexpr const char *kBodyCutLine = "*[Cut off here: too long for the board.]*";

// p[0] == '<': does a tag start here (not "2 < 3")?
bool tagStarts(const char *p, const char *e) {
  if (e - p < 2) return false;
  const unsigned char c = static_cast<unsigned char>(p[1]);
  return isalpha(c) || c == '/' || c == '!' || c == '?';
}

bool hasTags(const std::string &s) {
  const char *b = s.data(), *e = b + s.size();
  for (const char *p = b; p < e; ++p) {
    if (*p == '<' && tagStarts(p, e) && memchr(p, '>', e - p)) return true;
  }
  return false;
}

// A title, plain: some feeds put markup in it (escaped, so it arrives as tags here).
std::string plainTitle(const std::string &raw) {
  return hasTags(raw) ? plainText(raw, kTitleMax) : shorten(collapse(raw), kTitleMax);
}

bool isAny(const std::string &name, std::initializer_list<const char *> names) {
  for (const char *n : names) {
    if (name == n) return true;
  }
  return false;
}

// Builds the Markdown: text goes in through put(), structure through the block calls.
class Markdown {
 public:
  explicit Markdown(size_t maxBytes) : max_(maxBytes) {
    out_.reserve(std::min<size_t>(maxBytes, 4096));
  }

  bool full() const { return cut_; }

  // A paragraph boundary: the next text starts a new block, with prefix ("## ", "- ").
  void block(const std::string &prefix = "") {
    endLink();  // a link ends at a block's edge
    if (cut_) return;
    if (started_ && out_.size() >= max_) {
      cut_ = true;
      return;
    }
    needBlock_ = true;
    prefix_ = prefix;
  }
  void lineBreak() {
    endLink();
    if (started_ && !needBlock_) needBreak_ = true;
  }
  void rule() {
    block();
    if (cut_) return;
    open();
    out_ += "---";
    block();
  }
  void cellBreak() {  // between a table row's cells
    if (started_ && !needBlock_) {
      out_ += " \xC2\xB7 ";
      needSpace_ = false;
    }
  }
  void quote(int delta) {
    quote_ += delta;
    if (quote_ < 0) quote_ = 0;
    block();
  }

  // A link around the text that comes before endLink(): [text](url), once text comes; "" for
  // none.
  void startLink(const std::string &url) {
    endLink();
    linkUrl_.clear();
    for (const char c : url) {  // what would end the (url) early
      if (c == ' ') {
        linkUrl_ += "%20";
      } else if (c == ')') {
        linkUrl_ += "%29";
      } else if (c == '(') {
        linkUrl_ += "%28";
      } else {
        linkUrl_ += c;
      }
    }
    linkPending_ = !linkUrl_.empty();
  }
  void endLink() {
    if (linkOpen_ && !cut_ && linkAt_ < out_.size()) {
      out_ += "](";
      out_ += linkUrl_;
      out_ += ')';
    }
    linkOpen_ = linkPending_ = false;
  }

  // Text, entities already decoded. Runs of whitespace become one space.
  void put(const char *p, size_t n, bool pre = false) {
    for (size_t i = 0; i < n && !cut_; ++i) {
      const char c = p[i];
      if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
        if (pre && c == '\n') {
          lineBreak();
        } else if (started_ && !needBlock_ && !needBreak_) {
          needSpace_ = true;
        }
        continue;
      }
      open();
      out_ += linkOpen_ && c == '[' ? '(' : linkOpen_ && c == ']' ? ')' : c;
      if (out_.size() > max_ + kOverrun) cutAtSpace();
    }
  }
  void put(const std::string &s) { put(s.data(), s.size()); }

  std::string finish() {
    endLink();
    while (!out_.empty() && (out_.back() == ' ' || out_.back() == '\n')) out_.pop_back();
    if (cut_ && !out_.empty()) {
      out_ += "\n\n";
      out_ += kBodyCutLine;
    }
    return std::move(out_);
  }

 private:
  // Before the next visible character: the pending break, space or block start.
  void open() {
    if (needBlock_ || !started_) {
      if (started_) out_ += "\n\n";
      if (quote_) out_ += "> ";
      out_ += prefix_;
      prefix_.clear();
      started_ = true;
    } else if (needBreak_) {
      out_ += '\n';
      if (quote_) out_ += "> ";
    } else if (needSpace_) {
      out_ += ' ';
    }
    needBlock_ = needBreak_ = needSpace_ = false;
    if (linkPending_) {
      linkAt_ = out_.size();
      out_ += '[';
      linkOpen_ = true;
      linkPending_ = false;
    }
  }

  // One paragraph that never ends: cut at its last space, and stop.
  void cutAtSpace() {
    size_t at = out_.rfind(' ');
    if (at == std::string::npos || at < max_ / 2) at = max_;
    while (at > 0 && (static_cast<uint8_t>(out_[at]) & 0xC0) == 0x80) --at;
    out_.resize(at);
    cut_ = true;
  }

  std::string out_;
  std::string prefix_;
  std::string linkUrl_;
  size_t linkAt_ = 0;  // where the open link's [ is
  size_t max_;
  int quote_ = 0;
  bool started_ = false;
  bool needBlock_ = false;
  bool needBreak_ = false;
  bool needSpace_ = false;
  bool cut_ = false;
  bool linkPending_ = false;  // a [ goes before the next visible character
  bool linkOpen_ = false;
};

// "x@y.z (Ann Lee)" -> "Ann Lee"; a bare e-mail address isn't a byline.
std::string bylineFrom(const std::string &raw) {
  std::string s = collapse(raw);
  const size_t open = s.find('(');
  const size_t close = s.rfind(')');
  if (open != std::string::npos && close != std::string::npos && close > open) {
    s = collapse(s.substr(open + 1, close - open - 1));
  } else if (s.find('@') != std::string::npos && s.find(' ') == std::string::npos) {
    return {};
  }
  return shorten(s, 80);
}

// An Atom entry's link to the article: <link rel="alternate" href> or one without a rel.
std::string atomLink(const char *b, const char *e) {
  Element el;
  std::string fallback;
  for (const char *p = b; element(p, e, "link", el); p = el.end) {
    const std::string rel = attribute(el.tag, "rel");
    const std::string href = attribute(el.tag, "href");
    if (href.empty()) continue;
    if (rel.empty() || rel == "alternate") return href;
    if (fallback.empty() && rel != "self" && rel != "enclosure" && rel != "replies") {
      fallback = href;
    }
  }
  return fallback;
}

// The text of the first of several elements that has some.
std::string firstText(const char *b, const char *e, std::initializer_list<const char *> names,
                      const char **which = nullptr) {
  for (const char *name : names) {
    std::string t = elementText(b, e, name);
    if (!collapse(t).empty()) {
      if (which) *which = name;
      return t;
    }
  }
  return {};
}

time_t storyDate(const char *b, const char *e) {
  for (const char *name : {"pubDate", "dc:date", "published", "updated", "dc:modified"}) {
    const std::string t = collapse(elementText(b, e, name));
    if (t.empty()) continue;
    time_t v = parseDate(t);
    if (!v) v = parseIsoDate(t);
    if (v) return v;
  }
  return 0;
}

void readStory(const char *ib, const char *ie, bool atom, size_t maxBody, Story &s) {
  s.title = plainTitle(elementText(ib, ie, "title"));
  if (atom) {
    s.link = collapse(atomLink(ib, ie));
    s.id = collapse(elementText(ib, ie, "id"));
    Element who;
    if (element(ib, ie, "author", who)) {
      s.author = bylineFrom(elementText(who.inner.b, who.inner.e, "name"));
    }
  } else {
    s.link = collapse(elementText(ib, ie, "link"));
    s.id = collapse(elementText(ib, ie, "guid"));
    s.author = bylineFrom(firstText(ib, ie, {"dc:creator", "author"}));
    if (s.link.empty() && s.id.rfind("http", 0) == 0) s.link = s.id;  // a permalink guid
  }
  if (s.id.empty()) s.id = !s.link.empty() ? s.link : s.title;
  s.published = storyDate(ib, ie);

  const std::string content =
      atom ? elementText(ib, ie, "content") : elementText(ib, ie, "content:encoded");
  const std::string description = atom ? firstText(ib, ie, {"summary"})
                                       : firstText(ib, ie, {"description", "itunes:summary"});
  s.full = !collapse(content).empty();
  s.summary = plainText(!description.empty() ? description : content, kSummaryMax);
  s.body = markdownFromHtml(s.full ? content : description, maxBody, s.link);
  if (s.full && s.body.empty()) {  // content that was all pictures: the description, then
    s.full = false;
    s.body = markdownFromHtml(description, maxBody, s.link);
  }
}

}  // namespace

time_t parseIsoDate(const std::string &text) {
  const char *p = text.c_str();
  while (*p == ' ') ++p;
  int year = 0, month = 0, day = 0, used = 0;
  if (siscanf(p, "%4d-%2d-%2d%n", &year, &month, &day, &used) < 3) return 0;
  if (month < 1 || month > 12 || day < 1 || day > 31 || year < 1970) return 0;
  p += used;
  int hour = 0, minute = 0, second = 0;
  if (*p == 'T' || *p == 't' || *p == ' ') {
    int more = 0;
    if (siscanf(p + 1, "%2d:%2d%n", &hour, &minute, &more) < 2) return 0;
    p += 1 + more;
    if (*p == ':') {
      more = 0;
      siscanf(p + 1, "%2d%n", &second, &more);
      p += 1 + more;
    }
    if (*p == '.' || *p == ',') {
      ++p;
      while (isdigit(static_cast<unsigned char>(*p))) ++p;
    }
  }
  int zone = 0;  // minutes east of UTC
  if (*p == '+' || *p == '-') {
    int zh = 0, zm = 0, more = 0;
    if (siscanf(p + 1, "%2d%n", &zh, &more) == 1) {
      const char *q = p + 1 + more;
      if (*q == ':') ++q;
      siscanf(q, "%2d", &zm);
      zone = (zh * 60 + zm) * (*p == '-' ? -1 : 1);
    }
  }
  const long days = timeutil::daysFromCivil(year, month, day);
  return static_cast<time_t>(days) * 86400 + hour * 3600 + minute * 60 + second - zone * 60;
}

std::string resolveUrl(const std::string &base, const std::string &href) {
  const std::string h = collapse(href);
  if (h.empty() || h[0] == '#') return {};
  size_t colon = h.find(':');
  const size_t slash = h.find('/');
  if (colon != std::string::npos && (slash == std::string::npos || colon < slash)) {
    const std::string scheme = h.substr(0, colon);
    return strcasecmp(scheme.c_str(), "http") == 0 || strcasecmp(scheme.c_str(), "https") == 0
               ? h
               : std::string();  // javascript:, mailto:, tel:, data:
  }
  colon = base.find("://");
  if (colon == std::string::npos) return {};
  if (h.rfind("//", 0) == 0) return base.substr(0, colon + 1) + h;
  const size_t pathAt = base.find('/', colon + 3);
  const std::string origin = base.substr(0, pathAt);
  std::string path;
  if (h[0] == '/') {
    path = h;
  } else {
    std::string dir = pathAt == std::string::npos ? "/" : base.substr(pathAt);
    dir = dir.substr(0, dir.find_first_of("?#"));
    if (h[0] == '?') return origin + dir + h;
    dir.resize(dir.rfind('/') + 1);
    path = dir + h;
  }
  // "./" and "../" out of the path (not the query).
  const size_t q = path.find_first_of("?#");
  const std::string tail = q == std::string::npos ? std::string() : path.substr(q);
  std::vector<std::string> parts;
  size_t at = 1;
  const std::string bare = path.substr(0, q);
  while (at <= bare.size()) {
    size_t next = bare.find('/', at);
    if (next == std::string::npos) next = bare.size();
    const std::string part = bare.substr(at, next - at);
    if (part == "..") {
      if (!parts.empty()) parts.pop_back();
      if (next == bare.size()) parts.push_back("");
    } else if (part == ".") {
      if (next == bare.size()) parts.push_back("");
    } else {
      parts.push_back(part);
    }
    at = next + 1;
  }
  std::string out = origin;
  for (const std::string &part : parts) out += "/" + part;
  if (parts.empty()) out += "/";
  return out + tail;
}

std::string markdownFromHtml(const std::string &html, size_t maxBytes,
                             const std::string &linkBase) {
  Markdown md(maxBytes);
  struct List {
    bool ordered;
    int n;
  };
  std::vector<List> lists;
  std::string skipping;  // inside an element whose content doesn't show (a script, a picture)
  int skipDepth = 0;
  bool pre = false;
  const char *p = html.data(), *e = p + html.size();
  while (p < e && !md.full()) {
    if (*p == '<' && tagStarts(p, e)) {
      if (startsWith(p, e, "<!--")) {
        const char *end = find(p + 4, e, "-->");
        p = end ? end + 3 : e;
        continue;
      }
      if (startsWith(p, e, "<![CDATA[")) {
        const char *end = find(p + 9, e, "]]>");
        const char *stop = end ? end : e;
        if (!skipDepth) md.put(p + 9, stop - (p + 9), pre);
        p = end ? end + 3 : e;
        continue;
      }
      const char *gt = static_cast<const char *>(memchr(p, '>', e - p));
      if (!gt) break;
      const bool closing = p[1] == '/';
      const bool selfClosing = gt[-1] == '/';
      std::string name;
      const char *q = p + (closing ? 2 : 1);
      for (; q < gt && isalnum(static_cast<unsigned char>(*q)); ++q) {
        name += static_cast<char>(tolower(static_cast<unsigned char>(*q)));
      }
      const Span tag{q, gt};  // the attributes
      p = gt + 1;
      if (skipDepth) {
        if (name == skipping && !selfClosing) skipDepth += closing ? -1 : 1;
        continue;
      }
      if (isAny(name, {"script", "style", "noscript", "iframe", "svg", "object", "video",
                       "audio", "canvas", "template", "button", "form", "select", "math"})) {
        if (!closing && !selfClosing) {
          skipping = name;
          skipDepth = 1;
        }
      } else if (name == "a") {
        if (closing) {
          md.endLink();
        } else if (!linkBase.empty() && !selfClosing) {
          md.startLink(resolveUrl(linkBase, attribute(tag, "href")));
        }
      } else if (name == "br") {
        md.lineBreak();
      } else if (name == "hr") {
        md.rule();
      } else if (name.size() == 2 && name[0] == 'h' && name[1] >= '1' && name[1] <= '6') {
        md.block(closing ? "" : "## ");
      } else if (name == "ul" || name == "ol") {
        if (closing) {
          if (!lists.empty()) lists.pop_back();
        } else if (!selfClosing) {
          lists.push_back({name == "ol", 0});
        }
        md.block();
      } else if (name == "li") {
        if (closing) {
          md.block();
        } else {
          const int depth = std::max<int>(1, static_cast<int>(lists.size()));
          std::string marker(2 * std::min(depth - 1, 1), ' ');
          if (!lists.empty() && lists.back().ordered) {
            marker += std::to_string(++lists.back().n) + ". ";
          } else {
            marker += "- ";
          }
          md.block(marker);
        }
      } else if (name == "blockquote") {
        md.quote(closing ? -1 : 1);
      } else if (name == "pre") {
        pre = !closing;
        md.block();
      } else if (name == "td" || name == "th") {
        if (!closing) md.cellBreak();
      } else if (isAny(name, {"p", "div", "section", "article", "header", "footer", "main",
                              "aside", "figure", "figcaption", "address", "details", "summary",
                              "table", "thead", "tbody", "tfoot", "tr", "caption", "dl", "dt",
                              "dd", "center", "nav"})) {
        md.block();
      }
      continue;  // inline tags (a, b, em, span...) and pictures leave only their text
    }
    if (skipDepth) {
      ++p;
      continue;
    }
    if (*p == '&') {
      std::string decoded;
      p = entity(p, e, decoded);
      md.put(decoded.data(), decoded.size(), pre);
      continue;
    }
    const char *next = p + 1;
    while (next < e && *next != '<' && *next != '&') ++next;
    md.put(p, next - p, pre);
    p = next;
  }
  return md.finish();
}

bool parseNews(const char *xml, size_t len, int maxStories, size_t maxBody, NewsFeed &out) {
  const char *b = xml, *e = xml + len;
  const char *firstItem = findTag(b, e, "item");
  const char *firstEntry = firstItem ? nullptr : findTag(b, e, "entry");
  const bool atom = firstEntry != nullptr || (!firstItem && findTag(b, e, "feed"));
  out.kind = atom ? "ATOM" : findTag(b, firstItem ? firstItem : e, "rdf:RDF") ? "RDF" : "RSS";
  const char *first = atom ? firstEntry : firstItem;
  const char *head = first ? first : e;

  // The feed's title: the first <title> that isn't inside its <image> (RSS) or <author>.
  const char *p = b;
  Element el;
  while (element(p, head, "title", el)) {
    bool nested = false;
    for (const char *outer : {"image", "author"}) {
      const char *at = findTag(p, el.tag.b, outer);
      Element box;
      if (at && element(at, head, outer, box) && box.end > el.tag.b) {
        p = box.end;
        nested = true;
        break;
      }
    }
    if (nested) continue;
    out.title = plainTitle(text(el.inner));
    break;
  }
  out.description = plainText(atom ? firstText(b, head, {"subtitle", "tagline"})
                                   : firstText(b, head, {"description", "itunes:summary"}),
                              kChannelTextMax);

  const char *name = atom ? "entry" : "item";
  const char *q = first;
  while (q && static_cast<int>(out.stories.size()) < maxStories) {
    Element item;
    if (!element(q, e, name, item)) break;  // the download stopped inside it
    q = item.end;
    Story s;
    readStory(item.inner.b, item.inner.e, atom, maxBody, s);
    if (s.title.empty() && s.summary.empty()) continue;
    if (s.title.empty()) s.title = shorten(s.summary, 90);
    out.stories.push_back(std::move(s));
  }
  return !out.title.empty() || !out.stories.empty();
}

}  // namespace rss
