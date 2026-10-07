// String utilities: ASCII case helpers, splitting/joining, UTF-8 validation/truncation,
// percent-encoding (RFC 3986), RFC 5987/6266 Content-Disposition, HTML escaping.
#pragma once

#include <cstddef>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

namespace azm {

// ---- ASCII ---------------------------------------------------------------------------------
std::string_view trim(std::string_view s);  // ASCII whitespace (space, \t, \r, \n, \f, \v)
std::string to_lower_ascii(std::string_view s);
std::string to_upper_ascii(std::string_view s);
bool iequals(std::string_view a, std::string_view b);
bool istarts_with(std::string_view s, std::string_view prefix);
bool iends_with(std::string_view s, std::string_view suffix);
bool is_ascii(std::string_view s);

// ---- split / join / replace ------------------------------------------------------------------
// split("a,,b", ',') → {"a","","b"}; with skip_empty → {"a","b"}. Pieces are not trimmed.
std::vector<std::string> split(std::string_view s, char sep, bool skip_empty = false);

template <std::ranges::input_range R>
std::string join(const R& parts, std::string_view sep) {
  std::string out;
  bool first = true;
  for (const auto& p : parts) {
    if (!first) out.append(sep);
    first = false;
    out.append(std::string_view(p));
  }
  return out;
}

std::string replace_all(std::string_view s, std::string_view from, std::string_view to);

// ---- UTF-8 ---------------------------------------------------------------------------------
bool utf8_valid(std::string_view s);  // strict: no overlongs, surrogates or > U+10FFFF
// Replaces each maximal invalid subsequence with U+FFFD (WHATWG / Unicode "best practice").
std::string utf8_sanitize(std::string_view s);
// Longest prefix of at most max_bytes that does not split a code point.
std::string utf8_truncate(std::string_view s, std::size_t max_bytes);
std::size_t utf8_length(std::string_view s);  // code points (invalid bytes count as one each)

// ---- percent-encoding ------------------------------------------------------------------------
std::string url_encode(std::string_view s);  // keeps RFC 3986 unreserved: A-Z a-z 0-9 - . _ ~
// Decodes %XX (case-insensitive); '+' → ' ' when plus_as_space. nullopt on malformed escapes.
std::optional<std::string> url_decode(std::string_view s, bool plus_as_space = false);

// "UTF-8''%E6%8A%A5%E5%91%8A.pdf" (RFC 5987 ext-value; attr-chars kept).
std::string rfc5987_encode(std::string_view s);

// RFC 6266 header value: 'attachment; filename="ascii-fallback"; filename*=UTF-8''...'.
// filename* is added when the fallback differs from the (sanitized) name. Control chars,
// quotes, backslashes and path separators are neutralised; empty names become "file".
std::string content_disposition(std::string_view disposition, std::string_view filename);

// & < > " ' → entities.
std::string html_escape(std::string_view s);

}  // namespace azm
