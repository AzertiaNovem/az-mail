// RFC 5322-ish mailbox parsing and formatting (shared by inbound parsing, send freeze and UI).
// Display names are UTF-8; RFC 2047 encoding is left to Resend (DESIGN C13).
#pragma once

#include <span>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace azm {

struct Address {
  std::string name;   // display name, unquoted/unescaped ("" when absent)
  std::string email;  // addr-spec as written (case preserved)
  bool operator==(const Address&) const = default;
};

// Exactly one mailbox ("Name <a@b>", "a@b", "a@b (Name)"); nullopt if invalid or not exactly one.
std::optional<Address> parse_address(std::string_view s);

// Address list: quoted display names (with \" escapes), commas/semicolons inside quotes,
// comments in parens, bare and angle addresses, UTF-8 names, groups ("team: a@b, c@d;") are
// flattened. Entries without a valid email are skipped. Both ',' and ';' separate entries.
std::vector<Address> parse_address_list(std::string_view s);

// "Name <email>" with the display name quoted when it contains specials / non-atext
// (UTF-8 counts as atext per RFC 6532), escaping \ and ". Control chars become spaces.
std::string format_address(const Address& a);
std::string format_address_list(std::span<const Address> list);  // ", "-joined

// ASCII-lowercased, trimmed; with strip_plus_tag "a+tag@x" → "a@x".
std::string normalize_email(std::string_view email, bool strip_plus_tag = false);
std::string_view local_part(std::string_view email);  // before the last '@' (all if none)
std::string_view domain_of(std::string_view email);   // after the last '@' ("" if none)

// Pragmatic validity check: local@domain, dot-atom or quoted local part (≤64), domain with ≥2
// LDH/UTF-8 labels (≤253), no whitespace/control characters, total ≤254.
bool is_valid_email(std::string_view email);

}  // namespace azm
