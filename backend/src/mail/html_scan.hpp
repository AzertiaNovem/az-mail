// Owner: WP-B (internal; not a contract header)
//
// A small, forgiving HTML tokenizer for email bodies: enough of the HTML5 tokenization rules to
// find tags, attributes (with exact source spans, so callers can splice rewritten values back),
// comments and raw-text elements in arbitrary, possibly malformed markup. No tree building.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace azm::mail::html {

struct Attr {
  std::size_t begin = 0, end = 0;              // raw span [begin, end) of `name[=value]`
  std::string name;                            // lowercased
  bool has_value = false;
  std::size_t value_begin = 0, value_end = 0;  // raw value span, quotes excluded
  char quote = 0;                              // '"', '\'' or 0 (unquoted / no value)

  std::string_view raw_value(std::string_view html) const {
    return html.substr(value_begin, value_end - value_begin);
  }
};

struct Tag {
  std::size_t begin = 0, end = 0;  // [begin, end): '<' … '>' inclusive
  std::string name;                // lowercased
  bool closing = false;            // </name>
  bool self_closing = false;       // <name … />
  bool complete = true;            // false when the input ended inside the tag
  std::size_t close_at = 0;        // index of the terminating "/>" or ">" (insertion point)
  std::vector<Attr> attrs;

  const Attr* find(std::string_view attr_name) const;
};

enum class MarkupKind { Tag, Comment, Other };  // Other: <!DOCTYPE>, <?…?>, <![CDATA[…]]>, bogus

struct Markup {
  MarkupKind kind = MarkupKind::Other;
  std::size_t end = 0;  // one past the construct
  Tag tag;              // valid when kind == Tag
};

// Parses the markup construct starting at html[pos] == '<'. nullopt when the '<' is literal
// text ("a < b", "<3").
std::optional<Markup> parse_markup(std::string_view html, std::size_t pos);

// Elements whose content is raw text (no tags inside): script, style, textarea, title, xmp,
// iframe, noembed, noframes, noscript, template, plaintext.
bool is_raw_text_element(std::string_view name);

// Index of the next "</name" (ASCII case-insensitive, followed by whitespace, '/' or '>') at
// or after `pos`, or npos.
std::size_t find_close_tag(std::string_view html, std::size_t pos, std::string_view name);

// Decodes character references: named (common HTML set), decimal and hex. Numeric references
// follow the HTML rules (0 / surrogates / > U+10FFFF → U+FFFD, 0x80–0x9F → Windows-1252).
// Unknown or malformed references are kept literally.
std::string decode_entities(std::string_view s);

// UTF-8 encoding of a code point (invalid → U+FFFD).
void append_utf8(std::string& out, uint32_t cp);

// Escapes an attribute value for a double-quoted attribute (& < > ").
std::string escape_attr(std::string_view v);

}  // namespace azm::mail::html
