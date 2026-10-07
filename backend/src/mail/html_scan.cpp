// Owner: WP-B (internal; see html_scan.hpp)
#include "mail/html_scan.hpp"

#include <algorithm>
#include <array>
#include <utility>

namespace azm::mail::html {
namespace {

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }
bool is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_alnum(char c) { return is_alpha(c) || is_digit(c); }
char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

std::string lower_str(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = lower(c);
  return out;
}

// Named character references (the subset that occurs in real-world mail). Sorted by name for
// binary search; case-sensitive like HTML.
struct Entity {
  std::string_view name;
  uint32_t cp;
};
constexpr std::array kEntities = std::to_array<Entity>({
    {"AElig", 0xC6},    {"Aacute", 0xC1},   {"Acirc", 0xC2},    {"Agrave", 0xC0},
    {"Alpha", 0x391},   {"Aring", 0xC5},    {"Atilde", 0xC3},   {"Auml", 0xC4},
    {"Beta", 0x392},    {"Ccedil", 0xC7},   {"Dagger", 0x2021}, {"Delta", 0x394},
    {"ETH", 0xD0},      {"Eacute", 0xC9},   {"Ecirc", 0xCA},    {"Egrave", 0xC8},
    {"Euml", 0xCB},     {"Gamma", 0x393},   {"Iacute", 0xCD},   {"Icirc", 0xCE},
    {"Igrave", 0xCC},   {"Iuml", 0xCF},     {"Lambda", 0x39B},  {"Ntilde", 0xD1},
    {"OElig", 0x152},   {"Oacute", 0xD3},   {"Ocirc", 0xD4},    {"Ograve", 0xD2},
    {"Omega", 0x3A9},   {"Oslash", 0xD8},   {"Otilde", 0xD5},   {"Ouml", 0xD6},
    {"Pi", 0x3A0},      {"Prime", 0x2033},  {"Scaron", 0x160},  {"Sigma", 0x3A3},
    {"THORN", 0xDE},    {"Theta", 0x398},   {"Uacute", 0xDA},   {"Ucirc", 0xDB},
    {"Ugrave", 0xD9},   {"Uuml", 0xDC},     {"Yacute", 0xDD},   {"Yuml", 0x178},
    {"aacute", 0xE1},   {"acirc", 0xE2},    {"acute", 0xB4},    {"aelig", 0xE6},
    {"agrave", 0xE0},   {"alpha", 0x3B1},   {"amp", 0x26},      {"apos", 0x27},
    {"aring", 0xE5},    {"asymp", 0x2248},  {"atilde", 0xE3},   {"auml", 0xE4},
    {"bdquo", 0x201E},  {"beta", 0x3B2},    {"brvbar", 0xA6},   {"bull", 0x2022},
    {"ccedil", 0xE7},   {"cedil", 0xB8},    {"cent", 0xA2},     {"check", 0x2713},
    {"circ", 0x2C6},    {"copy", 0xA9},     {"curren", 0xA4},   {"dagger", 0x2020},
    {"darr", 0x2193},   {"deg", 0xB0},      {"delta", 0x3B4},   {"divide", 0xF7},
    {"eacute", 0xE9},   {"ecirc", 0xEA},    {"egrave", 0xE8},   {"emsp", 0x2003},
    {"ensp", 0x2002},   {"epsilon", 0x3B5}, {"equiv", 0x2261},  {"eth", 0xF0},
    {"euml", 0xEB},     {"euro", 0x20AC},   {"frac12", 0xBD},   {"frac14", 0xBC},
    {"frac34", 0xBE},   {"gamma", 0x3B3},   {"ge", 0x2265},     {"gt", 0x3E},
    {"harr", 0x2194},   {"hearts", 0x2665}, {"hellip", 0x2026}, {"iacute", 0xED},
    {"icirc", 0xEE},    {"iexcl", 0xA1},    {"igrave", 0xEC},   {"infin", 0x221E},
    {"iquest", 0xBF},   {"iuml", 0xEF},     {"lambda", 0x3BB},  {"laquo", 0xAB},
    {"larr", 0x2190},   {"ldquo", 0x201C},  {"le", 0x2264},     {"lrm", 0x200E},
    {"lsaquo", 0x2039}, {"lsquo", 0x2018},  {"lt", 0x3C},       {"macr", 0xAF},
    {"mdash", 0x2014},  {"micro", 0xB5},    {"middot", 0xB7},   {"minus", 0x2212},
    {"mu", 0x3BC},      {"nbsp", 0xA0},     {"ndash", 0x2013},  {"ne", 0x2260},
    {"not", 0xAC},      {"ntilde", 0xF1},   {"oacute", 0xF3},   {"ocirc", 0xF4},
    {"oelig", 0x153},   {"ograve", 0xF2},   {"omega", 0x3C9},   {"ordf", 0xAA},
    {"ordm", 0xBA},     {"oslash", 0xF8},   {"otilde", 0xF5},   {"ouml", 0xF6},
    {"para", 0xB6},     {"permil", 0x2030}, {"phi", 0x3C6},     {"pi", 0x3C0},
    {"plusmn", 0xB1},   {"pound", 0xA3},    {"prime", 0x2032},  {"quot", 0x22},
    {"raquo", 0xBB},    {"rarr", 0x2192},   {"rdquo", 0x201D},  {"reg", 0xAE},
    {"rlm", 0x200F},    {"rsaquo", 0x203A}, {"rsquo", 0x2019},  {"sbquo", 0x201A},
    {"scaron", 0x161},  {"sect", 0xA7},     {"shy", 0xAD},      {"sigma", 0x3C3},
    {"sup1", 0xB9},     {"sup2", 0xB2},     {"sup3", 0xB3},     {"szlig", 0xDF},
    {"tau", 0x3C4},     {"theta", 0x3B8},   {"thinsp", 0x2009}, {"thorn", 0xFE},
    {"tilde", 0x2DC},   {"times", 0xD7},    {"trade", 0x2122},  {"uacute", 0xFA},
    {"uarr", 0x2191},   {"ucirc", 0xFB},    {"ugrave", 0xF9},   {"uml", 0xA8},
    {"uuml", 0xFC},     {"yacute", 0xFD},   {"yen", 0xA5},      {"yuml", 0xFF},
    {"zwj", 0x200D},    {"zwnj", 0x200C},
});

std::optional<uint32_t> lookup_entity(std::string_view name) {
  const auto it = std::lower_bound(kEntities.begin(), kEntities.end(), name,
                                   [](const Entity& e, std::string_view n) { return e.name < n; });
  if (it != kEntities.end() && it->name == name) return it->cp;
  return std::nullopt;
}

// Legacy references the HTML parser accepts without the trailing ';'.
bool legacy_without_semicolon(std::string_view name) {
  return name == "amp" || name == "lt" || name == "gt" || name == "quot" || name == "nbsp" ||
         name == "copy" || name == "reg";
}

// HTML numeric reference fix-ups for C1 controls (Windows-1252 interpretation).
constexpr std::array<uint32_t, 32> kWin1252 = {
    0x20AC, 0x81,   0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
    0x2039, 0x0152, 0x8D,   0x017D, 0x8F,   0x90,   0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
    0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x9D,   0x017E, 0x0178};

uint32_t fix_numeric(uint64_t v) {
  if (v == 0 || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) return 0xFFFD;
  if (v >= 0x80 && v <= 0x9F) return kWin1252[v - 0x80];
  return static_cast<uint32_t>(v);
}

}  // namespace

const Attr* Tag::find(std::string_view attr_name) const {
  for (const auto& a : attrs)
    if (a.name == attr_name) return &a;
  return nullptr;
}

bool is_raw_text_element(std::string_view name) {
  return name == "script" || name == "style" || name == "textarea" || name == "title" ||
         name == "xmp" || name == "iframe" || name == "noembed" || name == "noframes" ||
         name == "noscript" || name == "template" || name == "plaintext";
}

std::size_t find_close_tag(std::string_view html, std::size_t pos, std::string_view name) {
  const std::size_t n = html.size();
  while (pos < n) {
    const std::size_t lt = html.find("</", pos);
    if (lt == std::string_view::npos) return std::string_view::npos;
    const std::size_t after = lt + 2 + name.size();
    if (after <= n) {
      bool match = true;
      for (std::size_t k = 0; k < name.size(); ++k) {
        if (lower(html[lt + 2 + k]) != name[k]) {
          match = false;
          break;
        }
      }
      if (match && (after == n || is_space(html[after]) || html[after] == '/' || html[after] == '>'))
        return lt;
    }
    pos = lt + 2;
  }
  return std::string_view::npos;
}

std::optional<Markup> parse_markup(std::string_view html, std::size_t pos) {
  const std::size_t n = html.size();
  if (pos + 1 >= n || html[pos] != '<') return std::nullopt;
  const char c = html[pos + 1];
  Markup m;
  auto until_gt = [&](std::size_t from) {
    const std::size_t e = html.find('>', from);
    return e == std::string_view::npos ? n : e + 1;
  };
  if (c == '!') {
    if (html.substr(pos, 4) == "<!--") {
      const std::size_t e = html.find("-->", pos + 4);
      m.kind = MarkupKind::Comment;
      m.end = e == std::string_view::npos ? n : e + 3;
      return m;
    }
    if (html.substr(pos, 9) == "<![CDATA[") {
      const std::size_t e = html.find("]]>", pos + 9);
      m.kind = MarkupKind::Other;
      m.end = e == std::string_view::npos ? n : e + 3;
      return m;
    }
    m.kind = MarkupKind::Other;
    m.end = until_gt(pos + 2);
    return m;
  }
  if (c == '?') {
    m.kind = MarkupKind::Other;
    m.end = until_gt(pos + 2);
    return m;
  }

  Tag& t = m.tag;
  t.begin = pos;
  std::size_t i = pos + 1;
  if (c == '/') {
    t.closing = true;
    ++i;
    if (i >= n || !is_alpha(html[i])) {  // "</>" or "</ x": bogus comment up to '>'
      m.kind = MarkupKind::Other;
      m.end = until_gt(i);
      return m;
    }
  } else if (!is_alpha(c)) {
    return std::nullopt;
  }

  const std::size_t name_begin = i;
  while (i < n && !is_space(html[i]) && html[i] != '/' && html[i] != '>') ++i;
  t.name = lower_str(html.substr(name_begin, i - name_begin));

  for (;;) {
    while (i < n && is_space(html[i])) ++i;
    if (i >= n) {
      t.complete = false;
      t.close_at = n;
      t.end = n;
      break;
    }
    if (html[i] == '>') {
      t.close_at = i;
      t.end = i + 1;
      break;
    }
    if (html[i] == '/') {
      if (i + 1 < n && html[i + 1] == '>') {
        t.self_closing = true;
        t.close_at = i;
        t.end = i + 2;
        break;
      }
      ++i;
      continue;
    }
    Attr a;
    a.begin = i;
    const std::size_t ab = i;
    while (i < n && !is_space(html[i]) && html[i] != '/' && html[i] != '>' &&
           (html[i] != '=' || i == ab))
      ++i;
    a.name = lower_str(html.substr(ab, i - ab));
    a.end = i;
    std::size_t j = i;
    while (j < n && is_space(html[j])) ++j;
    if (j < n && html[j] == '=') {
      ++j;
      while (j < n && is_space(html[j])) ++j;
      a.has_value = true;
      if (j < n && (html[j] == '"' || html[j] == '\'')) {
        a.quote = html[j];
        a.value_begin = j + 1;
        const std::size_t close = html.find(a.quote, a.value_begin);
        if (close == std::string_view::npos) {
          a.value_end = n;
          a.end = n;
          i = n;
        } else {
          a.value_end = close;
          a.end = close + 1;
          i = close + 1;
        }
      } else {
        a.value_begin = j;
        while (j < n && !is_space(html[j]) && html[j] != '>') ++j;
        a.value_end = j;
        a.end = j;
        i = j;
      }
    }
    if (!t.closing) t.attrs.push_back(std::move(a));
  }
  m.kind = MarkupKind::Tag;
  m.end = t.end;
  return m;
}

void append_utf8(std::string& out, uint32_t cp) {
  if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

std::string decode_entities(std::string_view s) {
  if (s.find('&') == std::string_view::npos) return std::string(s);
  std::string out;
  out.reserve(s.size());
  const std::size_t n = s.size();
  std::size_t i = 0;
  while (i < n) {
    if (s[i] != '&') {
      out.push_back(s[i++]);
      continue;
    }
    if (i + 1 < n && s[i + 1] == '#') {
      std::size_t j = i + 2;
      const bool hex = j < n && (s[j] == 'x' || s[j] == 'X');
      if (hex) ++j;
      const std::size_t digits_begin = j;
      uint64_t v = 0;
      while (j < n && (hex ? (is_digit(s[j]) || (lower(s[j]) >= 'a' && lower(s[j]) <= 'f'))
                           : is_digit(s[j]))) {
        const char d = lower(s[j]);
        const uint64_t dv = is_digit(d) ? static_cast<uint64_t>(d - '0')
                                        : static_cast<uint64_t>(d - 'a' + 10);
        if (v <= 0x10FFFF) v = v * (hex ? 16 : 10) + dv;  // saturates past the valid range
        ++j;
      }
      if (j == digits_begin) {  // "&#" / "&#x" without digits: literal
        out.push_back('&');
        ++i;
        continue;
      }
      if (j < n && s[j] == ';') ++j;
      append_utf8(out, fix_numeric(v));
      i = j;
      continue;
    }
    std::size_t j = i + 1;
    while (j < n && j - i <= 32 && is_alnum(s[j])) ++j;
    const std::string_view name = s.substr(i + 1, j - i - 1);
    if (!name.empty()) {
      if (j < n && s[j] == ';') {
        if (auto cp = lookup_entity(name)) {
          append_utf8(out, *cp);
          i = j + 1;
          continue;
        }
      } else if (legacy_without_semicolon(name)) {
        append_utf8(out, *lookup_entity(name));
        i = j;
        continue;
      }
    }
    out.push_back('&');
    ++i;
  }
  return out;
}

std::string escape_attr(std::string_view v) {
  std::string out;
  out.reserve(v.size() + 8);
  for (char c : v) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      default: out.push_back(c);
    }
  }
  return out;
}

}  // namespace azm::mail::html
