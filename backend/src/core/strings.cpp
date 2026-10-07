#include "core/strings.hpp"

#include <cstdint>

namespace azm {
namespace {

constexpr char kHexUpper[] = "0123456789ABCDEF";

bool is_space(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}
char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }
char upper(char c) { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c; }

int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool is_unreserved(unsigned char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
         c == '.' || c == '_' || c == '~';
}

// RFC 5987 attr-char.
bool is_attr_char(unsigned char c) {
  if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) return true;
  switch (c) {
    case '!': case '#': case '$': case '&': case '+': case '-': case '.': case '^': case '_':
    case '`': case '|': case '~':
      return true;
    default:
      return false;
  }
}

void pct(std::string& out, unsigned char c) {
  out.push_back('%');
  out.push_back(kHexUpper[c >> 4]);
  out.push_back(kHexUpper[c & 0xf]);
}

// Decodes one UTF-8 sequence at s[i]. On success returns its length (1-4) and sets cp.
// On failure returns -(length of the maximal invalid subpart), at least 1.
int decode_utf8(std::string_view s, std::size_t i, uint32_t& cp) {
  const auto b0 = static_cast<unsigned char>(s[i]);
  if (b0 < 0x80) {
    cp = b0;
    return 1;
  }
  int len = 0;
  unsigned char lo = 0x80, hi = 0xBF;
  if (b0 >= 0xC2 && b0 <= 0xDF) {
    len = 2;
    cp = b0 & 0x1F;
  } else if (b0 >= 0xE0 && b0 <= 0xEF) {
    len = 3;
    cp = b0 & 0x0F;
    if (b0 == 0xE0) lo = 0xA0;  // no overlongs
    if (b0 == 0xED) hi = 0x9F;  // no surrogates
  } else if (b0 >= 0xF0 && b0 <= 0xF4) {
    len = 4;
    cp = b0 & 0x07;
    if (b0 == 0xF0) lo = 0x90;  // no overlongs
    if (b0 == 0xF4) hi = 0x8F;  // <= U+10FFFF
  } else {
    return -1;
  }
  for (int k = 1; k < len; ++k) {
    if (i + static_cast<std::size_t>(k) >= s.size()) return -k;
    const auto b = static_cast<unsigned char>(s[i + static_cast<std::size_t>(k)]);
    const unsigned char l = k == 1 ? lo : 0x80, h = k == 1 ? hi : 0xBF;
    if (b < l || b > h) return -k;
    cp = (cp << 6) | (b & 0x3F);
  }
  return len;
}

void append_utf8(std::string& out, uint32_t cp) {
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

}  // namespace

// ---- ASCII ---------------------------------------------------------------------------------

std::string_view trim(std::string_view s) {
  while (!s.empty() && is_space(s.front())) s.remove_prefix(1);
  while (!s.empty() && is_space(s.back())) s.remove_suffix(1);
  return s;
}

std::string to_lower_ascii(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = lower(c);
  return out;
}

std::string to_upper_ascii(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = upper(c);
  return out;
}

bool iequals(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i)
    if (lower(a[i]) != lower(b[i])) return false;
  return true;
}

bool istarts_with(std::string_view s, std::string_view prefix) {
  return s.size() >= prefix.size() && iequals(s.substr(0, prefix.size()), prefix);
}

bool iends_with(std::string_view s, std::string_view suffix) {
  return s.size() >= suffix.size() && iequals(s.substr(s.size() - suffix.size()), suffix);
}

bool is_ascii(std::string_view s) {
  for (unsigned char c : s)
    if (c >= 0x80) return false;
  return true;
}

// ---- split / join / replace ------------------------------------------------------------------

std::vector<std::string> split(std::string_view s, char sep, bool skip_empty) {
  std::vector<std::string> out;
  std::size_t start = 0;
  while (true) {
    const auto pos = s.find(sep, start);
    const std::string_view piece =
        s.substr(start, pos == std::string_view::npos ? std::string_view::npos : pos - start);
    if (!(skip_empty && piece.empty())) out.emplace_back(piece);
    if (pos == std::string_view::npos) break;
    start = pos + 1;
  }
  return out;
}

std::string replace_all(std::string_view s, std::string_view from, std::string_view to) {
  if (from.empty()) return std::string(s);
  std::string out;
  out.reserve(s.size());
  std::size_t start = 0;
  while (true) {
    const auto pos = s.find(from, start);
    if (pos == std::string_view::npos) {
      out.append(s.substr(start));
      break;
    }
    out.append(s.substr(start, pos - start));
    out.append(to);
    start = pos + from.size();
  }
  return out;
}

// ---- UTF-8 ---------------------------------------------------------------------------------

bool utf8_valid(std::string_view s) {
  uint32_t cp = 0;
  for (std::size_t i = 0; i < s.size();) {
    const int n = decode_utf8(s, i, cp);
    if (n < 0) return false;
    i += static_cast<std::size_t>(n);
  }
  return true;
}

std::string utf8_sanitize(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  uint32_t cp = 0;
  for (std::size_t i = 0; i < s.size();) {
    const int n = decode_utf8(s, i, cp);
    if (n > 0) {
      out.append(s.substr(i, static_cast<std::size_t>(n)));
      i += static_cast<std::size_t>(n);
    } else {
      append_utf8(out, 0xFFFD);
      i += static_cast<std::size_t>(-n);
    }
  }
  return out;
}

std::string utf8_truncate(std::string_view s, std::size_t max_bytes) {
  if (s.size() <= max_bytes) return std::string(s);
  std::size_t cut = max_bytes;
  // s[cut] is the first excluded byte; if it is a continuation byte the code point it belongs
  // to started before `cut`, so back up to that lead byte (at most 3 steps).
  for (int k = 0; k < 3 && cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80; ++k)
    --cut;
  if ((static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) cut = max_bytes;  // invalid input
  return std::string(s.substr(0, cut));
}

std::size_t utf8_length(std::string_view s) {
  std::size_t n = 0;
  uint32_t cp = 0;
  for (std::size_t i = 0; i < s.size(); ++n) {
    const int k = decode_utf8(s, i, cp);
    i += static_cast<std::size_t>(k > 0 ? k : 1);
  }
  return n;
}

// ---- percent-encoding ------------------------------------------------------------------------

std::string url_encode(std::string_view s) {
  std::string out;
  out.reserve(s.size() * 3);
  for (unsigned char c : s) {
    if (is_unreserved(c)) out.push_back(static_cast<char>(c));
    else pct(out, c);
  }
  return out;
}

std::optional<std::string> url_decode(std::string_view s, bool plus_as_space) {
  std::string out;
  out.reserve(s.size());
  for (std::size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    if (c == '%') {
      if (i + 2 >= s.size()) return std::nullopt;
      const int hi = hex_value(s[i + 1]), lo = hex_value(s[i + 2]);
      if (hi < 0 || lo < 0) return std::nullopt;
      out.push_back(static_cast<char>((hi << 4) | lo));
      i += 2;
    } else if (c == '+' && plus_as_space) {
      out.push_back(' ');
    } else {
      out.push_back(c);
    }
  }
  return out;
}

std::string rfc5987_encode(std::string_view s) {
  std::string out = "UTF-8''";
  out.reserve(out.size() + s.size() * 3);
  for (unsigned char c : s) {
    if (is_attr_char(c)) out.push_back(static_cast<char>(c));
    else pct(out, c);
  }
  return out;
}

std::string content_disposition(std::string_view disposition, std::string_view filename) {
  std::string disp = to_lower_ascii(trim(disposition));
  if (disp != "inline" && disp != "attachment") disp = "attachment";

  // Sanitize: valid UTF-8, no control characters or path separators, bounded length.
  std::string name;
  {
    const std::string clean = utf8_sanitize(filename);
    name.reserve(clean.size());
    for (unsigned char c : clean) {
      if (c < 0x20 || c == 0x7f) continue;
      name.push_back(c == '/' || c == '\\' ? '_' : static_cast<char>(c));
    }
    name = std::string(trim(name));
    name = utf8_truncate(name, 255);
    if (name.empty()) name = "file";
  }

  // ASCII fallback: one '_' per non-ASCII code point; quotes and '%' replaced too.
  std::string fallback;
  fallback.reserve(name.size());
  uint32_t cp = 0;
  for (std::size_t i = 0; i < name.size();) {
    const int n = decode_utf8(name, i, cp);
    const std::size_t step = static_cast<std::size_t>(n > 0 ? n : 1);
    if (n == 1 && cp >= 0x20 && cp < 0x7f && cp != '"' && cp != '%' && cp != '\\')
      fallback.push_back(static_cast<char>(cp));
    else
      fallback.push_back('_');
    i += step;
  }

  std::string out = disp;
  out += "; filename=\"";
  out += fallback;
  out += '"';
  if (fallback != name) {
    out += "; filename*=";
    out += rfc5987_encode(name);
  }
  return out;
}

std::string html_escape(std::string_view s) {
  std::string out;
  out.reserve(s.size() + s.size() / 8);
  for (char c : s) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default: out.push_back(c);
    }
  }
  return out;
}

}  // namespace azm
