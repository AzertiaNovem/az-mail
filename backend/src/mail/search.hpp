// Owner: WP-B
//
// Gmail-style search (docs/API.md "Search grammar"; DESIGN §2 "Search queries"). Pure: no DB.
// Grammar: from: to: cc: bcc: subject: label: filename: in:(inbox|sent|drafts|spam|trash|
// starred|scheduled|anywhere) is:(unread|read|starred) has:attachment after: before:
// (YYYY/MM/DD or YYYY-MM-DD at local midnight per tzoff) newer_than: older_than: (Nd, Nm, Ny)
// larger: smaller: (10K, 5M; binary units) "phrases" -negation, implicit AND, OR between two
// bare terms. Unknown operators are treated as plain text. Matching is case-insensitive.
// Text terms ≥ 3 UTF-8 code points use the FTS5 trigram index (m.id IN (SELECT rowid FROM
// message_fts WHERE message_fts MATCH ?), each term fts_quote'd, column filters for
// from/to/cc/bcc/subject/filename); shorter terms use LIKE '%t%' over the FTS columns
// (ESCAPE '\'). Negation → NOT IN / NOT. The default scope excludes spam and trash.
#pragma once

#include "mail/types.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace azm::mail {

struct SearchTerm {
  // "" = free text; otherwise the operator name without ':' ("from", "in", "is", "has",
  // "after", "before", "newer_than", "older_than", "larger", "smaller", "label", …).
  std::string field;
  std::string value;     // unquoted value
  bool negated = false;  // leading '-'
  bool phrase = false;   // value was double-quoted
  int or_group = 0;      // terms sharing a non-zero group are OR-ed together
};

struct ParsedQuery {
  std::vector<SearchTerm> terms;  // in input order
};

// Tokenizes `q` (never throws; unbalanced quotes run to the end).
ParsedQuery parse_search(std::string_view q);

// Compiles to SQL over alias `m` (see SqlFilter in types.hpp). Date operators use
// `tzoff_min` (minutes east of UTC); relative ages use `now_ms`. label: matches the owner's
// label by name via a subquery joining labels on l.owner_id = m.owner_id.
SqlFilter compile_search(std::string_view q, int tzoff_min, int64_t now_ms);

// "10K" → 10240, "5M" → 5242880, "100" → 100; nullopt when invalid.
std::optional<int64_t> parse_size(std::string_view s);
// "3d" / "2m" / "1y" → milliseconds (m = 30 days, y = 365 days); nullopt when invalid.
std::optional<int64_t> parse_relative_age_ms(std::string_view s);
// Local midnight of "YYYY/MM/DD" or "YYYY-MM-DD" in UTC ms for `tzoff_min`; nullopt when invalid.
std::optional<int64_t> parse_search_date(std::string_view s, int tzoff_min);

}  // namespace azm::mail
