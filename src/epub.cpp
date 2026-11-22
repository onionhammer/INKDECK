#include "epub.h"

#include <ctype.h>
#include <esp32-hal-log.h>
#include <stdlib.h>
#include <string.h>

#include <map>

#include "flashfs.h"
#include "text.h"

namespace {

// ---------------------------------------------------------------------------------------------
// Tiny tag scanner

struct Tag {
  std::string name;  // lower-case local name, namespace prefix stripped
  size_t attrStart = 0, attrEnd = 0;
  size_t end = 0;    // index just past '>'
  bool closing = false;
  bool selfClosing = false;
  bool special = false;  // comment, doctype, processing instruction, CDATA: skip
};

std::string localName(const std::string &qname) {
  const size_t colon = qname.rfind(':');
  return colon == std::string::npos ? qname : qname.substr(colon + 1);
}

// s[pos] must be '<'. Returns false if the tag is unterminated.
bool parseTag(const std::string &s, size_t pos, Tag &t) {
  const size_t n = s.size();
  t = Tag{};
  auto skipTo = [&](const char *terminator, size_t from) {
    const size_t e = s.find(terminator, from);
    t.special = true;
    t.end = e == std::string::npos ? n : e + strlen(terminator);
    return true;
  };
  if (s.compare(pos, 4, "<!--") == 0) return skipTo("-->", pos + 4);
  if (s.compare(pos, 9, "<![CDATA[") == 0) return skipTo("]]>", pos + 9);
  if (pos + 1 < n && (s[pos + 1] == '?' || s[pos + 1] == '!')) return skipTo(">", pos);

  size_t i = pos + 1;
  if (i < n && s[i] == '/') {
    t.closing = true;
    ++i;
  }
  const size_t nameStart = i;
  while (i < n && !isspace(static_cast<unsigned char>(s[i])) && s[i] != '>' && s[i] != '/') ++i;
  t.name = localName(s.substr(nameStart, i - nameStart));
  t.name = text::lower(std::move(t.name));

  t.attrStart = i;
  char quote = 0;
  for (; i < n; ++i) {
    const char c = s[i];
    if (quote) {
      if (c == quote) quote = 0;
    } else if (c == '"' || c == '\'') {
      quote = c;
    } else if (c == '>') {
      break;
    }
  }
  if (i >= n) return false;
  t.attrEnd = i;
  t.selfClosing = i > t.attrStart && s[i - 1] == '/';
  t.end = i + 1;
  return true;
}

// s[i] must be '&'. Advances past the entity; a lone '&' decodes as itself.
uint32_t decodeEntity(const std::string &s, size_t &i) {
  const size_t semi = s.find(';', i);
  if (semi == std::string::npos || semi - i > 12) {
    ++i;
    return '&';
  }
  const uint32_t cp = text::entityCodepoint(s.substr(i + 1, semi - i - 1));
  if (cp == 0) {
    ++i;
    return '&';
  }
  i = semi + 1;
  return cp;
}

// Raw (entity-encoded) UTF-8 text -> decoded UTF-8. Used for attribute values like hrefs.
std::string decodeEntitiesUtf8(const std::string &raw) {
  if (raw.find('&') == std::string::npos) return raw;
  std::string out;
  for (size_t i = 0; i < raw.size();) {
    if (raw[i] != '&') {
      out += raw[i++];
      continue;
    }
    text::appendUtf8(out, decodeEntity(raw, i));
  }
  return out;
}

// Raw element text (entities, tags allowed) -> trimmed, whitespace-collapsed UTF-8. Used for
// the title, author and TOC labels, which the UI draws as UTF-8.
std::string decodeText(const std::string &raw) {
  std::string out;
  bool space = false;
  for (size_t i = 0; i < raw.size();) {
    if (raw[i] == '<') {  // inline markup inside a label (<span>, <br/>)
      const size_t close = raw.find('>', i);
      i = close == std::string::npos ? raw.size() : close + 1;
      space = true;
      continue;
    }
    const uint32_t cp = raw[i] == '&' ? decodeEntity(raw, i) : text::nextCodepoint(raw, i);
    if (cp <= 0x20 || cp == 0xA0) {
      space = true;
      continue;
    }
    if (cp == 0xAD || (cp >= 0x200B && cp <= 0x200D) || cp == 0xFEFF) continue;
    if (space && !out.empty()) out += ' ';
    space = false;
    text::appendUtf8(out, cp);
  }
  return out;
}

// The value of attribute `wanted`, or "" if it's missing; *found tells the two apart.
std::string attr(const std::string &s, const Tag &t, const char *wanted, bool *found = nullptr) {
  if (found) *found = false;
  size_t i = t.attrStart;
  const size_t e = t.attrEnd;
  auto skipSpace = [&] {
    while (i < e && (isspace(static_cast<unsigned char>(s[i])) || s[i] == '/')) ++i;
  };
  while (i < e) {
    skipSpace();
    const size_t keyStart = i;
    while (i < e && !isspace(static_cast<unsigned char>(s[i])) && s[i] != '=' && s[i] != '/') ++i;
    const std::string key = localName(s.substr(keyStart, i - keyStart));
    while (i < e && isspace(static_cast<unsigned char>(s[i]))) ++i;
    std::string value;
    if (i < e && s[i] == '=') {
      ++i;
      while (i < e && isspace(static_cast<unsigned char>(s[i]))) ++i;
      if (i < e && (s[i] == '"' || s[i] == '\'')) {
        const char quote = s[i++];
        const size_t valueStart = i;
        while (i < e && s[i] != quote) ++i;
        value = s.substr(valueStart, i - valueStart);
        ++i;
      } else {
        const size_t valueStart = i;
        while (i < e && !isspace(static_cast<unsigned char>(s[i]))) ++i;
        value = s.substr(valueStart, i - valueStart);
      }
    }
    if (key == wanted) {
      if (found) *found = true;
      return decodeEntitiesUtf8(value);
    }
    if (i == keyStart) ++i;  // no progress: skip junk
  }
  return {};
}

// Text from just after an opening tag up to the next '<'.
std::string elementText(const std::string &s, const Tag &t) {
  const size_t end = s.find('<', t.end);
  return s.substr(t.end, (end == std::string::npos ? s.size() : end) - t.end);
}

// ---------------------------------------------------------------------------------------------
// Paths inside the zip

std::string resolvePath(const std::string &baseDir, std::string href) {
  const size_t hash = href.find('#');
  if (hash != std::string::npos) href.resize(hash);
  href = text::percentDecode(href);
  const std::string joined = (!href.empty() && href[0] == '/') ? href.substr(1) : baseDir + href;

  std::vector<std::string> parts;
  size_t start = 0;
  while (start <= joined.size()) {
    size_t slash = joined.find('/', start);
    if (slash == std::string::npos) slash = joined.size();
    const std::string seg = joined.substr(start, slash - start);
    if (seg == "..") {
      if (!parts.empty()) parts.pop_back();
    } else if (!seg.empty() && seg != ".") {
      parts.push_back(seg);
    }
    start = slash + 1;
  }
  std::string out;
  for (const auto &p : parts) {
    if (!out.empty()) out += '/';
    out += p;
  }
  return out;
}

// ---------------------------------------------------------------------------------------------
// XHTML -> flow

class FlowBuilder {
 public:
  explicit FlowBuilder(std::string &out) : out_(out) {}

  void space() {
    if (!atParaStart_) pendingSpace_ = true;
  }
  void para() {
    if (!atParaStart_) {
      out_ += flow::kPara;
      atParaStart_ = true;
    }
    pendingSpace_ = false;
  }
  void center(bool on) { center_ = on; }
  // A picture paragraph (flow::kImage).
  void image(int index) {
    para();
    out_ += flow::kImage;
    out_ += static_cast<char>(flow::kImageBase + index);
    atParaStart_ = false;
    para();
  }
  void style(uint8_t s) { want_ = s; }
  // A soft hyphen: kept only inside a word, where the line may break.
  void softHyphen() {
    if (!atParaStart_ && !pendingSpace_ && !out_.empty() && out_.back() != flow::kSoftHyphen) {
      out_ += flow::kSoftHyphen;
    }
  }
  void text(const std::string &ascii) {
    for (const char c : ascii) put(c);
  }
  void put(char c) {
    if (c == ' ') {
      space();
      return;
    }
    if (atParaStart_ && center_) out_ += flow::kCenter;
    if (pendingSpace_) out_ += ' ';
    if (want_ != cur_) {
      out_ += static_cast<char>(flow::kStyleBase + want_);
      cur_ = want_;
    }
    out_ += c;
    atParaStart_ = false;
    pendingSpace_ = false;
  }

 private:
  std::string &out_;
  bool atParaStart_ = true;
  bool pendingSpace_ = false;
  bool center_ = false;
  uint8_t want_ = 0;
  uint8_t cur_ = 0;
};

bool isOneOf(const std::string &name, std::initializer_list<const char *> names) {
  for (const char *n : names) {
    if (name == n) return true;
  }
  return false;
}

bool isBlock(const std::string &name) {
  return isOneOf(name, {"p", "div", "section", "article", "aside", "header", "footer", "nav",
                        "main", "blockquote", "ul", "ol", "dl", "dt", "dd", "table", "tr",
                        "thead", "tbody", "tfoot", "caption", "figure", "figcaption", "pre",
                        "address", "center", "body"});
}

bool isHeading(const std::string &name) {
  return name.size() == 2 && name[0] == 'h' && name[1] >= '1' && name[1] <= '6';
}

// Elements that never have a closing tag, even written without "/>".
bool isVoid(const std::string &name) {
  return isOneOf(name, {"area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta",
                        "param", "source", "track", "wbr"});
}

// ---------------------------------------------------------------------------------------------
// CSS: only display:none

std::string lowerNoSpace(const std::string &s) {
  std::string out;
  out.reserve(s.size());
  for (const char c : s) {
    if (!isspace(static_cast<unsigned char>(c))) {
      out += static_cast<char>(tolower(static_cast<unsigned char>(c)));
    }
  }
  return out;
}

// A declaration block (or a style attribute) that sets display:none.
bool saysDisplayNone(const std::string &declarations) {
  const std::string d = lowerNoSpace(declarations);
  for (size_t at = d.find("display:none"); at != std::string::npos;
       at = d.find("display:none", at + 1)) {
    if (at == 0 || d[at - 1] == ';' || d[at - 1] == '{') return true;  // not "-x-display:"
  }
  return false;
}

bool isIdentChar(char c) {
  return isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' ||
         static_cast<unsigned char>(c) >= 0x80;
}

// One compound selector: "h1.ChapTitle", ".Nonprinting", "*[class*=\"Nonprinting\"]", "#x".
// False for anything else (combinators, pseudo-classes, other attributes).
bool parseSelector(const std::string &raw, HiddenSelector &out) {
  size_t b = 0, e = raw.size();
  while (b < e && isspace(static_cast<unsigned char>(raw[b]))) ++b;
  while (e > b && isspace(static_cast<unsigned char>(raw[e - 1]))) --e;
  const std::string s = raw.substr(b, e - b);
  size_t i = 0;
  const auto ident = [&] {
    const size_t start = i;
    while (i < s.size() && isIdentChar(s[i])) ++i;
    return s.substr(start, i - start);
  };
  if (i < s.size() && s[i] == '*') {
    ++i;
  } else {
    for (char c : ident()) out.tag += static_cast<char>(tolower(static_cast<unsigned char>(c)));
  }
  while (i < s.size()) {
    if (s[i] == '.') {
      ++i;
      out.classes.push_back(ident());
      if (out.classes.back().empty()) return false;
    } else if (s[i] == '#') {
      ++i;
      out.id = ident();
      if (out.id.empty()) return false;
    } else if (s[i] == '[') {
      const size_t close = s.find(']', i);
      if (close == std::string::npos || out.classOp) return false;
      const std::string inner = s.substr(i + 1, close - i - 1);
      i = close + 1;
      const size_t eq = inner.find('=');
      if (eq == std::string::npos || eq == 0) return false;
      char op = '=';
      size_t nameEnd = eq;
      if (strchr("^*~", inner[eq - 1])) {
        op = inner[eq - 1];
        nameEnd = eq - 1;
      }
      if (lowerNoSpace(inner.substr(0, nameEnd)) != "class") return false;
      std::string value = inner.substr(eq + 1);
      size_t vb = 0, ve = value.size();
      while (vb < ve && (isspace(static_cast<unsigned char>(value[vb])) || value[vb] == '"' ||
                         value[vb] == '\'')) {
        ++vb;
      }
      while (ve > vb && (isspace(static_cast<unsigned char>(value[ve - 1])) ||
                         value[ve - 1] == '"' || value[ve - 1] == '\'')) {
        --ve;
      }
      out.classOp = op;
      out.classValue = value.substr(vb, ve - vb);
      if (out.classValue.empty()) return false;
    } else {
      return false;
    }
  }
  if (out.tag == "html" || out.tag == "body") return false;  // never the whole chapter
  return !out.tag.empty() || !out.classes.empty() || !out.id.empty() || out.classOp;
}

// Whether the whitespace-separated list `list` has the word `word`.
bool hasWord(const std::string &list, const std::string &word) {
  for (size_t at = list.find(word); at != std::string::npos; at = list.find(word, at + 1)) {
    const size_t end = at + word.size();
    if ((at == 0 || isspace(static_cast<unsigned char>(list[at - 1]))) &&
        (end == list.size() || isspace(static_cast<unsigned char>(list[end])))) {
      return true;
    }
  }
  return false;
}

bool matches(const HiddenSelector &h, const std::string &tag, const std::string &cls,
             const std::string &id) {
  if (!h.tag.empty() && h.tag != tag) return false;
  if (!h.id.empty() && h.id != id) return false;
  for (const auto &c : h.classes) {
    if (!hasWord(cls, c)) return false;
  }
  switch (h.classOp) {
    case '^': return cls.compare(0, h.classValue.size(), h.classValue) == 0;
    case '*': return cls.find(h.classValue) != std::string::npos;
    case '~': return hasWord(cls, h.classValue);
    case '=': return cls == h.classValue;
    default: return true;
  }
}

}  // namespace

std::vector<HiddenSelector> hiddenSelectors(const std::string &css) {
  std::string s;  // without comments
  s.reserve(css.size());
  for (size_t i = 0; i < css.size();) {
    if (css.compare(i, 2, "/*") == 0) {
      const size_t end = css.find("*/", i + 2);
      i = end == std::string::npos ? css.size() : end + 2;
      s += ' ';
    } else {
      s += css[i++];
    }
  }
  std::vector<HiddenSelector> out;
  size_t i = 0;
  while (i < s.size()) {
    if (isspace(static_cast<unsigned char>(s[i])) || s[i] == '}' || s[i] == ';') {
      ++i;
      continue;
    }
    if (s[i] == '@') {  // @media, @font-face, @page, @import: nothing for us, nested or not
      const size_t semi = s.find(';', i), brace = s.find('{', i);
      if (brace == std::string::npos || (semi != std::string::npos && semi < brace)) {
        i = semi == std::string::npos ? s.size() : semi + 1;
        continue;
      }
      int depth = 0;
      size_t j = brace;
      for (; j < s.size(); ++j) {
        if (s[j] == '{') ++depth;
        if (s[j] == '}' && --depth == 0) break;
      }
      i = j + 1;
      continue;
    }
    const size_t open = s.find('{', i);
    if (open == std::string::npos) break;
    const size_t close = s.find('}', open);
    if (close == std::string::npos) break;
    if (saysDisplayNone(s.substr(open + 1, close - open - 1))) {
      const std::string selectors = s.substr(i, open - i);
      size_t start = 0;
      while (start <= selectors.size()) {
        size_t comma = selectors.find(',', start);
        if (comma == std::string::npos) comma = selectors.size();
        HiddenSelector h;
        if (parseSelector(selectors.substr(start, comma - start), h)) out.push_back(std::move(h));
        start = comma + 1;
      }
    }
    i = close + 1;
  }
  return out;
}

void xhtmlToFlow(const std::string &s, std::string &out, std::vector<Epub::Image> *images,
                 const std::string &baseDir, const std::vector<HiddenSelector> *hidden) {
  out.clear();
  out.reserve(s.size() / 2);
  FlowBuilder fb(out);
  int bold = 0, italic = 0, heading = 0, skip = 0, svg = 0;
  bool svgHadImage = false;
  // Inside an element the book hides: its tag name, and how deep in same-named tags.
  std::string hideName;
  int hideDepth = 0;
  const auto isHidden = [&](const Tag &t) {
    bool flagged = false;
    attr(s, t, "hidden", &flagged);
    if (flagged || saysDisplayNone(attr(s, t, "style"))) return true;
    if (!hidden || hidden->empty()) return false;
    const std::string cls = attr(s, t, "class"), id = attr(s, t, "id");
    for (const HiddenSelector &h : *hidden) {
      if (matches(h, t.name, cls, id)) return true;
    }
    return false;
  };
  auto applyStyle = [&] {
    fb.style((bold > 0 ? flow::kBold : 0) | (italic > 0 ? flow::kItalic : 0));
  };
  auto imagePlaceholder = [&] {
    fb.para();
    fb.center(true);
    fb.style(flow::kItalic);
    fb.text("[image]");
    fb.para();
    fb.center(heading > 0);
    applyStyle();
  };
  auto addImage = [&](const std::string &src, const std::string &alt) {
    if (!images || src.empty() || static_cast<int>(images->size()) >= flow::kMaxImages) {
      imagePlaceholder();
      return;
    }
    images->push_back({resolvePath(baseDir, src), alt});
    fb.image(static_cast<int>(images->size()) - 1);
    fb.center(heading > 0);
  };
  auto bump = [](int &counter, bool open) { counter = open ? counter + 1 : (counter > 0 ? counter - 1 : 0); };

  std::string glyphs;  // one code point as flow bytes
  Tag t;
  const size_t n = s.size();
  size_t i = 0;
  while (i < n) {
    if (s[i] == '<') {
      if (!parseTag(s, i, t)) break;
      i = t.end;
      if (t.special) continue;
      const std::string &name = t.name;
      const bool open = !t.closing;

      if (hideDepth) {  // only its own tag name counts until it closes
        if (name == hideName && !t.selfClosing && !isVoid(name)) hideDepth += open ? 1 : -1;
        continue;
      }
      if (isOneOf(name, {"head", "script", "style", "math"})) {
        if (t.selfClosing) continue;
        bump(skip, open);
        continue;
      }
      if (skip) continue;
      if (open && isHidden(t)) {
        if (!t.selfClosing && !isVoid(name)) {
          hideName = name;
          hideDepth = 1;
        }
        continue;
      }
      // SVG: covers often wrap a bitmap in <svg><image href=...>; draw that, skip the rest.
      if (name == "svg") {
        if (t.selfClosing) continue;
        if (open && svg == 0) svgHadImage = false;
        bump(svg, open);
        if (!open && svg == 0 && !svgHadImage) imagePlaceholder();
        continue;
      }
      if (svg) {
        if (open && name == "image") {
          addImage(attr(s, t, "href"), "");
          svgHadImage = true;
        }
        continue;
      }

      if (isHeading(name)) {
        fb.para();
        if (!t.selfClosing) {
          bump(heading, open);
          bump(bold, open);
        }
        fb.center(heading > 0);
        applyStyle();
      } else if (isOneOf(name, {"b", "strong", "th"})) {
        if (!t.selfClosing) bump(bold, open);
        applyStyle();
        if (name == "th") fb.space();
      } else if (isOneOf(name, {"i", "em", "cite", "var", "dfn"})) {
        if (!t.selfClosing) bump(italic, open);
        applyStyle();
      } else if (name == "br") {
        fb.para();
      } else if (name == "hr") {
        fb.para();
        fb.center(true);
        fb.text("* * *");
        fb.para();
        fb.center(heading > 0);
      } else if (name == "img" || name == "image") {
        if (open) addImage(attr(s, t, name == "img" ? "src" : "href"), attr(s, t, "alt"));
      } else if (name == "li") {
        fb.para();
        if (open && !t.selfClosing) {
          glyphs.clear();
          text::appendSlots(glyphs, 0x2022);  // •
          fb.text(glyphs + " ");
        }
      } else if (name == "td") {
        fb.space();
      } else if (isBlock(name)) {
        fb.para();
      }
      continue;
    }

    if (skip || svg || hideDepth) {
      i = s.find('<', i);
      if (i == std::string::npos) break;
      continue;
    }
    const uint32_t cp = s[i] == '&' ? decodeEntity(s, i) : text::nextCodepoint(s, i);
    if (cp == 0xAD) {
      fb.softHyphen();
      continue;
    }
    glyphs.clear();
    text::appendSlots(glyphs, cp);
    fb.text(glyphs);
  }
  fb.para();
}

// ---------------------------------------------------------------------------------------------
// The OPF

namespace {

// The OPF's path in the zip, from META-INF/container.xml.
bool opfPathOf(const ZipFile &zip, std::string &opfPath, std::string &err) {
  std::string xml;
  if (!zip.read("META-INF/container.xml", xml, err)) return false;
  Tag t;
  for (size_t pos = xml.find('<'); pos != std::string::npos; pos = xml.find('<', t.end)) {
    if (!parseTag(xml, pos, t)) break;
    if (!t.special && !t.closing && t.name == "rootfile") {
      opfPath = attr(xml, t, "full-path");
      break;
    }
  }
  if (opfPath.empty()) {
    err = "no rootfile in container.xml";
    return false;
  }
  return true;
}

std::string lowerAscii(std::string s) {
  s = text::lower(std::move(s));
  return s;
}

// The title and author from the OPF's <metadata> (EpubMeta in epub.h), which comes before its
// manifest and spine.
void parseOpfMeta(const std::string &xml, EpubMeta &out) {
  struct Creator {
    std::string id, name, fileAs, role;
  };
  std::vector<Creator> creators;
  std::string titleId, calibreTitleSort;
  std::map<std::string, std::string> fileAs, role;  // EPUB 3 refinements, by the id refined
  Tag t;
  for (size_t pos = xml.find('<'); pos != std::string::npos; pos = xml.find('<', t.end)) {
    if (!parseTag(xml, pos, t)) break;
    if (t.special) continue;
    if (t.closing) {
      if (t.name == "metadata") break;
      continue;
    }
    if (t.name == "manifest" || t.name == "spine") break;
    if (t.name == "title" && out.title.empty()) {
      out.title = decodeText(elementText(xml, t));
      titleId = attr(xml, t, "id");
    } else if (t.name == "creator") {
      creators.push_back({attr(xml, t, "id"), decodeText(elementText(xml, t)),
                          decodeText(attr(xml, t, "file-as")), lowerAscii(attr(xml, t, "role"))});
    } else if (t.name == "meta") {
      std::string refines = attr(xml, t, "refines");
      if (!refines.empty() && refines[0] == '#') refines.erase(0, 1);
      const std::string property = attr(xml, t, "property");
      if (!refines.empty() && property == "file-as") {
        fileAs[refines] = decodeText(elementText(xml, t));
      } else if (!refines.empty() && property == "role") {
        role[refines] = lowerAscii(decodeText(elementText(xml, t)));
      } else if (attr(xml, t, "name") == "calibre:title_sort") {
        calibreTitleSort = decodeText(attr(xml, t, "content"));
      } else if (property == "calibre:title_sort") {
        calibreTitleSort = decodeText(elementText(xml, t));
      }
    }
  }
  auto refined = [](const std::map<std::string, std::string> &m, const std::string &id) {
    const auto it = id.empty() ? m.end() : m.find(id);
    return it == m.end() ? std::string() : it->second;
  };

  const Creator *author = nullptr;
  for (const Creator &c : creators) {
    const std::string r = c.role.empty() ? refined(role, c.id) : c.role;
    if (r.empty() || r == "aut") {
      author = &c;
      break;
    }
  }
  if (!author && !creators.empty()) author = &creators.front();
  if (author) {
    out.author = author->name;
    out.authorSort = author->fileAs.empty() ? refined(fileAs, author->id) : author->fileAs;
  }
  out.titleSort = refined(fileAs, titleId);
  if (out.titleSort.empty()) out.titleSort = calibreTitleSort;
}

}  // namespace

bool readEpubMeta(const std::string &path, EpubMeta &out, std::string &err) {
  out = {};
  ZipFile zip;
  std::string opfPath, xml;
  if (!zip.open(path, err) || !opfPathOf(zip, opfPath, err) || !zip.read(opfPath, xml, err)) {
    return false;
  }
  parseOpfMeta(xml, out);
  return true;
}

// ---------------------------------------------------------------------------------------------

void Epub::close() {
  zip_.close();
  title_.clear();
  author_.clear();
  cover_.clear();
  spine_.clear();
  toc_.clear();
  css_.clear();
}

bool Epub::open(const std::string &path, std::string &err) {
  close();
  // A book on the onboard flash (the last book, after a reboot): mount it first.
  if (path.rfind(flashfs::kMountPoint, 0) == 0) flashfs::mount();
  if (!zip_.open(path, err)) return false;

  std::string opfPath, xml;
  if (!opfPathOf(zip_, opfPath, err) || !zip_.read(opfPath, xml, err)) return false;
  const size_t slash = opfPath.rfind('/');
  const std::string opfDir = slash == std::string::npos ? "" : opfPath.substr(0, slash + 1);

  // The title and author as the library has them (the first author, not an editor).
  EpubMeta meta;
  parseOpfMeta(xml, meta);
  title_ = meta.title;
  author_ = meta.author;

  struct Item {
    std::string href, mediaType, properties;
  };
  std::map<std::string, Item> manifest;  // id -> item
  std::vector<std::string> idrefs;
  std::string ncxId, coverId;
  Tag t;
  for (size_t pos = xml.find('<'); pos != std::string::npos; pos = xml.find('<', t.end)) {
    if (!parseTag(xml, pos, t)) break;
    if (t.special || t.closing) continue;
    if (t.name == "item") {
      manifest[attr(xml, t, "id")] = {attr(xml, t, "href"), attr(xml, t, "media-type"),
                                      attr(xml, t, "properties")};
    } else if (t.name == "spine") {
      ncxId = attr(xml, t, "toc");
    } else if (t.name == "itemref") {
      idrefs.push_back(attr(xml, t, "idref"));
    } else if (t.name == "meta" && attr(xml, t, "name") == "cover") {
      coverId = attr(xml, t, "content");
    }
  }

  // The cover picture: EPUB 3 marks it (properties="cover-image"); EPUB 2 names its manifest id
  // in <meta name="cover" content=...> (some books put the file there instead); failing both,
  // an image whose id or file name says "cover".
  std::string coverHref;
  for (const auto &kv : manifest) {
    if ((" " + kv.second.properties + " ").find(" cover-image ") != std::string::npos) {
      coverHref = kv.second.href;
    }
  }
  if (coverHref.empty() && !coverId.empty()) {
    const auto it = manifest.find(coverId);
    coverHref = it != manifest.end() ? it->second.href : coverId;
  }
  if (coverHref.empty()) {
    for (const auto &kv : manifest) {
      if (kv.second.mediaType.compare(0, 6, "image/") != 0) continue;
      std::string key = kv.first + " " + kv.second.href;
      key = text::lower(std::move(key));
      if (key.find("cover") != std::string::npos) {
        coverHref = kv.second.href;
        break;
      }
    }
  }
  if (!coverHref.empty()) {
    const std::string entry = resolvePath(opfDir, coverHref);
    if (zip_.find(entry) >= 0) cover_ = entry;
  }

  for (const auto &idref : idrefs) {
    const auto it = manifest.find(idref);
    if (it == manifest.end() || it->second.href.empty()) continue;
    const std::string entry = resolvePath(opfDir, it->second.href);
    const int index = zip_.find(entry);
    if (index < 0) {
      log_w("EPUB: spine item %s missing from zip", entry.c_str());
      continue;
    }
    spine_.push_back({entry, zip_.entries()[index].size});
  }
  if (spine_.empty()) {
    err = "no readable chapters";
    return false;
  }

  // Table of contents: the EPUB 2 NCX if there is one (simpler), else the EPUB 3 nav document.
  std::string ncxHref, navHref;
  if (manifest.count(ncxId)) ncxHref = manifest[ncxId].href;
  for (const auto &kv : manifest) {
    const Item &item = kv.second;
    if (ncxHref.empty() && item.mediaType == "application/x-dtbncx+xml") ncxHref = item.href;
    if (navHref.empty() && (" " + item.properties + " ").find(" nav ") != std::string::npos) {
      navHref = item.href;
    }
  }
  const std::string tocPath = resolvePath(opfDir, ncxHref.empty() ? navHref : ncxHref);
  std::string tocXml, tocErr;
  if (!tocPath.empty() && zip_.read(tocPath, tocXml, tocErr)) {
    const size_t s = tocPath.rfind('/');
    const std::string tocDir = s == std::string::npos ? "" : tocPath.substr(0, s + 1);
    if (ncxHref.empty()) {
      parseNav(tocXml, tocDir);
    } else {
      parseNcx(tocXml, tocDir);
    }
  }

  if (title_.empty()) {
    const size_t base = path.rfind('/');
    title_ = path.substr(base == std::string::npos ? 0 : base + 1);
  }
  log_i("EPUB: \"%s\" by \"%s\", %u spine items, %u TOC entries", title_.c_str(),
        author_.c_str(), static_cast<unsigned>(spine_.size()),
        static_cast<unsigned>(toc_.size()));
  return true;
}

void Epub::addTocEntry(const std::string &title, const std::string &dir,
                       const std::string &href) {
  const std::string path = resolvePath(dir, href);
  for (size_t i = 0; i < spine_.size(); ++i) {
    if (spine_[i].path == path) {
      toc_.push_back({title.empty() ? "Untitled" : title, i});
      return;
    }
  }
}

// <navMap><navPoint><navLabel><text>Title</text></navLabel><content src="ch1.xhtml#x"/>...
// Nested navPoints come in document order, which is reading order.
void Epub::parseNcx(const std::string &xml, const std::string &dir) {
  Tag t;
  bool inMap = false;
  std::string label;
  for (size_t pos = xml.find('<'); pos != std::string::npos; pos = xml.find('<', t.end)) {
    if (!parseTag(xml, pos, t)) break;
    if (t.special) continue;
    if (t.name == "navmap") {
      inMap = !t.closing;  // stop at </navMap>: pageList/navList follow
      if (t.closing) break;
    } else if (inMap && !t.closing && t.name == "text") {
      label = decodeText(elementText(xml, t));
    } else if (inMap && !t.closing && t.name == "content") {
      addTocEntry(label, dir, attr(xml, t, "src"));
      label.clear();
    }
  }
}

// <nav epub:type="toc"><ol><li><a href="ch1.xhtml">Title</a>...</nav>
void Epub::parseNav(const std::string &xml, const std::string &dir) {
  Tag t;
  bool inToc = false;
  for (size_t pos = xml.find('<'); pos != std::string::npos; pos = xml.find('<', t.end)) {
    if (!parseTag(xml, pos, t)) break;
    if (t.special) continue;
    if (t.name == "nav") {
      if (t.closing) {
        if (inToc) break;
      } else {
        inToc = (" " + attr(xml, t, "type") + " ").find(" toc ") != std::string::npos;
      }
    } else if (inToc && !t.closing && t.name == "a") {
      size_t end = xml.find("</a", t.end);
      if (end == std::string::npos) end = xml.size();
      addTocEntry(decodeText(xml.substr(t.end, end - t.end)), dir, attr(xml, t, "href"));
    }
  }
}

std::string Epub::chapterTitle(size_t i) const {
  const TocEntry *best = nullptr;  // first entry of the latest chapter at or before i
  for (const auto &e : toc_) {
    if (e.chapter <= i && (!best || e.chapter > best->chapter)) best = &e;
  }
  return best ? best->title : std::string();
}

uint64_t Epub::sizeBefore(size_t i) const {
  uint64_t total = 0;
  for (size_t k = 0; k < i && k < spine_.size(); ++k) total += spine_[k].size;
  return total;
}

bool Epub::chapterFlow(size_t i, std::string &flow, std::vector<Image> &images,
                       std::string &err) const {
  flow.clear();
  images.clear();
  if (i >= spine_.size()) {
    err = "no such chapter";
    return false;
  }
  std::string xhtml;
  if (!zip_.read(spine_[i].path, xhtml, err)) return false;
  const std::string &path = spine_[i].path;
  const std::string dir = path.substr(0, path.rfind('/') + 1);
  const std::vector<HiddenSelector> hidden = chapterHidden(xhtml, dir);
  xhtmlToFlow(xhtml, flow, &images, dir, &hidden);
  return true;
}

std::vector<HiddenSelector> Epub::chapterHidden(const std::string &xhtml,
                                                const std::string &dir) const {
  std::vector<HiddenSelector> hidden;
  const auto add = [&](const std::vector<HiddenSelector> &rules) {
    hidden.insert(hidden.end(), rules.begin(), rules.end());
  };
  const size_t body = xhtml.find("<body");
  const size_t headEnd = body == std::string::npos ? xhtml.size() : body;
  Tag t;
  for (size_t pos = xhtml.find('<'); pos < headEnd; pos = xhtml.find('<', t.end)) {
    if (!parseTag(xhtml, pos, t)) break;
    if (t.special || t.closing) continue;
    if (t.name == "link") {
      const std::string rel = lowerNoSpace(attr(xhtml, t, "rel"));
      if (rel.find("stylesheet") == std::string::npos || rel.find("alternate") != std::string::npos) {
        continue;
      }
      const std::string css = resolvePath(dir, attr(xhtml, t, "href"));
      auto it = css_.find(css);
      if (it == css_.end()) {
        std::string text, err;
        std::vector<HiddenSelector> rules;
        if (zip_.read(css, text, err)) rules = hiddenSelectors(text);
        log_i("EPUB: %s hides %u selectors", css.c_str(), static_cast<unsigned>(rules.size()));
        it = css_.emplace(css, std::move(rules)).first;
      }
      add(it->second);
    } else if (t.name == "style" && !t.selfClosing) {
      const size_t end = xhtml.find("</style", t.end);
      add(hiddenSelectors(xhtml.substr(t.end, (end == std::string::npos ? headEnd : end) - t.end)));
    }
  }
  return hidden;
}

bool Epub::readFile(const std::string &path, std::string &out, std::string &err) const {
  return zip_.read(path, out, err);
}
