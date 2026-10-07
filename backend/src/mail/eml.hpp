// Owner: WP-C
//
// Raw RFC 5322 header-block parsing for inbound mail (DESIGN "Inbound pipeline" step 3, F4.7,
// fixtures in tests/fixtures/*.eml). Pure functions; no DB, no network.
//  * Only the header block is parsed (up to the first empty line, at most kMaxHeaderBytes).
//  * Unfolding per RFC 5322 §2.2.3 (CRLF or bare LF followed by WSP).
//  * RFC 2047 encoded-words (=?charset?B|Q?…?=) decoded to UTF-8; adjacent encoded-words
//    separated only by whitespace are joined. Charsets via iconv: UTF-8, US-ASCII, GBK, GB2312
//    and GB18030 (all decoded as GB18030), Big5, ISO-8859-1…16, Windows-125x, Shift_JIS,
//    EUC-KR …; unknown charsets / invalid bytes fall back to utf8_sanitize of the raw text.
//  * Raw 8-bit header bytes that are not valid UTF-8 are tried as GB18030, then Latin-1.
#pragma once

#include "core/address.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace azm::mail::eml {

inline constexpr std::size_t kMaxHeaderBytes = 512u << 10;  // 512 KiB

struct Header {
  std::string name;   // as written (case preserved)
  std::string value;  // unfolded raw value (not RFC 2047-decoded), leading WSP removed
};

struct HeaderBlock {
  std::vector<Header> headers;  // in order; duplicates kept
  bool truncated = false;       // the block exceeded the byte limit

  // First / all values of a header (case-insensitive name).
  std::optional<std::string> get(std::string_view name) const;
  std::vector<std::string> get_all(std::string_view name) const;
};

// Parses the header block at the start of `raw` (stops at the first empty line or `max_bytes`).
HeaderBlock parse_header_block(std::string_view raw, std::size_t max_bytes = kMaxHeaderBytes);

// Reads at most `max_bytes` from the start of a file (the raw .eml blob staged on disk).
// Throws std::runtime_error on I/O failure.
std::string read_file_prefix(const std::filesystem::path& file, std::size_t max_bytes = kMaxHeaderBytes);

// Removes folding (CRLF/LF + WSP → the WSP).
std::string unfold(std::string_view value);

// RFC 2047 decoding of a header value to UTF-8 (see file comment for charset handling).
std::string decode_rfc2047(std::string_view value);

// Converts `bytes` in `charset` (case-insensitive, aliases accepted) to UTF-8 via iconv.
// nullopt when the charset is unknown or the input is invalid in it.
std::optional<std::string> to_utf8(std::string_view bytes, std::string_view charset);

// Message-ID lists ("<a@b> <c@d>", comments and folding tolerated, also bare ids without <>):
// normalized ids without <>, in order, deduplicated.
std::vector<std::string> parse_msgid_list(std::string_view value);
// First id of the list, or nullopt.
std::optional<std::string> parse_msgid(std::string_view value);

// The headers inbound delivery needs (DESIGN step 3).
struct ParsedHeaders {
  std::optional<std::string> message_id;    // normalized
  std::optional<std::string> in_reply_to;   // first id of In-Reply-To, normalized
  std::vector<std::string> references;      // References ids, normalized, in order
  std::optional<std::string> x_azmail_ref;  // X-AzMail-Ref (trimmed)
  std::optional<std::string> auto_submitted;  // Auto-Submitted (lowercased token)
  std::optional<int64_t> date_ms;           // Date via parse_rfc5322_date
  std::optional<std::string> subject;       // decoded UTF-8
  std::vector<Address> from;                // decoded display names
  std::vector<Address> reply_to;
};
ParsedHeaders extract_headers(const HeaderBlock& block);
// parse_header_block + extract_headers.
ParsedHeaders parse_headers(std::string_view raw);

}  // namespace azm::mail::eml
