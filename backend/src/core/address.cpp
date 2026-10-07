#include "core/address.hpp"

#include "core/strings.hpp"

namespace azm {
namespace {

bool is_ws(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

// RFC 5322 atext (ASCII part); bytes >= 0x80 are accepted separately (RFC 6532).
bool is_atext(unsigned char c) {
  if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return true;
  switch (c) {
    case '!': case '#': case '$': case '%': case '&': case '\'': case '*': case '+': case '-':
    case '/': case '=': case '?': case '^': case '_': case '`': case '{': case '|': case '}':
    case '~':
      return true;
    default:
      return false;
  }
}

// Collapse whitespace/control runs into single spaces and trim.
std::string clean_display(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  bool pending_space = false;
  for (unsigned char c : s) {
    if (c < 0x20 || c == 0x7f || c == ' ') {
      pending_space = !out.empty();
      continue;
    }
    if (pending_space) out.push_back(' ');
    pending_space = false;
    out.push_back(static_cast<char>(c));
  }
  return out;
}

struct Word {
  std::string text;  // unquoted / unescaped
  std::string raw;   // as written (quotes and escapes kept)
};

struct Entry {
  std::vector<Word> words;
  std::vector<std::string> comments;
  std::optional<std::string> angle;  // raw content inside <...>
  bool word_open = false;

  Word& word() {
    if (!word_open) {
      words.emplace_back();
      word_open = true;
    }
    return words.back();
  }
  void close_word() { word_open = false; }
  bool empty() const { return words.empty() && !angle && comments.empty(); }
  bool has_at() const {
    for (const auto& w : words)
      if (w.raw.find('@') != std::string::npos) return true;
    return false;
  }
};

// Angle content: drop whitespace outside quotes, "mailto:" and obsolete source routes.
std::string clean_angle(std::string_view raw) {
  std::string out;
  out.reserve(raw.size());
  bool in_q = false;
  for (std::size_t i = 0; i < raw.size(); ++i) {
    const char c = raw[i];
    if (in_q) {
      out.push_back(c);
      if (c == '\\' && i + 1 < raw.size()) {
        out.push_back(raw[++i]);
      } else if (c == '"') {
        in_q = false;
      }
      continue;
    }
    if (c == '"') {
      in_q = true;
      out.push_back(c);
    } else if (!is_ws(c)) {
      out.push_back(c);
    }
  }
  if (out.size() >= 7 && iequals(std::string_view(out).substr(0, 7), "mailto:")) out.erase(0, 7);
  if (!out.empty() && out[0] == '@') {  // obs-route: "@a,@b:user@host"
    const auto colon = out.find(':');
    if (colon != std::string::npos) out.erase(0, colon + 1);
  }
  return out;
}

std::string join_texts(const std::vector<Word>& words, std::size_t skip = static_cast<std::size_t>(-1)) {
  std::string out;
  for (std::size_t i = 0; i < words.size(); ++i) {
    if (i == skip || words[i].text.empty()) continue;
    if (!out.empty()) out.push_back(' ');
    out += words[i].text;
  }
  return out;
}

std::optional<Address> build(const Entry& e) {
  Address a;
  if (e.angle) {
    a.email = clean_angle(*e.angle);
    a.name = join_texts(e.words);
  } else {
    // Bare addr-spec, possibly with a stray phrase ("John a@b.com") or obsolete spacing.
    std::size_t idx = static_cast<std::size_t>(-1);
    for (std::size_t i = 0; i < e.words.size(); ++i)
      if (e.words[i].raw.find('@') != std::string::npos) idx = i;
    if (idx != static_cast<std::size_t>(-1) && is_valid_email(e.words[idx].raw)) {
      a.email = e.words[idx].raw;
      a.name = join_texts(e.words, idx);
    } else {
      std::string concat;
      for (const auto& w : e.words) concat += w.raw;
      a.email = std::move(concat);
    }
  }
  if (!is_valid_email(a.email)) return std::nullopt;

  if (a.name.empty() && !e.comments.empty()) {
    std::string c;
    for (const auto& s : e.comments) {
      if (s.empty()) continue;
      if (!c.empty()) c.push_back(' ');
      c += s;
    }
    a.name = std::move(c);
  }
  a.name = clean_display(a.name);
  if (a.name.size() >= 2 && a.name.front() == '\'' && a.name.back() == '\'')
    a.name = clean_display(std::string_view(a.name).substr(1, a.name.size() - 2));
  if (iequals(a.name, a.email)) a.name.clear();
  return a;
}

}  // namespace

std::vector<Address> parse_address_list(std::string_view s) {
  std::vector<Address> out;
  Entry cur;
  bool in_group = false;

  auto flush = [&] {
    if (!cur.empty())
      if (auto a = build(cur)) out.push_back(std::move(*a));
    cur = Entry{};
  };

  const std::size_t n = s.size();
  std::size_t i = 0;
  while (i < n) {
    const char c = s[i];
    if (c == '"') {  // quoted-string (part of the current word if adjacent)
      Word& w = cur.word();
      w.raw.push_back('"');
      ++i;
      while (i < n && s[i] != '"') {
        if (s[i] == '\\' && i + 1 < n) {
          w.raw.push_back('\\');
          w.raw.push_back(s[i + 1]);
          w.text.push_back(s[i + 1]);
          i += 2;
          continue;
        }
        w.raw.push_back(s[i]);
        w.text.push_back(s[i]);
        ++i;
      }
      w.raw.push_back('"');
      if (i < n) ++i;  // closing quote (tolerate unterminated)
    } else if (c == '(') {  // comment, nested, with quoted-pairs
      std::string text;
      int depth = 1;
      ++i;
      while (i < n) {
        const char d = s[i];
        if (d == '\\' && i + 1 < n) {
          text.push_back(s[i + 1]);
          i += 2;
          continue;
        }
        ++i;
        if (d == '(') {
          ++depth;
        } else if (d == ')' && --depth == 0) {
          break;
        }
        text.push_back(d);
      }
      cur.comments.push_back(std::string(trim(text)));
      cur.close_word();
    } else if (c == '<') {  // angle-addr (quotes inside respected)
      std::string raw;
      ++i;
      while (i < n && s[i] != '>') {
        if (s[i] == '"') {
          raw.push_back('"');
          ++i;
          while (i < n && s[i] != '"') {
            if (s[i] == '\\' && i + 1 < n) {
              raw.push_back(s[i]);
              raw.push_back(s[i + 1]);
              i += 2;
              continue;
            }
            raw.push_back(s[i++]);
          }
          if (i < n) {
            raw.push_back('"');
            ++i;
          }
          continue;
        }
        raw.push_back(s[i++]);
      }
      if (i < n) ++i;  // '>'
      if (!cur.angle) cur.angle = std::move(raw);
      cur.close_word();
    } else if (c == ',') {
      flush();
      ++i;
    } else if (c == ';') {
      flush();
      in_group = false;
      ++i;
    } else if (c == ':' && !in_group && !cur.angle && !cur.has_at()) {
      cur = Entry{};  // "group-name:" — the group's display name is dropped
      in_group = true;
      ++i;
    } else if (is_ws(c)) {
      cur.close_word();
      ++i;
    } else {
      Word& w = cur.word();
      w.raw.push_back(c);
      w.text.push_back(c);
      ++i;
    }
  }
  flush();
  return out;
}

std::optional<Address> parse_address(std::string_view s) {
  auto list = parse_address_list(s);
  if (list.size() != 1) return std::nullopt;
  return std::move(list.front());
}

std::string format_address(const Address& a) {
  std::string email;
  email.reserve(a.email.size());
  for (unsigned char c : trim(a.email))
    if (c >= 0x20 && c != 0x7f && c != '<' && c != '>') email.push_back(static_cast<char>(c));

  const std::string name = clean_display(a.name);
  if (name.empty()) return email;

  bool needs_quote = false;
  for (unsigned char c : name) {
    if (c == ' ' || c >= 0x80 || is_atext(c)) continue;
    needs_quote = true;
    break;
  }
  std::string out;
  out.reserve(name.size() + email.size() + 6);
  if (needs_quote) {
    out.push_back('"');
    for (char c : name) {
      if (c == '\\' || c == '"') out.push_back('\\');
      out.push_back(c);
    }
    out.push_back('"');
  } else {
    out += name;
  }
  out += " <";
  out += email;
  out += '>';
  return out;
}

std::string format_address_list(std::span<const Address> list) {
  std::string out;
  for (const auto& a : list) {
    if (!out.empty()) out += ", ";
    out += format_address(a);
  }
  return out;
}

std::string normalize_email(std::string_view email, bool strip_plus_tag) {
  std::string out = to_lower_ascii(trim(email));
  if (strip_plus_tag) {
    const auto at = out.rfind('@');
    if (at != std::string::npos) {
      const auto plus = out.find('+');
      if (plus != std::string::npos && plus > 0 && plus < at) out.erase(plus, at - plus);
    }
  }
  return out;
}

std::string_view local_part(std::string_view email) {
  const auto at = email.rfind('@');
  return at == std::string_view::npos ? email : email.substr(0, at);
}

std::string_view domain_of(std::string_view email) {
  const auto at = email.rfind('@');
  return at == std::string_view::npos ? std::string_view{} : email.substr(at + 1);
}

bool is_valid_email(std::string_view email) {
  if (email.empty() || email.size() > 254) return false;
  const auto at = email.rfind('@');
  if (at == std::string_view::npos || at == 0 || at + 1 >= email.size()) return false;
  const std::string_view local = email.substr(0, at);
  const std::string_view domain = email.substr(at + 1);

  // Local part.
  if (local.size() > 64) return false;
  if (local.size() >= 2 && local.front() == '"' && local.back() == '"') {
    for (std::size_t i = 1; i + 1 < local.size(); ++i) {
      const unsigned char c = static_cast<unsigned char>(local[i]);
      if (c < 0x20 || c == 0x7f) return false;
      if (c == '\\') {
        if (i + 2 >= local.size()) return false;
        ++i;
      } else if (c == '"') {
        return false;
      }
    }
  } else {
    if (local.front() == '.' || local.back() == '.') return false;
    char prev = 0;
    for (char ch : local) {
      const unsigned char c = static_cast<unsigned char>(ch);
      if (c == '.') {
        if (prev == '.') return false;
      } else if (!(c >= 0x80 || is_atext(c))) {
        return false;
      }
      prev = ch;
    }
  }

  // Domain: LDH (or UTF-8) labels, at least two, no empty labels.
  if (domain.size() > 253) return false;
  std::size_t labels = 0, start = 0;
  while (start <= domain.size()) {
    auto dot = domain.find('.', start);
    if (dot == std::string_view::npos) dot = domain.size();
    const std::string_view label = domain.substr(start, dot - start);
    if (label.empty() || label.size() > 63) return false;
    if (label.front() == '-' || label.back() == '-') return false;
    for (char ch : label) {
      const unsigned char c = static_cast<unsigned char>(ch);
      const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                      c == '-' || c >= 0x80;
      if (!ok) return false;
    }
    ++labels;
    start = dot + 1;
  }
  return labels >= 2;
}

}  // namespace azm
