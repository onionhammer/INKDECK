#include "article.h"

#include <ctype.h>
#include <string.h>
#include <strings.h>

#include <algorithm>
#include <initializer_list>
#include <vector>

#include "rss.h"
#include "text.h"

namespace article {
namespace {

enum Tag : uint8_t {
  kOther, kP, kDiv, kSection, kArticle, kMain, kBody, kA, kHeading, kUl, kOl, kLi, kPre,
  kBlockquote, kTd, kTable, kHeader, kForm, kTime,
};

enum Flag : uint8_t {
  kHidden = 1,     // never the text: left out of the scores and the output
  kInHidden = 2,   // it, or something around it, is hidden
  kCandidate = 4,  // a paragraph has scored it
};

struct Node {
  uint32_t start = 0;  // the '<' of its start tag
  uint32_t end = 0;    // just past its end tag (or where it was closed for it)
  int32_t parent = -1;
  uint32_t text = 0;      // visible characters inside, hidden parts left out
  uint32_t linkText = 0;  // of them, inside links
  uint16_t commas = 0;
  uint16_t paras = 0;     // paragraphs inside that scored
  float score = 0;
  int8_t weight = 0;      // from its class, id...: -25, 0, +25
  Tag tag = kOther;
  uint8_t flags = 0;
};

constexpr uint32_t kMinParagraph = 25;   // characters for a paragraph to score
constexpr uint32_t kMinArticle = 200;    // characters for the page to have an article
constexpr uint32_t kRetryBelow = 600;    // a strict reading this short is tried loose

// --- Names --------------------------------------------------------------------------------------

bool contains(const std::string &s, std::initializer_list<const char *> words) {
  for (const char *w : words) {
    if (s.find(w) != std::string::npos) return true;
  }
  return false;
}

// Readability's lists, with what news sites add (BBC's data-component names among them).
bool unlikely(const std::string &names) {
  if (contains(names, {"article", "body", "column", "content", "main", "shadow"})) return false;
  if (contains(names, {"-ad-", "ad-slot", "ad-break", "advert", "agegate", "banner", "breadcrumb",
                       "byline", "caption", "combx", "comment", "community", "consent", "cookie",
                       "cover-wrap", "disqus", "extra", "footer", "gdpr", "header", "legends",
                       "menu", "most-read", "mostread", "newsletter", "pager", "pagination",
                       "popup", "promo", "recommend", "related", "remark", "replies", "rss",
                       "share", "shoutbox", "sidebar", "signup", "skyscraper", "social",
                       "sponsor", "subscribe", "supplemental", "tags", "topic-list", "video"})) {
    return true;
  }
  // Whole words: "ad", "ads".
  size_t at = 0;
  while (at < names.size()) {
    size_t end = names.find(' ', at);
    if (end == std::string::npos) end = names.size();
    const std::string word = names.substr(at, end - at);
    if (word == "ad" || word == "ads") return true;
    at = end + 1;
  }
  return false;
}

int8_t weightOf(const std::string &names) {
  if (names.empty()) return 0;
  int w = 0;
  if (contains(names, {"article", "body", "content", "entry", "hentry", "main", "page", "post",
                       "text", "blog", "story"})) {
    w += 25;
  }
  if (contains(names, {"banner", "combx", "comment", "contact", "foot", "masthead", "media",
                       "meta", "outbrain", "promo", "related", "scroll", "share", "shoutbox",
                       "sidebar", "skyscraper", "sponsor", "shopping", "tags", "tool", "widget",
                       "newsletter", "social", "byline", "caption", "recommend", "trending"})) {
    w -= 25;
  }
  return static_cast<int8_t>(w);
}

Tag tagOf(const char *n) {
  struct Named {
    const char *name;
    Tag tag;
  };
  static constexpr Named kTags[] = {
      {"p", kP},         {"div", kDiv},       {"section", kSection},
      {"article", kArticle}, {"main", kMain}, {"body", kBody},
      {"a", kA},         {"h1", kHeading},    {"h2", kHeading},
      {"h3", kHeading},  {"h4", kHeading},    {"h5", kHeading},
      {"h6", kHeading},  {"ul", kUl},         {"ol", kOl},
      {"li", kLi},       {"pre", kPre},       {"blockquote", kBlockquote},
      {"td", kTd},       {"table", kTable},   {"header", kHeader},
      {"form", kForm},   {"time", kTime},
  };
  for (const Named &t : kTags) {
    if (strcmp(n, t.name) == 0) return t.tag;
  }
  return kOther;
}

bool isAny(const char *n, std::initializer_list<const char *> names) {
  for (const char *x : names) {
    if (strcmp(n, x) == 0) return true;
  }
  return false;
}

bool isVoid(const char *n) {
  return isAny(n, {"area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta",
                   "param", "source", "track", "wbr"});
}
// Their content isn't markup: skipped to their end tag.
bool isRaw(const char *n) {
  return isAny(n, {"script", "style", "textarea", "title", "noscript", "xmp", "svg", "math",
                   "template", "iframe", "video", "audio", "canvas", "object", "select",
                   "button"});
}
// Never the text, whatever their names say.
bool isHiddenTag(const char *n) {
  return isAny(n, {"nav", "aside", "footer", "figure", "figcaption", "picture", "dialog"});
}
// A start tag that closes an open <p>.
bool closesP(const char *n) {
  return isAny(n, {"p", "div", "section", "article", "main", "h1", "h2", "h3", "h4", "h5", "h6",
                   "ul", "ol", "pre", "blockquote", "table", "figure", "header", "footer", "nav",
                   "aside", "form", "dl", "hr", "address", "details", "fieldset"});
}

// --- The page -----------------------------------------------------------------------------------

struct Attr {
  const char *name;
  size_t nameLen;
  const char *value;
  size_t valueLen;
  bool is(const char *n) const { return strlen(n) == nameLen && strncasecmp(name, n, nameLen) == 0; }
  std::string text() const {  // entities decoded
    return rss::plainText(std::string(value, valueLen), 2048);
  }
};

// Calls f(attr) for each attribute in [p, e) (after the tag's name, before its '>').
template <typename F>
void forEachAttr(const char *p, const char *e, F f) {
  while (p < e) {
    while (p < e && (isspace(static_cast<unsigned char>(*p)) || *p == '/')) ++p;
    const char *nb = p;
    while (p < e && !isspace(static_cast<unsigned char>(*p)) && *p != '=' && *p != '/') ++p;
    const char *ne = p;
    if (ne == nb) {
      ++p;
      continue;
    }
    while (p < e && isspace(static_cast<unsigned char>(*p))) ++p;
    const char *vb = p, *ve = p;
    if (p < e && *p == '=') {
      ++p;
      while (p < e && isspace(static_cast<unsigned char>(*p))) ++p;
      if (p < e && (*p == '"' || *p == '\'')) {
        const char q = *p++;
        vb = p;
        while (p < e && *p != q) ++p;
        ve = p;
        if (p < e) ++p;
      } else {
        vb = p;
        while (p < e && !isspace(static_cast<unsigned char>(*p))) ++p;
        ve = p;
      }
    }
    f(Attr{nb, static_cast<size_t>(ne - nb), vb, static_cast<size_t>(ve - vb)});
  }
}

// The '>' that ends the tag starting at p, past quoted attribute values; nullptr if none.
const char *tagEnd(const char *p, const char *e) {
  char quote = 0;
  char last = 0;  // the last non-space character outside quotes
  for (const char *q = p + 1; q < e; ++q) {
    const char c = *q;
    if (quote) {
      if (c == quote) quote = 0;
    } else if ((c == '"' || c == '\'') && last == '=') {
      quote = c;
    } else if (c == '>') {
      return q;
    } else if (!isspace(static_cast<unsigned char>(c))) {
      last = c;
    }
  }
  return nullptr;
}

// needle in [p, e), case and all; nullptr if absent.
const char *findCs(const char *p, const char *e, const char *needle) {
  const size_t n = strlen(needle);
  const char *hit = std::search(p, e, needle, needle + n);
  return hit == e ? nullptr : hit;
}

const char *findCi(const char *p, const char *e, const char *needle) {
  const size_t n = strlen(needle);
  for (; p + n <= e; ++p) {
    if (strncasecmp(p, needle, n) == 0) return p;
  }
  return nullptr;
}

// A JSON string starting at p (just past its opening quote), decoded.
std::string jsonString(const char *p, const char *e) {
  std::string out;
  while (p < e && *p != '"') {
    if (*p == '\\' && p + 1 < e) {
      const char c = p[1];
      p += 2;
      switch (c) {
        case 'n': out += '\n'; break;
        case 't': out += ' '; break;
        case 'r': break;
        case 'u': {
          if (e - p < 4) return out;
          uint32_t cp = strtoul(std::string(p, p + 4).c_str(), nullptr, 16);
          p += 4;
          if (cp >= 0xD800 && cp < 0xDC00 && e - p >= 6 && p[0] == '\\' && p[1] == 'u') {
            const uint32_t lo = strtoul(std::string(p + 2, p + 6).c_str(), nullptr, 16);
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            p += 6;
          }
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
          break;
        }
        default: out += c;
      }
    } else {
      out += *p++;
    }
  }
  return out;
}

// The string value of the first "key": "..." in [p, e).
std::string jsonField(const char *p, const char *e, const char *key) {
  const std::string k = std::string("\"") + key + "\"";
  for (const char *at = p; (at = std::search(at, e, k.begin(), k.end())) != e; at += k.size()) {
    const char *q = at + k.size();
    while (q < e && isspace(static_cast<unsigned char>(*q))) ++q;
    if (q >= e || *q != ':') continue;
    ++q;
    while (q < e && isspace(static_cast<unsigned char>(*q))) ++q;
    if (q < e && *q == '"') return jsonString(q + 1, e);
  }
  return {};
}

std::string collapse(const std::string &s) { return rss::plainText(s, s.size() + 16); }

// "Story - BBC News" -> "Story": a <title>'s site suffix.
std::string trimTitle(std::string t) {
  for (const char *sep : {" | ", " - ", " \xE2\x80\x93 ", " \xE2\x80\x94 "}) {
    const size_t at = t.rfind(sep);
    if (at != std::string::npos && at > 10 && t.size() - at < 40) t.resize(at);
  }
  return t;
}

std::string lower(std::string s) {
  s = text::lower(std::move(s));
  while (!s.empty() && (s.back() == ':' || s.back() == ' ')) s.pop_back();
  return s;
}

// Page furniture that lands inside the text as blocks of its own.
bool boilerplate(const std::string &block) {
  if (block.size() > 60) return false;
  const std::string b = lower(block);
  for (const char *w : {"advertisement", "advert", "skip advertisement", "listen to this article",
                        "by", "posted", "updated", "share", "share this article", "watch next",
                        "read more", "continue reading", "story continues below advertisement",
                        "related", "most read", "in brief", "---"}) {
    if (b == w) return true;
  }
  return b.rfind("published on ", 0) == 0;
}

// The text, tidied: what comes before the title's heading (section names, dates), page
// furniture, a block that repeats the one before, headings with nothing under them ("More on
// this story" over the links that went) and rules at the end, out.
std::string tidy(const std::string &md, const std::string &title) {
  std::vector<std::string> blocks;
  for (size_t at = 0; at < md.size();) {
    size_t end = md.find("\n\n", at);
    if (end == std::string::npos) end = md.size();
    blocks.push_back(md.substr(at, end - at));
    at = end + 2;
  }
  const std::string heading = "## " + lower(title);
  for (size_t i = 0; i < blocks.size() && i < 8; ++i) {
    if (lower(blocks[i]) == heading) {
      bool short_ = true;
      for (size_t k = 0; k < i; ++k) short_ = short_ && blocks[k].size() < 100;
      if (short_) blocks.erase(blocks.begin(), blocks.begin() + i);
      break;
    }
    if (blocks[i].size() >= 100) break;
  }
  std::vector<std::string> kept;
  for (const std::string &b : blocks) {
    if (boilerplate(b) || (!kept.empty() && kept.back() == b)) continue;
    kept.push_back(b);
  }
  // From the end: bare headings and rules.
  std::vector<std::string> out;
  for (size_t i = 0; i < kept.size(); ++i) {
    const bool heading = kept[i].rfind("## ", 0) == 0;
    if (heading && (i + 1 == kept.size() || kept[i + 1].rfind("## ", 0) == 0)) continue;
    out.push_back(kept[i]);
  }
  while (!out.empty() && (out.back() == "---" || out.back().rfind("## ", 0) == 0)) out.pop_back();
  std::string joined;
  for (const std::string &b : out) {
    if (!joined.empty()) joined += "\n\n";
    joined += b;
  }
  return joined;
}

// "Story | CBC News" -> "Story", when the end names the site.
std::string trimSite(std::string t, const std::string &site, const std::string &host) {
  std::string key = lower(host.substr(0, host.find('.')));
  for (const char *sep : {" | ", " - ", " \xE2\x80\x93 ", " \xE2\x80\x94 "}) {
    const size_t at = t.rfind(sep);
    if (at == std::string::npos || at < 10) continue;
    std::string tail = lower(t.substr(at + strlen(sep)));
    tail.erase(std::remove(tail.begin(), tail.end(), ' '), tail.end());
    std::string s = lower(site);
    s.erase(std::remove(s.begin(), s.end(), ' '), s.end());
    if ((!s.empty() && (tail.find(s) != std::string::npos || s.find(tail) != std::string::npos)) ||
        (!key.empty() && tail.find(key) != std::string::npos)) {
      t.resize(at);
    }
  }
  return t;
}

class Page {
 public:
  // strict: hide what class names say is clutter. A page that wraps its text in such a name
  // ("ContainerWithSidebarWrapper") comes out empty, and is read again without (Readability).
  // spare: start offsets of elements not to hide by name (the second reading's: the article's
  // ancestors as a reading without names found them).
  Page(const char *html, size_t len, bool strict, const std::vector<uint32_t> *spare = nullptr)
      : b_(html), e_(html + len), strict_(strict), spare_(spare) {}

  // The article's element and its ancestors, by start offset (sorted).
  std::vector<uint32_t> containerPath();

  void scan();
  bool article(const std::string &url, size_t maxBytes, Article &out);

 private:
  void push(Tag tag, uint32_t start, uint8_t flags, int8_t weight);
  void closeTop(uint32_t at);
  void addText(const char *p, const char *e);
  void meta(const char *p, const char *e);
  void scoreParagraph(const Node &n);
  void initCandidate(int i);
  float finalScore(int i) const {
    const Node &n = nodes_[i];
    const float density = n.text ? static_cast<float>(n.linkText) / n.text : 0;
    return n.score * (1 - density);
  }
  int choose();
  bool inside(int k, int i) const { return nodes_[k].start >= nodes_[i].start && nodes_[k].start < nodes_[i].end; }
  bool dropFromOutput(int i, int top) const;
  void appendClean(int i, int top, std::string &out) const;

  const char *b_, *e_;
  bool strict_;
  const std::vector<uint32_t> *spare_;
  std::vector<Node> nodes_;
  std::vector<int32_t> stack_;
  int hidden_ = 0;  // open hidden elements
  int links_ = 0;   // open links
  bool lastSpace_ = true;

  std::string title_, ogTitle_, site_, byline_, base_;
  time_t published_ = 0, firstTime_ = 0;
  std::vector<std::pair<const char *, const char *>> jsonLd_;
};

void Page::push(Tag tag, uint32_t start, uint8_t flags, int8_t weight) {
  Node n;
  n.start = start;
  n.end = static_cast<uint32_t>(e_ - b_);
  n.parent = stack_.empty() ? -1 : stack_.back();
  n.tag = tag;
  n.weight = weight;
  n.flags = flags;
  if (hidden_ || (flags & kHidden)) n.flags |= kInHidden;
  if (flags & kHidden) ++hidden_;
  if (tag == kA) ++links_;
  stack_.push_back(static_cast<int32_t>(nodes_.size()));
  nodes_.push_back(n);
}

void Page::closeTop(uint32_t at) {
  const int i = stack_.back();
  stack_.pop_back();
  Node &n = nodes_[i];
  n.end = at;
  if (n.flags & kHidden) --hidden_;
  if (n.tag == kA) --links_;
  if (n.tag == kP && !(n.flags & kInHidden)) scoreParagraph(n);
}

void Page::initCandidate(int i) {
  Node &n = nodes_[i];
  if (n.flags & kCandidate) return;
  n.flags |= kCandidate;
  switch (n.tag) {
    case kDiv: n.score = 5; break;
    case kPre: case kTd: case kBlockquote: n.score = 3; break;
    case kUl: case kOl: case kLi: case kForm: n.score = -3; break;
    case kHeading: n.score = -5; break;
    default: n.score = 0;
  }
  n.score += n.weight;
}

void Page::scoreParagraph(const Node &n) {
  if (n.text < kMinParagraph) return;
  const float score = 1 + n.commas + std::min<uint32_t>(n.text / 100, 3);
  int a = n.parent;
  for (int level = 0; a >= 0 && level < 5; ++level, a = nodes_[a].parent) {
    initCandidate(a);
    nodes_[a].score += score / (level == 0 ? 1 : level == 1 ? 2 : level * 3);
  }
  for (a = n.parent; a >= 0; a = nodes_[a].parent) {
    if (nodes_[a].paras < 0xFFFF) ++nodes_[a].paras;
  }
}

void Page::addText(const char *p, const char *e) {
  if (hidden_) return;
  uint32_t chars = 0, commas = 0;
  for (; p < e; ++p) {
    const unsigned char c = static_cast<unsigned char>(*p);
    if (isspace(c)) {
      if (!lastSpace_) ++chars;
      lastSpace_ = true;
      continue;
    }
    lastSpace_ = false;
    if ((c & 0xC0) == 0x80) continue;  // a UTF-8 continuation
    if (c == '&') {                    // an entity: one character
      const char *semi = static_cast<const char *>(memchr(p, ';', std::min<ptrdiff_t>(e - p, 10)));
      if (semi) p = semi;
    }
    if (c == ',') ++commas;
    ++chars;
  }
  if (!chars) return;
  for (const int32_t i : stack_) {
    Node &n = nodes_[i];
    n.text += chars;
    n.commas = static_cast<uint16_t>(std::min<uint32_t>(n.commas + commas, 0xFFFF));
    if (links_) n.linkText += chars;
  }
}

void Page::meta(const char *p, const char *e) {
  std::string key, content;
  forEachAttr(p, e, [&](const Attr &a) {
    if (a.is("property") || a.is("name") || a.is("itemprop")) {
      key.assign(a.value, a.valueLen);
      key = text::lower(std::move(key));
    } else if (a.is("content")) {
      content = collapse(a.text());
    }
  });
  if (key.empty() || content.empty()) return;
  if (key == "og:title" || (key == "twitter:title" && ogTitle_.empty())) {
    ogTitle_ = content;
  } else if (key == "og:site_name") {
    site_ = content;
  } else if (key == "og:url") {
    if (base_.empty()) base_ = content;
  } else if (key == "author" || key == "article:author" || key == "byl" ||
             key == "parsely-author" || key == "sailthru.author") {
    if (byline_.empty() && content.rfind("http", 0) != 0) {
      byline_ = strncasecmp(content.c_str(), "by ", 3) == 0 ? content.substr(3) : content;
    }
  } else if (key == "article:published_time" || key == "datepublished" ||
             key == "og:published_time" || key == "parsely-pub-date" || key == "date" ||
             key == "dc.date" || key == "pubdate") {
    if (!published_) published_ = rss::parseIsoDate(content);
  }
}

void Page::scan() {
  nodes_.reserve((e_ - b_) / 64);
  const char *p = b_;
  while (p < e_) {
    if (*p != '<') {
      const char *next = static_cast<const char *>(memchr(p, '<', e_ - p));
      if (!next) next = e_;
      addText(p, next);
      p = next;
      continue;
    }
    const uint32_t start = static_cast<uint32_t>(p - b_);
    if (e_ - p >= 4 && memcmp(p, "<!--", 4) == 0) {
      const char *end = findCs(p + 4, e_, "-->");
      p = end ? end + 3 : e_;
      continue;
    }
    const bool closing = p + 1 < e_ && p[1] == '/';
    const char *nb = p + (closing ? 2 : 1);
    if (nb >= e_ || !(isalpha(static_cast<unsigned char>(*nb)) || *nb == '!' || *nb == '?')) {
      addText(p, p + 1);  // a bare '<'
      ++p;
      continue;
    }
    const char *gt = tagEnd(p, e_);
    if (!gt) break;
    if (*nb == '!' || *nb == '?') {  // <!DOCTYPE>, <?xml?>
      p = gt + 1;
      continue;
    }
    char name[16];
    size_t n = 0;
    const char *q = nb;
    for (; q < gt && (isalnum(static_cast<unsigned char>(*q)) || *q == '-' || *q == ':'); ++q) {
      if (n + 1 < sizeof name) name[n++] = static_cast<char>(tolower(static_cast<unsigned char>(*q)));
    }
    name[n] = 0;
    const char *attrs = q;
    const bool selfClosing = gt[-1] == '/';
    p = gt + 1;
    const uint32_t after = static_cast<uint32_t>(p - b_);

    if (closing) {
      const Tag tag = tagOf(name);
      // Pop to the matching element, if it's open not far up; a stray end tag does nothing.
      const char *nm = name;
      for (int k = static_cast<int>(stack_.size()) - 1, seen = 0; k >= 0 && seen < 16;
           --k, ++seen) {
        const Node &open = nodes_[stack_[k]];
        // Unknown tags are all kOther: compare the names in the page.
        bool match = open.tag == tag;
        if (match && tag == kOther) {
          const char *on = b_ + open.start + 1;
          match = strncasecmp(on, nm, n) == 0 &&
                  !(isalnum(static_cast<unsigned char>(on[n])) || on[n] == '-');
        }
        if (!match) continue;
        while (static_cast<int>(stack_.size()) > k + 1) closeTop(start);
        closeTop(after);
        break;
      }
      continue;
    }

    if (strcmp(name, "meta") == 0) {
      meta(attrs, gt);
      continue;
    }
    if (strcmp(name, "link") == 0 || strcmp(name, "base") == 0) {
      std::string rel, href;
      forEachAttr(attrs, gt, [&](const Attr &a) {
        if (a.is("rel")) rel.assign(a.value, a.valueLen);
        if (a.is("href")) href = collapse(a.text());
      });
      if (!href.empty() && (name[0] == 'b' || rel == "canonical")) base_ = href;
      continue;
    }
    if (isVoid(name) || selfClosing) {
      if (strcmp(name, "br") == 0) lastSpace_ = true;
      continue;
    }
    if (isRaw(name)) {
      const std::string close = std::string("</") + name;
      const char *end = findCi(p, e_, close.c_str());
      const char *stop = end ? end : e_;
      if (strcmp(name, "title") == 0 && title_.empty()) {
        title_ = collapse(std::string(p, stop));
      } else if (strcmp(name, "script") == 0 && findCi(attrs, gt, "ld+json")) {
        jsonLd_.push_back({p, stop});
      }
      if (!end) break;
      const char *endGt = static_cast<const char *>(memchr(end, '>', e_ - end));
      p = endGt ? endGt + 1 : e_;
      lastSpace_ = true;
      continue;
    }

    const Tag tag = tagOf(name);
    if (closesP(name) && !stack_.empty() && nodes_[stack_.back()].tag == kP) closeTop(start);
    if (tag == kLi && !stack_.empty() && nodes_[stack_.back()].tag == kLi) closeTop(start);
    if (tag == kP || tag == kHeading || tag == kLi || tag == kDiv) lastSpace_ = true;

    std::string names;
    bool hide = isHiddenTag(name);
    forEachAttr(attrs, gt, [&](const Attr &a) {
      if (a.is("class") || a.is("id") || a.is("role") || a.is("data-component") ||
          a.is("data-testid")) {
        if (!names.empty()) names += ' ';
        names.append(a.value, a.valueLen);
      } else if (a.is("hidden")) {
        hide = true;
      } else if (a.is("aria-hidden")) {
        if (a.valueLen == 4 && strncasecmp(a.value, "true", 4) == 0) hide = true;
      } else if (a.is("style")) {
        std::string s(a.value, a.valueLen);
        s.erase(std::remove(s.begin(), s.end(), ' '), s.end());
        if (s.find("display:none") != std::string::npos) hide = true;
      } else if (tag == kTime && a.is("datetime") && !firstTime_) {
        firstTime_ = rss::parseIsoDate(std::string(a.value, a.valueLen));
      }
    });
    names = text::lower(std::move(names));
    const bool structural = tag == kBody || tag == kArticle || tag == kMain || tag == kA;
    if (strict_ && !structural && unlikely(names) &&
        !(spare_ && std::binary_search(spare_->begin(), spare_->end(), start))) {
      hide = true;
    }
    push(tag, start, hide ? kHidden : 0, weightOf(names));
  }
  while (!stack_.empty()) closeTop(static_cast<uint32_t>(e_ - b_));
}

// Inside the article, what doesn't go into the text: hidden parts, and blocks that are mostly
// links or named as clutter (Readability's "clean conditionally").
bool Page::dropFromOutput(int i, int top) const {
  const Node &n = nodes_[i];
  if (n.flags & kHidden) return true;
  if (i == top) return false;
  const bool block = n.tag == kDiv || n.tag == kSection || n.tag == kUl || n.tag == kOl ||
                     n.tag == kTable || n.tag == kHeader || n.tag == kForm;
  if (!block) return false;
  if (n.weight < 0) return true;
  if (n.commas >= 10) return false;
  const float density = n.text ? static_cast<float>(n.linkText) / n.text : 0;
  if (density > 0.5f) return true;
  return n.paras == 0 && density > 0.25f && n.text < 200;
}

// Node i's HTML, without what dropFromOutput() leaves out.
void Page::appendClean(int i, int top, std::string &out) const {
  const Node &n = nodes_[i];
  uint32_t at = n.start;
  for (int k = i + 1; k < static_cast<int>(nodes_.size()) && nodes_[k].start < n.end;) {
    const Node &c = nodes_[k];
    if (!dropFromOutput(k, top)) {
      ++k;
      continue;
    }
    if (c.start > at) out.append(b_ + at, b_ + c.start);
    at = std::max(at, c.end);
    // Past everything inside it.
    for (++k; k < static_cast<int>(nodes_.size()) && nodes_[k].start < c.end; ++k) {
    }
  }
  if (n.end > at) out.append(b_ + at, b_ + n.end);
  out += "\n";
}

// The article's element: the best candidate, up to an ancestor that holds more of the text
// (Readability), then out to hold other strong candidates too (BBC's text comes in blocks,
// each its own container, between pictures and videos).
int Page::choose() {
  int top = -1;
  float best = 0;
  for (int i = 0; i < static_cast<int>(nodes_.size()); ++i) {
    const Node &n = nodes_[i];
    if (!(n.flags & kCandidate) || (n.flags & kInHidden)) continue;
    const float s = finalScore(i);
    if (top < 0 || s > best) {
      top = i;
      best = s;
    }
  }
  if (top < 0) return -1;
  float last = nodes_[top].score;
  const float threshold = last / 3;
  for (int a = nodes_[top].parent; a >= 0 && nodes_[a].tag != kBody; a = nodes_[a].parent) {
    if (!(nodes_[a].flags & kCandidate)) continue;
    if (nodes_[a].score < threshold) break;
    if (nodes_[a].score > last) {
      top = a;
      break;
    }
    last = nodes_[a].score;
  }
  best = finalScore(top);
  for (int i = 0; i < static_cast<int>(nodes_.size()); ++i) {
    const Node &n = nodes_[i];
    if (!(n.flags & kCandidate) || (n.flags & kInHidden) || n.paras < 2) continue;
    if (inside(i, top) || finalScore(i) < best * 0.25f) continue;
    int a = nodes_[top].parent;
    while (a >= 0 && !inside(i, a)) a = nodes_[a].parent;
    if (a < 0 || nodes_[a].tag == kBody || (nodes_[a].flags & kInHidden)) continue;
    // Not if the common ancestor is mostly something else (the whole page).
    if (nodes_[a].text > 3 * (nodes_[top].text + n.text)) continue;
    top = a;
  }
  return top;
}

std::vector<uint32_t> Page::containerPath() {
  std::vector<uint32_t> out;
  for (int a = choose(); a >= 0; a = nodes_[a].parent) out.push_back(nodes_[a].start);
  std::sort(out.begin(), out.end());
  return out;
}

bool Page::article(const std::string &url, size_t maxBytes, Article &out) {
  const std::string base = !base_.empty() ? rss::resolveUrl(url, base_) : url;
  out.site = !site_.empty() ? site_ : host(url);
  out.title = trimSite(!ogTitle_.empty() ? ogTitle_ : trimTitle(title_), out.site, host(url));
  out.published = published_ ? published_ : firstTime_;
  out.byline = byline_;
  for (const auto &j : jsonLd_) {
    if (out.byline.empty()) {
      const char *a = findCs(j.first, j.second, "\"author\"");
      if (a) out.byline = collapse(jsonField(a, std::min(a + 400, j.second), "name"));
    }
    if (!out.published) out.published = rss::parseIsoDate(jsonField(j.first, j.second,
                                                                      "datePublished"));
  }

  const int top = choose();
  std::string html;
  if (top >= 0) {
    const float best = finalScore(top);
    // Siblings that score nearly as well, or are paragraphs of their own.
    const int parent = nodes_[top].parent;
    const float siblingMin = std::max(10.0f, best * 0.2f);
    for (int i = 0; i < static_cast<int>(nodes_.size()); ++i) {
      const Node &n = nodes_[i];
      if (i != top && (parent < 0 || n.parent != parent)) continue;
      if (i != top) {
        if (n.flags & kInHidden) continue;
        const float density = n.text ? static_cast<float>(n.linkText) / n.text : 0;
        const bool scored = (n.flags & kCandidate) && finalScore(i) >= siblingMin;
        const bool paragraph = n.tag == kP && n.text > 80 && density < 0.25f;
        if (!scored && !paragraph) continue;
      }
      appendClean(i, top, html);
    }
  }
  out.markdown = tidy(rss::markdownFromHtml(html, maxBytes, base), out.title);

  if (out.markdown.size() < kMinArticle) {
    // Some sites keep the text in their JSON-LD.
    for (const auto &j : jsonLd_) {
      std::string body = jsonField(j.first, j.second, "articleBody");
      if (body.size() < kMinArticle) continue;
      // Its paragraphs are lines: Markdown's are blank-line apart.
      std::string md;
      for (size_t at = 0; at < body.size();) {
        size_t end = body.find('\n', at);
        if (end == std::string::npos) end = body.size();
        const std::string line = collapse(body.substr(at, end - at));
        if (!line.empty()) md += (md.empty() ? "" : "\n\n") + line;
        at = end + 1;
        if (md.size() > maxBytes) break;
      }
      out.markdown = md;
      break;
    }
  }
  return out.markdown.size() >= kMinArticle;
}

}  // namespace

std::string host(const std::string &url) {
  size_t at = url.find("://");
  if (at == std::string::npos) return {};
  at += 3;
  size_t end = url.find_first_of("/?#:", at);
  std::string h = url.substr(at, end == std::string::npos ? std::string::npos : end - at);
  if (h.rfind("www.", 0) == 0) h.erase(0, 4);
  return h;
}

bool extract(const char *html, size_t len, const std::string &url, size_t maxBytes, Article &out) {
  out = Article{};
  {
    Page page(html, len, true);
    page.scan();
    if (page.article(url, maxBytes, out) && out.markdown.size() >= kRetryBelow) return true;
  }
  // Read without names to find where the text is, then strictly again, sparing the elements
  // around it.
  std::vector<uint32_t> path;
  {
    Page loose(html, len, false);
    loose.scan();
    path = loose.containerPath();
  }
  Article spared;
  Page page(html, len, true, &path);
  page.scan();
  const bool ok = page.article(url, maxBytes, spared);
  if (spared.markdown.size() > out.markdown.size()) out = std::move(spared);
  return ok || out.markdown.size() >= kMinArticle;
}

}  // namespace article
