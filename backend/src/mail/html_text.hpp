// Owner: WP-B
//
// HTML → plain text and snippets (pure; no DB). Used for the frozen text part of outgoing mail,
// FTS bodies (§2), snippets and thread previews.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace azm::mail {

inline constexpr std::size_t kSnippetChars = 200;  // code points

// Readable text from arbitrary (possibly malformed) email HTML: drops <head>, <style>,
// <script> and comments; block elements (p, div, br, li, tr, h1–h6, blockquote) become line
// breaks; <a href> keeps its text (plus " <url>" when the text differs); entities decoded
// (named, decimal, hex); whitespace runs collapsed; ≤ 2 consecutive blank lines; output is
// valid UTF-8 (invalid input bytes replaced).
std::string html_to_text(std::string_view html);

// Preview text: drops quoted parts (lines starting with '>', "On … wrote:" / "在 … 写道："
// headers and everything after them, Gmail/Outlook quote blocks when given HTML), collapses all
// whitespace to single spaces and truncates to `max_chars` code points (no ellipsis).
// `is_html` selects html_to_text first.
std::string make_snippet(std::string_view body, bool is_html, std::size_t max_chars = kSnippetChars);

// The text with quoted reply sections removed (as described for make_snippet), whitespace kept.
std::string strip_quoted_text(std::string_view text);

}  // namespace azm::mail
