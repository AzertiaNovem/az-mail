// Owner: WP-B (internal helpers shared by the mail/*.cpp files; not a contract header)
//
// Small utilities used across the mail domain: JSON <-> address-list columns, UTF-8 code-point
// helpers, LIKE escaping, SQL placeholder lists and the owner's own address set.
#pragma once

#include "core/address.hpp"
#include "db/sqlite.hpp"

#include <boost/json/array.hpp>
#include <boost/json/object.hpp>

#include <cstddef>
#include <cstdint>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace azm::mail::detail {

// ---- JSON TEXT columns ------------------------------------------------------------------------

// Parses a messages.{to,cc,bcc,reply_to}_json column: [{name,email}, …]. Tolerant: malformed
// JSON or entries without a string email are skipped; a missing name becomes "".
std::vector<Address> addresses_from_json(std::string_view json);
// Serializes an address list for those columns ([{"name":…,"email":…}, …]).
std::string addresses_to_json(std::span<const Address> list);
// ["a","b"] → {"a","b"}; non-string entries and malformed JSON are ignored.
std::vector<std::string> strings_from_json(std::string_view json);
// Parsed JSON array, [] for malformed input or a non-array value.
boost::json::array array_from_json(std::string_view json);
// Parsed JSON object, {} for malformed input or a non-object value.
boost::json::object object_from_json(std::string_view json);

// ---- UTF-8 ------------------------------------------------------------------------------------

// The first `max_chars` code points of `s` (s must be valid UTF-8; invalid bytes count as one).
std::string utf8_prefix_chars(std::string_view s, std::size_t max_chars);
// Collapses every whitespace run (ASCII whitespace, U+00A0, U+3000) to one ' ' and trims.
std::string collapse_whitespace(std::string_view s);
// Length in bytes of the Unicode whitespace character at s[i] (ASCII ws, U+00A0, U+3000), else 0.
std::size_t unicode_space_len(std::string_view s, std::size_t i);

// ---- SQL ----------------------------------------------------------------------------------------

// "%<s with \ % _ escaped>%" for LIKE … ESCAPE '\'.
std::string like_contains(std::string_view s);
// JSON array text for `x IN (SELECT value FROM json_each(?))`: one SQL text for any list
// length, so the per-connection prepared-statement cache stays bounded.
std::string json_ids(std::span<const int64_t> ids);
std::string json_strings(std::span<const std::string> values);

// Lowercased emails of `owner`: their own mailbox address(es) plus every alias they belong to.
std::set<std::string> owner_addresses(db::Conn& c, int64_t owner);

// ---- misc ----------------------------------------------------------------------------------

// Lowercased "type/subtype" without parameters when it is a syntactically valid MIME type,
// else "" (callers substitute application/octet-stream).
std::string normalize_mime(std::string_view content_type);

}  // namespace azm::mail::detail
