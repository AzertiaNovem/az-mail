// Owner: WP-B
// HTML → text, quote stripping and snippets (html_text.hpp).
#include "mail/html_text.hpp"

#include "core/strings.hpp"
#include "mail/html_scan.hpp"
#include "mail/internal.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace azm::mail {
namespace {

using html::Markup;
using html::MarkupKind;
using html::Tag;

// ---- text accumulation ------------------------------------------------------------------------

// Builds the plain-text output: collapses whitespace in normal flow, keeps it verbatim inside
// <pre>, and turns block boundaries into (bounded) line breaks.
class TextBuilder {
 public:
  void text(std::string_view s, bool preserve) {
    if (preserve) {
      for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '\r') {
          if (i + 1 < s.size() && s[i + 1] == '\n') continue;
          hard_newline();
          continue;
        }
        if (c == '\n') {
          hard_newline();
          continue;
        }
        flush();
        out_.push_back(c);
        at_line_start_ = false;
      }
      return;
    }
    for (std::size_t i = 0; i < s.size();) {
      if (const std::size_t n = detail::unicode_space_len(s, i); n > 0) {
        pending_space_ = true;
        i += n;
        continue;
      }
      flush();
      out_.push_back(s[i]);
      at_line_start_ = false;
      ++i;
    }
  }

  void block() { request_newlines(1); }      // div, li, tr, …
  void paragraph() { request_newlines(2); }  // p, h1–h6, blockquote, table, …
  void line_break() {                        // <br>: cumulative, at most 2 blank lines
    if (out_.empty()) return;
    pending_newlines_ = std::min(pending_newlines_ + 1, 3);
    pending_space_ = false;
  }
  void space() { pending_space_ = true; }

  // Text that must start on the current (flushed) position, e.g. list bullets.
  void prefix(std::string_view p) {
    flush();
    out_.append(p);
    at_line_start_ = false;
    pending_space_ = false;
  }

  // Appends inline text after the current content (link targets).
  void inline_text(std::string_view s) {
    pending_space_ = false;
    flush();
    out_.append(s);
    at_line_start_ = false;
  }

  std::size_t size() const { return out_.size(); }
  std::string_view since(std::size_t pos) const {
    return pos <= out_.size() ? std::string_view(out_).substr(pos) : std::string_view();
  }

  std::string finish() {
    // Trim trailing whitespace; leading whitespace never gets in (pending is dropped at start).
    while (!out_.empty() && (out_.back() == ' ' || out_.back() == '\n')) out_.pop_back();
    return std::move(out_);
  }

 private:
  void request_newlines(int n) {
    if (out_.empty()) return;
    pending_newlines_ = std::max(pending_newlines_, n);
    pending_space_ = false;
  }
  void hard_newline() {
    flush();
    trim_trailing_spaces();
    out_.push_back('\n');
    at_line_start_ = true;
    pending_space_ = false;
  }
  void trim_trailing_spaces() {
    while (!out_.empty() && out_.back() == ' ') out_.pop_back();
  }
  void flush() {
    if (pending_newlines_ > 0 && !out_.empty()) {
      trim_trailing_spaces();
      // Count newlines already at the end so explicit ones (pre) are not doubled. Only up to
      // pending_newlines_ matter: counting a long run of <pre> newlines on every flush would be
      // quadratic (review SEC-1).
      int existing = 0;
      for (auto it = out_.rbegin(); it != out_.rend() && *it == '\n' && existing < pending_newlines_; ++it)
        ++existing;
      for (int k = existing; k < pending_newlines_; ++k) out_.push_back('\n');
      at_line_start_ = true;
      pending_space_ = false;
    }
    pending_newlines_ = 0;
    if (pending_space_ && !at_line_start_ && !out_.empty()) out_.push_back(' ');
    pending_space_ = false;
  }

  std::string out_;
  int pending_newlines_ = 0;
  bool pending_space_ = false;
  bool at_line_start_ = true;
};

bool is_paragraph_tag(std::string_view n) {
  return n == "p" || n == "h1" || n == "h2" || n == "h3" || n == "h4" || n == "h5" ||
         n == "h6" || n == "blockquote" || n == "pre" || n == "table" || n == "dl" ||
         n == "hr" || n == "address" || n == "figure" || n == "form" || n == "fieldset" ||
         n == "ul" || n == "ol";
}

bool is_block_tag(std::string_view n) {
  return n == "div" || n == "tr" || n == "li" || n == "dt" || n == "dd" || n == "section" ||
         n == "article" || n == "header" || n == "footer" || n == "nav" || n == "aside" ||
         n == "main" || n == "center" || n == "caption" || n == "figcaption" ||
         n == "details" || n == "summary" || n == "tbody" || n == "thead" || n == "tfoot" ||
         n == "body" || n == "html" || n == "legend" || n == "option";
}

// Whether a link's target adds information beyond its visible text. `text` is a view into the
// builder, never a copy: many nested unclosed <a> around a large text would otherwise copy it
// once per </a> (quadratic, review SEC-1). Every comparison below is size-checked first
// (iequals), so a long text costs O(1) here.
bool link_worth_showing(std::string_view href, std::string_view text) {
  href = trim(href);
  if (href.empty()) return false;
  if (href[0] == '#' || istarts_with(href, "javascript:") || istarts_with(href, "cid:") ||
      istarts_with(href, "data:"))
    return false;
  // A text far longer than the target cannot be a spelling of it: decided without scanning the
  // text (trailing '/' or whitespace runs would otherwise be walked once per </a>).
  if (text.size() > href.size() + 4096) return true;
  text = trim(text);
  if (text.empty()) return false;
  auto same = [&](std::string_view h) {
    while (!h.empty() && h.back() == '/') h.remove_suffix(1);
    std::string_view t = text;
    while (!t.empty() && t.back() == '/') t.remove_suffix(1);
    return iequals(h, t);
  };
  if (same(href)) return false;
  if (istarts_with(href, "mailto:")) {
    std::string_view addr = href.substr(7);
    if (const auto q = addr.find('?'); q != std::string_view::npos) addr = addr.substr(0, q);
    return !same(addr);
  }
  for (std::string_view scheme : {"https://", "http://"}) {
    if (istarts_with(href, scheme)) {
      std::string_view rest = href.substr(scheme.size());
      if (same(rest)) return false;
      if (istarts_with(rest, "www.") && same(rest.substr(4))) return false;
    }
  }
  return true;
}

struct ListState {
  bool ordered = false;
  int counter = 0;
};
struct LinkState {
  std::string href;
  std::size_t text_start = 0;
};

// Index just past the element whose content must be skipped (script/style/title/head…).
std::size_t skip_element_content(std::string_view h, std::size_t content_begin,
                                 const std::string& name) {
  if (name == "head") {
    // Malformed mail often omits </head>: stop at <body as well. One forward scan that stops at
    // whichever comes first — looking for "</head" over the whole rest of the document first
    // made every unclosed <head> cost O(n) (quadratic for "<head><body>" repeated, review SEC-1).
    for (std::size_t p = h.find('<', content_begin); p != std::string_view::npos;
         p = h.find('<', p + 1)) {
      if (p + 5 <= h.size() && iequals(h.substr(p + 1, 4), "body")) return p;
      if (p + 6 <= h.size() && h[p + 1] == '/' && iequals(h.substr(p + 2, 4), "head")) {
        const std::size_t after = p + 6;
        const char c = after < h.size() ? h[after] : '>';
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '/' || c == '>') return p;
      }
    }
    return h.size();
  }
  const std::size_t close = html::find_close_tag(h, content_begin, name);
  return close == std::string_view::npos ? h.size() : close;
}

// ---- quote detection ------------------------------------------------------------------------

std::string_view trim_line(std::string_view s) {
  // ASCII whitespace plus U+3000 / U+00A0 at both ends.
  for (;;) {
    const std::string_view t = trim(s);
    if (t.size() != s.size()) {
      s = t;
      continue;
    }
    if (s.size() >= 3 && s.substr(0, 3) == "\xE3\x80\x80") { s.remove_prefix(3); continue; }
    if (s.size() >= 3 && s.substr(s.size() - 3) == "\xE3\x80\x80") { s.remove_suffix(3); continue; }
    if (s.size() >= 2 && s.substr(0, 2) == "\xC2\xA0") { s.remove_prefix(2); continue; }
    if (s.size() >= 2 && s.substr(s.size() - 2) == "\xC2\xA0") { s.remove_suffix(2); continue; }
    return s;
  }
}

bool starts_with(std::string_view s, std::string_view p) { return s.substr(0, p.size()) == p; }
bool ends_with(std::string_view s, std::string_view p) {
  return s.size() >= p.size() && s.substr(s.size() - p.size()) == p;
}
bool contains(std::string_view s, std::string_view p) { return s.find(p) != std::string_view::npos; }
bool icontains(std::string_view s, std::string_view p) {
  return to_lower_ascii(s).find(to_lower_ascii(p)) != std::string::npos;
}

bool ends_with_wrote(std::string_view l) {
  return ends_with(l, "wrote:") || ends_with(l, "写道：") || ends_with(l, "写道:") ||
         ends_with(l, "寫道：") || ends_with(l, "寫道:");
}
bool starts_attribution(std::string_view l) {
  return starts_with(l, "On ") || starts_with(l, "在") || starts_with(l, "于") ||
         starts_with(l, "於");
}

bool is_original_separator(std::string_view l) {
  if (!starts_with(l, "--") && !starts_with(l, "__")) return false;
  return icontains(l, "original message") || contains(l, "原始邮件") || contains(l, "原始郵件") ||
         icontains(l, "reply message");
}

bool is_header_line(std::string_view l, std::initializer_list<std::string_view> names) {
  for (std::string_view n : names) {
    if (starts_with(l, n)) {
      const std::string_view rest = l.substr(n.size());
      if (starts_with(rest, ":") || starts_with(rest, "：") || starts_with(rest, " :")) return true;
    }
  }
  return false;
}

// Outlook-style quoted header block: "From: …" followed by Sent/Date and To/Subject lines.
bool is_outlook_header(const std::vector<std::string_view>& lines, std::size_t i) {
  const std::string_view l = trim_line(lines[i]);
  if (!is_header_line(l, {"From", "发件人", "寄件者", "*From"})) return false;
  bool date = false, other = false;
  for (std::size_t k = i + 1; k < lines.size() && k <= i + 5; ++k) {
    const std::string_view x = trim_line(lines[k]);
    if (is_header_line(x, {"Sent", "Date", "发送时间", "日期", "時間", "*Sent"})) date = true;
    if (is_header_line(x, {"To", "Subject", "Cc", "收件人", "主题", "抄送", "主旨", "*To"}))
      other = true;
  }
  return date && other;
}

bool is_underscore_rule(std::string_view l) {
  if (l.size() < 10) return false;
  return std::all_of(l.begin(), l.end(), [](char c) { return c == '_'; });
}

// Index of the first line that starts the quoted part (attribution, separator, Outlook
// header), or lines.size(). '>' lines are handled separately.
std::size_t quote_cut_index(const std::vector<std::string_view>& lines) {
  for (std::size_t i = 0; i < lines.size(); ++i) {
    const std::string_view l = trim_line(lines[i]);
    if (l.empty()) continue;
    if (starts_attribution(l)) {
      if (ends_with_wrote(l)) return i;
      // Attribution wrapped over two lines.
      if (i + 1 < lines.size() && ends_with_wrote(trim_line(lines[i + 1]))) return i;
    }
    if (ends_with_wrote(l)) {
      // "<name> wrote:" directly followed by quoted lines.
      for (std::size_t k = i + 1; k < lines.size(); ++k) {
        const std::string_view x = trim_line(lines[k]);
        if (x.empty()) continue;
        if (starts_with(x, ">")) return i;
        break;
      }
    }
    if (is_original_separator(l)) return i;
    if (is_underscore_rule(l) && i + 1 < lines.size() && is_outlook_header(lines, i + 1)) return i;
    if (is_outlook_header(lines, i)) return i;
  }
  return lines.size();
}

std::vector<std::string_view> split_lines(std::string_view text) {
  std::vector<std::string_view> lines;
  std::size_t b = 0;
  for (std::size_t i = 0; i <= text.size(); ++i) {
    if (i == text.size() || text[i] == '\n') {
      std::string_view l = text.substr(b, i - b);
      if (!l.empty() && l.back() == '\r') l.remove_suffix(1);
      lines.push_back(l);
      b = i + 1;
    }
  }
  return lines;
}

std::string join_lines(const std::vector<std::string_view>& lines, std::size_t count) {
  std::string out;
  for (std::size_t i = 0; i < count; ++i) {
    if (i) out.push_back('\n');
    out.append(lines[i]);
  }
  while (!out.empty() && (out.back() == '\n' || out.back() == ' ' || out.back() == '\t'))
    out.pop_back();
  return out;
}

// Removes everything from a signature delimiter line ("-- ") on.
std::string cut_signature(std::string_view text) {
  const auto lines = split_lines(text);
  for (std::size_t i = 0; i < lines.size(); ++i) {
    if (lines[i] == "-- " || lines[i] == "--") return join_lines(lines, i);
  }
  return std::string(text);
}

// Drops Gmail / Apple Mail / Thunderbird / Yahoo / Outlook quote containers from HTML.
std::string remove_html_quote_blocks(std::string_view h) {
  auto is_quote_container = [&](const Tag& t) {
    if (t.name != "div" && t.name != "blockquote") return false;
    for (const auto& a : t.attrs) {
      const std::string v = to_lower_ascii(html::decode_entities(a.raw_value(h)));
      if (a.name == "class" && (contains(v, "gmail_quote") || contains(v, "yahoo_quoted") ||
                                contains(v, "moz-cite-prefix") || contains(v, "protonmail_quote")))
        return true;
      if (a.name == "type" && v == "cite") return true;
      if (a.name == "id" && (v == "divrplyfwdmsg" || v == "appendonsend")) return true;
    }
    return false;
  };
  std::string out;
  out.reserve(h.size());
  std::size_t i = 0, copied = 0;
  while (i < h.size()) {
    const std::size_t lt = h.find('<', i);
    if (lt == std::string_view::npos) break;
    auto m = html::parse_markup(h, lt);
    if (!m) {
      i = lt + 1;
      continue;
    }
    if (m->kind != MarkupKind::Tag) {
      i = m->end;
      continue;
    }
    const Tag& t = m->tag;
    if (!t.closing && html::is_raw_text_element(t.name)) {
      const std::size_t close = html::find_close_tag(h, m->end, t.name);
      i = close == std::string_view::npos ? h.size() : close;
      continue;
    }
    if (!t.closing && !t.self_closing && is_quote_container(t)) {
      if (t.find("id") && iequals(html::decode_entities(t.find("id")->raw_value(h)), "divRplyFwdMsg")) {
        // Outlook: the reply header div is followed by the quoted message as siblings.
        out.append(h.substr(copied, lt - copied));
        copied = h.size();
        break;
      }
      // Skip to the matching close tag (same name, nesting-aware).
      int depth = 1;
      std::size_t j = m->end;
      while (depth > 0 && j < h.size()) {
        const std::size_t p = h.find('<', j);
        if (p == std::string_view::npos) {
          j = h.size();
          break;
        }
        auto mm = html::parse_markup(h, p);
        if (!mm) {
          j = p + 1;
          continue;
        }
        if (mm->kind == MarkupKind::Tag && mm->tag.name == t.name && !mm->tag.self_closing)
          depth += mm->tag.closing ? -1 : 1;
        j = mm->end;
      }
      out.append(h.substr(copied, lt - copied));
      copied = j;
      i = j;
      continue;
    }
    i = m->end;
  }
  if (copied < h.size()) out.append(h.substr(copied));
  return out;
}

}  // namespace

std::string html_to_text(std::string_view input) {
  const std::string h = utf8_sanitize(input);
  const std::string_view hv = h;
  TextBuilder b;
  std::vector<ListState> lists;
  std::vector<LinkState> links;
  int pre_depth = 0;
  bool first_cell = true;

  std::size_t i = 0;
  const std::size_t n = hv.size();
  while (i < n) {
    if (hv[i] != '<') {
      std::size_t j = hv.find('<', i + 1);
      if (j == std::string_view::npos) j = n;
      b.text(html::decode_entities(hv.substr(i, j - i)), pre_depth > 0);
      i = j;
      continue;
    }
    auto m = html::parse_markup(hv, i);
    if (!m) {  // literal '<'
      std::size_t j = hv.find('<', i + 1);
      if (j == std::string_view::npos) j = n;
      b.text(html::decode_entities(hv.substr(i, j - i)), pre_depth > 0);
      i = j;
      continue;
    }
    i = m->end;
    if (m->kind != MarkupKind::Tag) continue;  // comments, doctype, CDATA
    const Tag& t = m->tag;
    if (!t.complete) break;  // input ended inside a tag
    const std::string& name = t.name;

    if (!t.closing && (name == "head" || name == "script" || name == "style" || name == "title" ||
                       name == "noscript" || name == "template" || name == "xmp" ||
                       name == "iframe" || name == "noembed" || name == "noframes" ||
                       name == "textarea")) {
      if (!t.self_closing) i = skip_element_content(hv, m->end, name);
      continue;
    }

    if (name == "br") {
      b.line_break();
    } else if (name == "pre") {
      b.paragraph();
      if (t.closing) pre_depth = std::max(0, pre_depth - 1);
      else if (!t.self_closing) ++pre_depth;
    } else if (name == "ul" || name == "ol") {
      b.paragraph();
      if (t.closing) {
        if (!lists.empty()) lists.pop_back();
      } else if (!t.self_closing) {
        lists.push_back({name == "ol", 0});
      }
    } else if (name == "li") {
      b.block();
      if (!t.closing) {
        if (!lists.empty() && lists.back().ordered)
          b.prefix(std::to_string(++lists.back().counter) + ". ");
        else
          b.prefix("- ");
      }
    } else if (name == "tr") {
      b.block();
      first_cell = true;
    } else if (name == "td" || name == "th") {
      if (!t.closing) {
        if (!first_cell) b.space();
        first_cell = false;
      }
    } else if (name == "a") {
      if (!t.closing) {
        std::string href;
        if (const auto* a = t.find("href"); a && a->has_value)
          href = html::decode_entities(a->raw_value(hv));
        if (!t.self_closing) links.push_back({std::move(href), b.size()});
      } else if (!links.empty()) {
        LinkState l = std::move(links.back());
        links.pop_back();
        // Decided before inline_text appends (the view points into the builder).
        if (link_worth_showing(l.href, b.since(l.text_start))) b.inline_text(" <" + std::string(trim(l.href)) + ">");
      }
    } else if (is_paragraph_tag(name)) {
      b.paragraph();
    } else if (is_block_tag(name)) {
      b.block();
    }
  }
  return b.finish();
}

std::string strip_quoted_text(std::string_view text) {
  const std::string clean = utf8_sanitize(text);
  const auto lines = split_lines(clean);
  const std::size_t cut = quote_cut_index(lines);
  std::vector<std::string_view> kept;
  kept.reserve(cut);
  for (std::size_t i = 0; i < cut; ++i) {
    if (starts_with(trim_line(lines[i]), ">")) continue;
    kept.push_back(lines[i]);
  }
  return join_lines(kept, kept.size());
}

std::string make_snippet(std::string_view body, bool is_html, std::size_t max_chars) {
  // The snippet is a short prefix: only the start of a huge body matters, and the work done
  // here (inside write transactions) stays bounded whatever the sender put in (review SEC-1).
  constexpr std::size_t kMaxSnippetInput = 1u << 20;
  const std::string clean = utf8_sanitize(body.size() > kMaxSnippetInput ? utf8_truncate(body, kMaxSnippetInput)
                                                                         : std::string(body));
  const std::string text = is_html ? html_to_text(remove_html_quote_blocks(clean)) : clean;
  std::string snippet = detail::collapse_whitespace(cut_signature(strip_quoted_text(text)));
  if (snippet.empty()) {
    // Nothing but a quote (e.g. an empty reply): show the quoted text rather than nothing.
    snippet = detail::collapse_whitespace(cut_signature(is_html ? html_to_text(clean) : clean));
  }
  return detail::utf8_prefix_chars(snippet, max_chars);
}

}  // namespace azm::mail
