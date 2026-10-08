// Owner: WP-C
// Raw header-block parsing, RFC 2047 decoding (iconv) and Message-ID lists (DESIGN "Inbound
// pipeline" step 3). Pure; tested against tests/fixtures/*.eml.
#include "mail/eml.hpp"

#include "core/crypto.hpp"
#include "core/strings.hpp"
#include "core/time.hpp"
#include "mail/types.hpp"

#include <iconv.h>

#include <algorithm>
#include <cerrno>
#include <fstream>
#include <stdexcept>
#include <unordered_set>

namespace azm::mail::eml {
namespace {

bool is_wsp(char c) { return c == ' ' || c == '\t'; }
bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

// ---- iconv -----------------------------------------------------------------------------------

// RAII iconv descriptor.
class Iconv {
 public:
  Iconv(const char* to, const char* from) : cd_(iconv_open(to, from)) {}
  ~Iconv() {
    if (ok()) iconv_close(cd_);
  }
  Iconv(const Iconv&) = delete;
  Iconv& operator=(const Iconv&) = delete;
  bool ok() const { return cd_ != reinterpret_cast<iconv_t>(-1); }

  // Whole-buffer conversion; nullopt on invalid or incomplete input.
  std::optional<std::string> convert(std::string_view in) {
    std::string out;
    out.resize(in.size() * 4 + 16);
    char* inp = const_cast<char*>(in.data());
    std::size_t inleft = in.size();
    std::size_t done = 0;
    for (;;) {
      char* outp = out.data() + done;
      std::size_t outleft = out.size() - done;
      const std::size_t rc = iconv(cd_, &inp, &inleft, &outp, &outleft);
      done = out.size() - outleft;
      if (rc != static_cast<std::size_t>(-1)) break;
      if (errno == E2BIG) {
        out.resize(out.size() * 2);
        continue;
      }
      return std::nullopt;  // EILSEQ / EINVAL
    }
    // Flush any shift state (stateful encodings such as ISO-2022-JP).
    for (;;) {
      char* outp = out.data() + done;
      std::size_t outleft = out.size() - done;
      const std::size_t rc = iconv(cd_, nullptr, nullptr, &outp, &outleft);
      done = out.size() - outleft;
      if (rc != static_cast<std::size_t>(-1)) break;
      if (errno == E2BIG) {
        out.resize(out.size() * 2);
        continue;
      }
      return std::nullopt;
    }
    out.resize(done);
    return out;
  }

 private:
  iconv_t cd_;
};

// Canonical iconv name(s) for a MIME charset label; tried in order.
std::vector<std::string> iconv_names(std::string_view charset) {
  std::string cs = to_lower_ascii(trim(charset));
  if (cs.size() >= 2 && cs.front() == '"' && cs.back() == '"') cs = cs.substr(1, cs.size() - 2);
  // GB family: GB2312 and GBK are subsets of GB18030; mail clients mislabel freely.
  if (cs == "gb2312" || cs == "gbk" || cs == "gb18030" || cs == "cp936" || cs == "ms936" ||
      cs == "x-gbk" || cs == "euc-cn" || cs == "csgb2312" || cs == "x-euc-cn" || cs == "windows-936" ||
      cs == "gb_2312-80" || cs == "chinese")
    return {"GB18030"};
  if (cs == "big5" || cs == "big-5" || cs == "x-big5" || cs == "cn-big5" || cs == "cp950" ||
      cs == "csbig5" || cs == "big5-hkscs")
    return {"BIG5-HKSCS", "CP950", "BIG5"};
  if (cs == "shift_jis" || cs == "shift-jis" || cs == "sjis" || cs == "x-sjis" || cs == "ms_kanji" ||
      cs == "cp932" || cs == "windows-31j" || cs == "csshiftjis" || cs == "x-ms-cp932")
    return {"CP932", "SHIFT_JIS"};
  if (cs == "euc-kr" || cs == "ks_c_5601-1987" || cs == "ks_c_5601" || cs == "cp949" ||
      cs == "ksc5601" || cs == "x-windows-949" || cs == "uhc")
    return {"CP949", "EUC-KR"};
  if (cs == "latin1" || cs == "latin-1" || cs == "l1" || cs == "iso8859-1" || cs == "iso_8859-1")
    return {"ISO-8859-1"};
  if (cs.rfind("iso8859-", 0) == 0) return {"ISO-8859-" + cs.substr(8)};
  if (cs.rfind("iso_8859-", 0) == 0) return {"ISO-8859-" + cs.substr(9)};
  if (cs.rfind("cp125", 0) == 0 && cs.size() == 6) return {"WINDOWS-" + cs.substr(2)};
  if (cs == "ansi_x3.4-1968") return {"ASCII"};
  return {to_upper_ascii(cs)};
}

// Bytes in an unlabeled header (raw 8-bit): UTF-8 when valid, else GB18030, else Latin-1.
std::string raw_to_utf8(std::string_view bytes) {
  if (utf8_valid(bytes)) return std::string(bytes);
  if (auto gb = to_utf8(bytes, "gb18030")) return *gb;
  if (auto l1 = to_utf8(bytes, "iso-8859-1")) return *l1;
  return utf8_sanitize(bytes);
}

int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

std::string q_decode(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (std::size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    if (c == '_') {
      out.push_back(' ');
    } else if (c == '=' && i + 2 < s.size() && hex_value(s[i + 1]) >= 0 &&
               hex_value(s[i + 2]) >= 0) {
      out.push_back(static_cast<char>(hex_value(s[i + 1]) * 16 + hex_value(s[i + 2])));
      i += 2;
    } else {
      out.push_back(c);  // lenient: a stray '=' or raw byte is kept
    }
  }
  return out;
}

// One parsed encoded-word "=?charset[*lang]?B|Q?text?=".
struct EncodedWord {
  std::size_t end = 0;  // index just past "?="
  std::string charset;
  std::string bytes;    // decoded octets (still in `charset`)
};

// Parses an encoded-word starting at s[pos] ("=?"). nullopt when malformed.
std::optional<EncodedWord> parse_encoded_word(std::string_view s, std::size_t pos) {
  if (s.compare(pos, 2, "=?") != 0) return std::nullopt;
  const std::size_t q1 = s.find('?', pos + 2);
  if (q1 == std::string_view::npos || q1 == pos + 2) return std::nullopt;
  if (q1 + 2 >= s.size() || s[q1 + 2] != '?') return std::nullopt;
  const char enc = static_cast<char>(s[q1 + 1] | 0x20);
  if (enc != 'b' && enc != 'q') return std::nullopt;
  const std::size_t text_begin = q1 + 3;
  // The terminating "?=" is searched for within a bounded window only: RFC 2047 §2 caps an
  // encoded-word at 75 characters, and an unbounded search scanned to the end of the value for
  // every unterminated "=?" (quadratic over a 512 KiB header block, review SEC-5). The window
  // is lenient (broken encoders write longer words, or a raw '?' inside Q text).
  constexpr std::size_t kMaxEncodedText = 256;
  const std::string_view window = s.substr(text_begin, std::min(kMaxEncodedText, s.size() - text_begin));
  const std::size_t rel = window.find("?=");
  if (rel == std::string_view::npos) return std::nullopt;
  const std::size_t close = text_begin + rel;
  std::string_view charset = s.substr(pos + 2, q1 - (pos + 2));
  for (char c : charset)
    if (is_space(c)) return std::nullopt;
  if (const auto star = charset.find('*'); star != std::string_view::npos)  // RFC 2231 language
    charset = charset.substr(0, star);
  const std::string_view text = s.substr(text_begin, close - text_begin);
  EncodedWord w;
  w.end = close + 2;
  w.charset = std::string(charset);
  if (enc == 'b') {
    auto d = crypto::b64_decode(text);
    if (!d) return std::nullopt;
    w.bytes = std::move(*d);
  } else {
    w.bytes = q_decode(text);
  }
  return w;
}

// Converts accumulated encoded-word bytes (one charset run) to UTF-8.
std::string charset_to_utf8(std::string_view bytes, std::string_view charset) {
  if (auto u = to_utf8(bytes, charset)) return *u;
  return utf8_sanitize(bytes);
}

// ---- msg-id helpers ----------------------------------------------------------------------------

// Removes RFC 5322 comments (nested, with quoted-pairs) and quoted strings outside <...>.
std::string strip_comments_and_quotes(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  int depth = 0;
  bool in_quote = false;
  bool in_angle = false;
  for (std::size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    if (in_angle) {
      out.push_back(c);
      if (c == '>') in_angle = false;
      continue;
    }
    if (depth > 0) {
      if (c == '\\' && i + 1 < s.size()) ++i;
      else if (c == '(') ++depth;
      else if (c == ')') --depth;
      if (depth == 0) out.push_back(' ');
      continue;
    }
    if (in_quote) {
      if (c == '\\' && i + 1 < s.size()) ++i;
      else if (c == '"') {
        in_quote = false;
        out.push_back(' ');
      }
      continue;
    }
    if (c == '(') {
      depth = 1;
    } else if (c == '"') {
      in_quote = true;
    } else {
      if (c == '<') in_angle = true;
      out.push_back(c);
    }
  }
  return out;
}

std::string remove_spaces(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s)
    if (!is_space(c)) out.push_back(c);
  return out;
}

}  // namespace

// =============================================================================================
// Header block
// =============================================================================================

std::optional<std::string> HeaderBlock::get(std::string_view name) const {
  for (const auto& h : headers)
    if (iequals(h.name, name)) return h.value;
  return std::nullopt;
}

std::vector<std::string> HeaderBlock::get_all(std::string_view name) const {
  std::vector<std::string> out;
  for (const auto& h : headers)
    if (iequals(h.name, name)) out.push_back(h.value);
  return out;
}

HeaderBlock parse_header_block(std::string_view raw, std::size_t max_bytes) {
  HeaderBlock block;
  if (raw.size() > max_bytes) {
    raw = raw.substr(0, max_bytes);
    block.truncated = true;  // cleared below when the block ends inside the limit
  }
  bool ended = false;
  bool continuing = false;  // the previous line started a header we kept (folds attach to it)
  std::size_t pos = 0;
  while (pos < raw.size()) {
    std::size_t eol = raw.find('\n', pos);
    const bool complete = eol != std::string_view::npos;
    if (!complete) eol = raw.size();
    std::string_view line = raw.substr(pos, eol - pos);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    pos = complete ? eol + 1 : raw.size();
    if (line.empty()) {  // end of the header block
      ended = true;
      break;
    }
    if (is_wsp(line.front())) {  // folded continuation: CRLF removed, WSP kept (RFC 5322 §2.2.3)
      if (continuing) block.headers.back().value.append(line);
      continue;
    }
    continuing = false;
    const auto colon = line.find(':');
    if (colon == std::string_view::npos || colon == 0) continue;  // mbox "From " line / garbage
    std::string_view name = line.substr(0, colon);
    while (!name.empty() && is_wsp(name.back())) name.remove_suffix(1);  // obs "Name :"
    bool valid = !name.empty();
    for (char c : name)
      if (static_cast<unsigned char>(c) <= 32 || static_cast<unsigned char>(c) >= 127) valid = false;
    if (!valid) continue;
    std::string_view value = line.substr(colon + 1);
    while (!value.empty() && is_wsp(value.front())) value.remove_prefix(1);
    block.headers.push_back({std::string(name), std::string(value)});
    continuing = true;
  }
  if (ended) block.truncated = false;
  for (auto& h : block.headers) {
    while (!h.value.empty() && is_space(h.value.back())) h.value.pop_back();
  }
  return block;
}

std::string read_file_prefix(const std::filesystem::path& file, std::size_t max_bytes) {
  std::ifstream in(file, std::ios::binary);
  if (!in) throw std::runtime_error("eml: cannot open " + file.filename().string());
  std::string out(max_bytes, '\0');
  in.read(out.data(), static_cast<std::streamsize>(max_bytes));
  if (in.bad()) throw std::runtime_error("eml: read failed for " + file.filename().string());
  out.resize(static_cast<std::size_t>(in.gcount()));
  return out;
}

std::string unfold(std::string_view value) {
  std::string out;
  out.reserve(value.size());
  for (std::size_t i = 0; i < value.size(); ++i) {
    const char c = value[i];
    if (c == '\r' || c == '\n') continue;  // folding CRLF/LF dropped; the WSP after it stays
    out.push_back(c);
  }
  return out;
}

// =============================================================================================
// RFC 2047
// =============================================================================================

std::optional<std::string> to_utf8(std::string_view bytes, std::string_view charset) {
  const std::string cs = to_lower_ascii(trim(charset));
  if (cs.empty()) return std::nullopt;
  if (cs == "utf-8" || cs == "utf8" || cs == "us-ascii" || cs == "ascii" || cs == "unicode-1-1-utf-8") {
    if (utf8_valid(bytes)) return std::string(bytes);
    return std::nullopt;
  }
  for (const auto& name : iconv_names(cs)) {
    Iconv cd("UTF-8", name.c_str());
    if (!cd.ok()) continue;
    auto out = cd.convert(bytes);
    if (out && utf8_valid(*out)) return out;
    return std::nullopt;  // known charset, invalid input
  }
  return std::nullopt;  // unknown charset
}

std::string decode_rfc2047(std::string_view value_in) {
  const std::string value = unfold(value_in);
  const std::string_view s = value;
  std::string out;
  out.reserve(s.size());

  std::string run_bytes;    // decoded bytes of adjacent encoded-words sharing a charset
  std::string run_charset;  // lowercase
  auto flush_run = [&] {
    if (!run_bytes.empty()) out += charset_to_utf8(run_bytes, run_charset);
    run_bytes.clear();
    run_charset.clear();
  };

  std::string plain;  // pending unencoded text (may contain raw 8-bit bytes)
  auto flush_plain = [&] {
    if (!plain.empty()) out += raw_to_utf8(plain);
    plain.clear();
  };

  bool prev_was_word = false;
  std::size_t i = 0;
  while (i < s.size()) {
    if (s[i] == '=' && i + 1 < s.size() && s[i + 1] == '?') {
      if (auto w = parse_encoded_word(s, i)) {
        // Whitespace between two adjacent encoded-words is not displayed (RFC 2047 §6.2).
        bool only_ws = prev_was_word;
        for (char c : plain)
          if (!is_space(c)) only_ws = false;
        if (only_ws) plain.clear();
        flush_plain();
        const std::string cs = to_lower_ascii(w->charset);
        if (!run_bytes.empty() && cs != run_charset) flush_run();
        run_charset = cs;
        run_bytes += w->bytes;  // same-charset runs are joined before conversion (split chars)
        i = w->end;
        prev_was_word = true;
        continue;
      }
    }
    // Plain text: keep collecting; a run of encoded-words ends at the first non-WSP char.
    if (!is_space(s[i]) && !run_bytes.empty()) flush_run();
    if (!is_space(s[i])) prev_was_word = false;
    plain.push_back(s[i]);
    ++i;
  }
  // Trailing whitespace after the last encoded word is kept as plain text.
  flush_run();
  flush_plain();
  return out;
}

// =============================================================================================
// Message-ID lists
// =============================================================================================

std::vector<std::string> parse_msgid_list(std::string_view value) {
  const std::string cleaned = strip_comments_and_quotes(unfold(value));
  std::vector<std::string> out;
  std::unordered_set<std::string> seen;
  auto add = [&](std::string_view raw) {
    // Raw header bytes: invalid UTF-8, control characters or specials must never reach the
    // database or a reply's In-Reply-To/References (review R1/SEC-8).
    std::string id = sanitize_message_id(remove_spaces(raw));
    if (id.empty() || !seen.insert(id).second) return;
    out.push_back(std::move(id));
  };
  const std::string_view s = cleaned;
  std::size_t i = 0;
  while (i < s.size()) {
    const char c = s[i];
    if (c == '<') {
      const std::size_t close = s.find('>', i + 1);
      if (close == std::string_view::npos) {  // unterminated: take the rest of the token
        std::size_t e = i + 1;
        while (e < s.size() && !is_space(s[e]) && s[e] != ',') ++e;
        add(s.substr(i + 1, e - i - 1));
        i = e;
        continue;
      }
      add(s.substr(i + 1, close - i - 1));
      i = close + 1;
      continue;
    }
    if (is_space(c) || c == ',' || c == ';') {
      ++i;
      continue;
    }
    // Bare token without brackets: accepted when it looks like an id (contains '@').
    std::size_t e = i;
    while (e < s.size() && !is_space(s[e]) && s[e] != ',' && s[e] != ';' && s[e] != '<') ++e;
    const std::string_view tok = s.substr(i, e - i);
    if (tok.find('@') != std::string_view::npos && tok.front() != '@' && tok.back() != '@') add(tok);
    i = e;
  }
  return out;
}

std::optional<std::string> parse_msgid(std::string_view value) {
  auto ids = parse_msgid_list(value);
  if (ids.empty()) return std::nullopt;
  return std::move(ids.front());
}

// =============================================================================================
// Extraction
// =============================================================================================

namespace {

// Header values we decode are capped (review SEC-5): no legitimate Subject / From / Reply-To is
// anywhere near this, and decoding (iconv per charset run) stays cheap for a crafted 512 KiB
// header block. Cut at whitespace so an encoded-word or multi-byte character is not split.
constexpr std::size_t kMaxDecodedHeaderBytes = 64u << 10;

std::string_view capped(std::string_view v) {
  if (v.size() <= kMaxDecodedHeaderBytes) return v;
  const std::size_t ws = v.find_last_of(" \t", kMaxDecodedHeaderBytes);
  if (ws != std::string_view::npos && ws > kMaxDecodedHeaderBytes / 2) return v.substr(0, ws);
  return v.substr(0, kMaxDecodedHeaderBytes);
}

std::vector<Address> decoded_addresses(const std::optional<std::string>& raw) {
  if (!raw) return {};
  // Parse the RAW value first (encoded-words are atoms / quoted text), then decode names, so
  // decoded specials (",", "<") in a display name cannot break the list.
  std::vector<Address> list = parse_address_list(unfold(capped(*raw)));
  for (auto& a : list) {
    a.name = std::string(trim(decode_rfc2047(a.name)));
    a.email = raw_to_utf8(a.email);
  }
  return list;
}

}  // namespace

ParsedHeaders extract_headers(const HeaderBlock& block) {
  ParsedHeaders out;
  if (auto v = block.get("Message-ID")) out.message_id = parse_msgid(*v);
  if (auto v = block.get("In-Reply-To")) out.in_reply_to = parse_msgid(*v);
  {
    std::string refs;
    for (const auto& v : block.get_all("References")) {
      refs += v;
      refs.push_back(' ');
    }
    out.references = parse_msgid_list(refs);
  }
  if (auto v = block.get("X-AzMail-Ref")) {
    std::string ref(trim(unfold(*v)));
    if (!ref.empty()) out.x_azmail_ref = std::move(ref);
  }
  if (auto v = block.get("Auto-Submitted")) {
    // "auto-replied; owner-email=…" (RFC 3834) → "auto-replied"
    std::string tok = strip_comments_and_quotes(unfold(*v));
    if (const auto semi = tok.find(';'); semi != std::string::npos) tok.resize(semi);
    std::string t = to_lower_ascii(trim(tok));
    if (!t.empty()) out.auto_submitted = std::move(t);
  }
  if (auto v = block.get("Date")) out.date_ms = parse_rfc5322_date(trim(unfold(*v)));
  if (auto v = block.get("Subject")) out.subject = std::string(trim(decode_rfc2047(capped(*v))));
  out.from = decoded_addresses(block.get("From"));
  out.reply_to = decoded_addresses(block.get("Reply-To"));
  return out;
}

ParsedHeaders parse_headers(std::string_view raw) { return extract_headers(parse_header_block(raw)); }

}  // namespace azm::mail::eml
