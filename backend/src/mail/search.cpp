// Owner: WP-B
// Gmail-style search: tokenizer + compiler to SQL over messages `m` (search.hpp). Pure.
#include "mail/search.hpp"

#include "core/strings.hpp"
#include "core/time.hpp"
#include "mail/fts.hpp"
#include "mail/internal.hpp"

#include <array>
#include <charconv>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace azm::mail {
namespace {

constexpr std::array<std::string_view, 16> kOperators = {
    "from", "to", "cc", "bcc", "subject", "label", "filename", "in",
    "is", "has", "after", "before", "newer_than", "older_than", "larger", "smaller"};

bool is_operator(std::string_view f) {
  for (auto op : kOperators)
    if (op == f) return true;
  return false;
}

bool space_at(std::string_view s, std::size_t i) { return detail::unicode_space_len(s, i) > 0; }

// A plain term carries searchable content (letters, digits or any non-ASCII character).
bool has_content(std::string_view v) {
  for (unsigned char c : v) {
    if (c >= 0x80) return true;
    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) return true;
  }
  return false;
}

std::optional<int64_t> parse_uint(std::string_view s) {
  if (s.empty() || s.size() > 18) return std::nullopt;
  int64_t v = 0;
  const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
  if (r.ec != std::errc() || r.ptr != s.data() + s.size() || v < 0) return std::nullopt;
  return v;
}

// SQL fragments for the message sets of DESIGN §2 ("normal" = not trashed, not spam).
constexpr std::string_view kNormal = "m.trashed_at IS NULL AND m.is_spam = 0";
constexpr std::string_view kSentCond =
    "m.direction = 'out' AND m.is_draft = 0 AND (m.outbound_id IS NULL OR EXISTS (SELECT 1 FROM "
    "outbound o WHERE o.id = m.outbound_id AND o.status NOT IN ('scheduled','canceled') AND NOT "
    "(o.status IN ('queued','sending') AND o.scheduled_at IS NOT NULL)))";
constexpr std::string_view kScheduledCond =
    "m.direction = 'out' AND m.is_draft = 0 AND EXISTS (SELECT 1 FROM outbound o WHERE o.id = "
    "m.outbound_id AND o.scheduled_at IS NOT NULL AND o.status IN "
    "('queued','sending','accepted','scheduled'))";

struct Expr {
  std::string sql;
  std::vector<db::Value> binds;
};

Expr fts_match(std::string_view column, std::string_view value) {
  std::string q = column.empty() ? fts_quote(value) : std::string(column) + " : " + fts_quote(value);
  return {"m.id IN (SELECT rowid FROM message_fts WHERE message_fts MATCH ?)", {std::move(q)}};
}

// Short terms (1–2 code points; trigram MATCH cannot find them): LIKE on this message's FTS
// row (rowid lookup, so the scan stays within the owner's candidate messages).
Expr fts_like(std::initializer_list<std::string_view> columns, std::string_view value) {
  Expr e;
  e.sql = "EXISTS (SELECT 1 FROM message_fts f WHERE f.rowid = m.id AND (";
  bool first = true;
  const std::string pat = detail::like_contains(value);
  for (auto col : columns) {
    if (!first) e.sql += " OR ";
    first = false;
    e.sql += "f.";
    e.sql += col;
    e.sql += " LIKE ? ESCAPE '\\'";
    e.binds.emplace_back(pat);
  }
  e.sql += "))";
  return e;
}

// Address-list JSON column (cc_json / bcc_json) containing the value in a name or email.
Expr json_addr_like(std::string_view column, std::string_view value) {
  const std::string pat = detail::like_contains(value);
  Expr e;
  e.sql = "EXISTS (SELECT 1 FROM json_each(m." + std::string(column) +
          ") j WHERE json_extract(j.value, '$.email') LIKE ? ESCAPE '\\' OR "
          "json_extract(j.value, '$.name') LIKE ? ESCAPE '\\')";
  e.binds = {pat, pat};
  return e;
}

Expr free_text(std::string_view value) {
  if (utf8_length(value) >= 3) return fts_match("", value);
  return fts_like({"subject", "from_text", "to_text", "body", "attach_names"}, value);
}

struct CompileState {
  SqlFilter* filter;
  int tzoff_min;
  int64_t now_ms;
};

// nullopt → the term is not a valid operator use and falls back to plain text.
std::optional<Expr> compile_operator(const SearchTerm& t, CompileState& st) {
  const std::string& f = t.field;
  const std::string& v = t.value;
  const std::string lv = to_lower_ascii(v);
  const bool long_term = utf8_length(v) >= 3;

  if (f == "from") {
    if (long_term) return fts_match("from_text", v);
    const std::string pat = detail::like_contains(v);
    return Expr{"(m.from_name LIKE ? ESCAPE '\\' OR m.from_email LIKE ? ESCAPE '\\')", {pat, pat}};
  }
  if (f == "to") return long_term ? fts_match("to_text", v) : fts_like({"to_text"}, v);
  if (f == "cc") {
    Expr e = json_addr_like("cc_json", v);
    if (long_term) {  // index prefilter (to_text = to + cc), then the exact cc check
      Expr pre = fts_match("to_text", v);
      pre.sql += " AND " + e.sql;
      for (auto& b : e.binds) pre.binds.push_back(std::move(b));
      pre.sql = "(" + pre.sql + ")";
      return pre;
    }
    return e;
  }
  if (f == "bcc") return json_addr_like("bcc_json", v);  // inbound "bcc:[self]" is not in FTS
  if (f == "subject") {
    if (long_term) return fts_match("subject", v);
    return Expr{"m.subject LIKE ? ESCAPE '\\'", {detail::like_contains(v)}};
  }
  if (f == "filename") {
    if (long_term) return fts_match("attach_names", v);
    return Expr{"EXISTS (SELECT 1 FROM attachments a WHERE a.message_id = m.id AND a.filename LIKE "
                "? ESCAPE '\\')",
                {detail::like_contains(v)}};
  }
  if (f == "label") {
    // Gmail writes spaces in label names as '-': label:my-label matches "my label".
    return Expr{"EXISTS (SELECT 1 FROM message_labels ml JOIN labels l ON l.id = ml.label_id "
                "WHERE ml.message_id = m.id AND l.owner_id = m.owner_id AND (l.name = ? OR "
                "replace(l.name, ' ', '-') = ? COLLATE NOCASE))",
                {v, v}};
  }
  if (f == "in") {
    std::string sql;
    std::optional<Folder> folder;
    bool wide = false;
    if (lv == "inbox") {
      sql = "(m.in_inbox = 1 AND " + std::string(kNormal) + ")";
      folder = Folder::Inbox;
    } else if (lv == "starred") {
      sql = "(m.is_starred = 1 AND " + std::string(kNormal) + ")";
      folder = Folder::Starred;
    } else if (lv == "sent") {
      sql = "(" + std::string(kNormal) + " AND " + std::string(kSentCond) + ")";
      folder = Folder::Sent;
    } else if (lv == "drafts" || lv == "draft") {
      sql = "(m.is_draft = 1 AND " + std::string(kNormal) + ")";
      folder = Folder::Drafts;
    } else if (lv == "scheduled") {
      sql = "(" + std::string(kNormal) + " AND " + std::string(kScheduledCond) + ")";
      folder = Folder::Scheduled;
    } else if (lv == "spam") {
      sql = "(m.is_spam = 1 AND m.trashed_at IS NULL)";
      folder = Folder::Spam;
      wide = true;
    } else if (lv == "trash") {
      sql = "(m.trashed_at IS NOT NULL)";
      folder = Folder::Trash;
      wide = true;
    } else if (lv == "anywhere") {
      sql = "1";
      wide = true;
    } else {
      return std::nullopt;
    }
    if (!t.negated) {
      if (wide) st.filter->include_spam_trash = true;
      st.filter->in_folder = folder;  // last positive in: wins (anywhere → none)
    }
    return Expr{std::move(sql), {}};
  }
  if (f == "is") {
    if (lv == "unread") return Expr{"m.is_read = 0", {}};
    if (lv == "read") return Expr{"m.is_read = 1", {}};
    if (lv == "starred") return Expr{"m.is_starred = 1", {}};
    return std::nullopt;
  }
  if (f == "has") {
    if (lv == "attachment" || lv == "attachments") return Expr{"m.has_attachments = 1", {}};
    return std::nullopt;
  }
  if (f == "after" || f == "before") {
    const auto at = parse_search_date(v, st.tzoff_min);
    if (!at) return std::nullopt;
    return Expr{f == "after" ? "m.date >= ?" : "m.date < ?", {*at}};
  }
  if (f == "newer_than" || f == "older_than") {
    const auto age = parse_relative_age_ms(v);
    if (!age) return std::nullopt;
    return Expr{f == "newer_than" ? "m.date >= ?" : "m.date < ?", {st.now_ms - *age}};
  }
  if (f == "larger" || f == "smaller") {
    const auto size = parse_size(v);
    if (!size) return std::nullopt;
    return Expr{f == "larger" ? "m.size_bytes >= ?" : "m.size_bytes < ?", {*size}};
  }
  return std::nullopt;
}

Expr compile_term(const SearchTerm& t, CompileState& st) {
  Expr e;
  if (t.field.empty()) {
    e = free_text(t.value);
  } else if (auto op = compile_operator(t, st)) {
    e = std::move(*op);
  } else {
    // Invalid operator value (in:foo, after:yesterday): search the literal text instead.
    e = free_text(t.field + ":" + t.value);
  }
  if (t.negated) e.sql = "NOT (" + e.sql + ")";
  return e;
}

}  // namespace

ParsedQuery parse_search(std::string_view q_in) {
  const std::string s = utf8_sanitize(q_in);
  const std::size_t n = s.size();
  ParsedQuery out;
  int next_group = 1;
  bool pending_or = false;

  auto push = [&](SearchTerm t) {
    if (pending_or && !out.terms.empty()) {
      SearchTerm& prev = out.terms.back();
      if (prev.or_group == 0) prev.or_group = next_group++;
      t.or_group = prev.or_group;
    }
    pending_or = false;
    out.terms.push_back(std::move(t));
  };

  std::size_t i = 0;
  while (i < n) {
    if (const std::size_t sp = detail::unicode_space_len(s, i); sp > 0) {
      i += sp;
      continue;
    }
    SearchTerm t;
    if (s[i] == '-' && i + 1 < n && !space_at(s, i + 1)) {
      t.negated = true;
      ++i;
    }
    if (s[i] == '"') {  // "a phrase"
      const std::size_t close = s.find('"', i + 1);
      const std::size_t end = close == std::string::npos ? n : close;
      t.value = s.substr(i + 1, end - i - 1);
      t.phrase = true;
      i = close == std::string::npos ? n : close + 1;
      if (has_content(t.value)) push(std::move(t));
      continue;
    }

    std::string word;
    std::optional<std::string> quoted;  // field:"quoted value"
    while (i < n && !space_at(s, i)) {
      if (s[i] == '"') {
        if (!word.empty() && word.back() == ':') {
          const std::size_t close = s.find('"', i + 1);
          const std::size_t end = close == std::string::npos ? n : close;
          quoted = s.substr(i + 1, end - i - 1);
          i = close == std::string::npos ? n : close + 1;
        }
        break;  // a quote mid-word starts a new token
      }
      word.push_back(s[i]);
      ++i;
    }

    if (!t.negated && !quoted) {
      if (word == "OR") {
        pending_or = !out.terms.empty();
        continue;
      }
      if (word == "AND") continue;
    }

    if (const auto colon = word.find(':'); colon != std::string::npos && colon > 0) {
      const std::string field = to_lower_ascii(word.substr(0, colon));
      if (is_operator(field)) {
        t.field = field;
        if (quoted) {
          t.value = std::move(*quoted);
          t.phrase = true;
        } else {
          t.value = word.substr(colon + 1);
        }
        if (!trim(t.value).empty()) {
          t.value = std::string(trim(t.value));
          push(std::move(t));
        }
        continue;
      }
    }
    t.value = word;
    if (quoted) {
      t.value += *quoted;
      t.phrase = true;
    }
    if (has_content(t.value)) push(std::move(t));
  }
  return out;
}

SqlFilter compile_search(std::string_view q, int tzoff_min, int64_t now_ms) {
  if (now_ms == 0) now_ms = azm::now_ms();
  SqlFilter f;
  const ParsedQuery pq = parse_search(q);
  CompileState st{&f, tzoff_min, now_ms};

  // Compile in input order; OR groups are emitted at their first member's position.
  struct Part {
    int group;
    Expr expr;
  };
  std::vector<Part> parts;
  parts.reserve(pq.terms.size());
  for (const auto& t : pq.terms) parts.push_back({t.or_group, compile_term(t, st)});

  std::vector<std::string> clauses;
  std::vector<db::Value> binds;
  std::vector<bool> used(parts.size(), false);
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (used[i]) continue;
    used[i] = true;
    if (parts[i].group == 0) {
      clauses.push_back(parts[i].expr.sql);
      for (auto& b : parts[i].expr.binds) binds.push_back(std::move(b));
      continue;
    }
    std::string sql = "(" + parts[i].expr.sql;
    for (auto& b : parts[i].expr.binds) binds.push_back(std::move(b));
    for (std::size_t k = i + 1; k < parts.size(); ++k) {
      if (used[k] || parts[k].group != parts[i].group) continue;
      used[k] = true;
      sql += " OR " + parts[k].expr.sql;
      for (auto& b : parts[k].expr.binds) binds.push_back(std::move(b));
    }
    sql += ")";
    clauses.push_back(std::move(sql));
  }

  std::string where;
  if (!f.include_spam_trash) where = std::string(kNormal);
  for (const auto& c : clauses) {
    if (!where.empty()) where += " AND ";
    where += c;
  }
  f.where = where.empty() ? "1" : std::move(where);
  f.binds = std::move(binds);
  return f;
}

std::optional<int64_t> parse_size(std::string_view s) {
  s = trim(s);
  if (s.empty()) return std::nullopt;
  std::size_t digits = 0;
  while (digits < s.size() && s[digits] >= '0' && s[digits] <= '9') ++digits;
  if (digits == 0) return std::nullopt;
  const auto num = parse_uint(s.substr(0, digits));
  if (!num) return std::nullopt;
  const std::string unit = to_lower_ascii(s.substr(digits));
  int64_t mul = 1;
  if (unit.empty() || unit == "b") mul = 1;
  else if (unit == "k" || unit == "kb") mul = 1024;
  else if (unit == "m" || unit == "mb") mul = 1024LL * 1024;
  else if (unit == "g" || unit == "gb") mul = 1024LL * 1024 * 1024;
  else return std::nullopt;
  if (*num > std::numeric_limits<int64_t>::max() / mul) return std::nullopt;
  return *num * mul;
}

std::optional<int64_t> parse_relative_age_ms(std::string_view s) {
  s = trim(s);
  if (s.size() < 2) return std::nullopt;
  const char unit = static_cast<char>(s.back() | 0x20);  // ASCII lowercase
  const auto num = parse_uint(s.substr(0, s.size() - 1));
  if (!num) return std::nullopt;
  constexpr int64_t kDay = 24LL * 3600 * 1000;
  int64_t mul = 0;
  switch (unit) {
    case 'd': mul = kDay; break;
    case 'm': mul = 30 * kDay; break;
    case 'y': mul = 365 * kDay; break;
    default: return std::nullopt;
  }
  if (*num > std::numeric_limits<int64_t>::max() / mul) return std::nullopt;
  return *num * mul;
}

std::optional<int64_t> parse_search_date(std::string_view s, int tzoff_min) {
  s = trim(s);
  char sep = 0;
  for (char c : s) {
    if (c == '/' || c == '-') {
      sep = c;
      break;
    }
  }
  if (sep == 0) return std::nullopt;
  const auto parts = split(s, sep);
  if (parts.size() != 3) return std::nullopt;
  if (parts[0].size() != 4 || parts[1].empty() || parts[1].size() > 2 || parts[2].empty() ||
      parts[2].size() > 2)
    return std::nullopt;
  const auto y = parse_uint(parts[0]);
  const auto mo = parse_uint(parts[1]);
  const auto d = parse_uint(parts[2]);
  if (!y || !mo || !d) return std::nullopt;
  const auto utc = utc_ms(static_cast<int>(*y), static_cast<int>(*mo), static_cast<int>(*d));
  if (!utc) return std::nullopt;
  // Local midnight: local = utc + tzoff  →  utc = local − tzoff.
  return *utc - static_cast<int64_t>(tzoff_min) * 60'000;
}

}  // namespace azm::mail
