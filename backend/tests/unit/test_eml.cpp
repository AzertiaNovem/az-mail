// Owner: WP-C — raw header parsing, RFC 2047 (iconv), Message-ID lists (tests/fixtures/*.eml).
#include "core/time.hpp"
#include "mail/eml.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

using namespace azm;
using namespace azm::mail::eml;

namespace {

std::string fixture(const std::string& name) {
  const std::filesystem::path p = std::filesystem::path(AZMAIL_TEST_FIXTURES) / name;
  return read_file_prefix(p);
}

int64_t utc(int y, int mo, int d, int h, int mi, int s) { return *utc_ms(y, mo, d, h, mi, s); }

}  // namespace

TEST_CASE("eml fixtures keep their line endings (tests/fixtures/.gitattributes)", "[eml]") {
  for (std::string name : {"gbk_subject.eml", "folded_references.eml", "missing_brackets.eml", "raw_8bit_gbk.eml"})
    CHECK(fixture(name).find("\r\n") != std::string::npos);
  for (std::string name : {"q_encoding.eml", "mixed_encodings.eml"}) CHECK(fixture(name).find('\r') == std::string::npos);
}

TEST_CASE("eml fixture: GBK / GB2312 encoded subject and names", "[eml]") {
  const auto h = parse_headers(fixture("gbk_subject.eml"));
  REQUIRE(h.subject.has_value());
  CHECK(*h.subject == "周报：第三季度总结");
  REQUIRE(h.from.size() == 1);
  CHECK(h.from[0].name == "张三");
  CHECK(h.from[0].email == "zhang@example.cn");
  CHECK(h.message_id == std::optional<std::string>("20261007120300.GA1234@mail.example.cn"));
  CHECK(h.date_ms == utc(2026, 10, 7, 12, 3, 0));
  CHECK_FALSE(h.in_reply_to.has_value());
  CHECK(h.references.empty());
  CHECK_FALSE(h.x_azmail_ref.has_value());
  CHECK_FALSE(h.auto_submitted.has_value());
  const auto block = parse_header_block(fixture("gbk_subject.eml"));
  CHECK(decode_rfc2047(*block.get("to")) == "运营组 <ops@team.example>");
}

TEST_CASE("eml fixture: folded References, In-Reply-To with comment, X-AzMail-Ref", "[eml]") {
  const std::string raw = fixture("folded_references.eml");
  const auto block = parse_header_block(raw);
  CHECK_FALSE(block.truncated);
  CHECK(block.get_all("subject").size() == 1);  // the body line "Subject: …" is not a header
  CHECK(*block.get("Subject") == "Re: Re: project\t plan");

  const auto h = extract_headers(block);
  CHECK(h.message_id == std::optional<std::string>("reply-3@example.org"));
  CHECK(h.in_reply_to == std::optional<std::string>("msg-2@team.example"));
  CHECK(h.references == std::vector<std::string>{"root-1@example.org", "msg-1@team.example",
                                                 "msg-2@team.example"});
  CHECK(h.x_azmail_ref == std::optional<std::string>("6f1c2c1e-3b8a-4c55-9a51-0d6c9d2a7b10"));
  CHECK(h.auto_submitted == std::optional<std::string>("auto-replied"));
  CHECK(h.date_ms == utc(2026, 10, 8, 9, 15, 42));
  REQUIRE(h.reply_to.size() == 2);
  CHECK(h.reply_to[0] == Address{"Li Si (work)", "lisi+work@example.org"});
  CHECK(h.reply_to[1].email == "other@example.org");
  REQUIRE(h.from.size() == 1);
  CHECK(h.from[0] == Address{"Li Si", "lisi@example.org"});
}

TEST_CASE("eml fixture: Q encoding, adjacent words, ISO-8859-1, LF line endings", "[eml]") {
  const std::string raw = fixture("q_encoding.eml");
  const auto h = parse_headers(raw);
  CHECK(h.subject == std::optional<std::string>("Café au lait ☕"));
  REQUIRE(h.from.size() == 1);
  CHECK(h.from[0].name == "René Dupont");
  CHECK(h.date_ms == utc(2026, 10, 7, 12, 0, 0));
  CHECK(h.message_id == std::optional<std::string>("q-1@example.fr"));
  const auto block = parse_header_block(raw);
  CHECK(decode_rfc2047(*block.get("X-Latin")) == "Grüße aus Köln");
}

TEST_CASE("eml fixture: mixed charsets, split multibyte character, broken words", "[eml]") {
  const std::string raw = fixture("mixed_encodings.eml");
  const auto h = parse_headers(raw);
  REQUIRE(h.from.size() == 1);
  CHECK(h.from[0].name == "陳大文");
  // The UTF-8 run is joined before conversion (the first word ends mid-character); whitespace
  // between adjacent encoded-words disappears even across charsets (RFC 2047 §6.2).
  CHECK(h.subject == std::optional<std::string>("Re: 中文 and café日本語"));
  CHECK(h.date_ms == utc(2026, 10, 8, 16, 2, 3));
  const auto block = parse_header_block(raw);
  CHECK(decode_rfc2047(*block.get("X-Unknown-Charset")) == "abc tail");
  CHECK(decode_rfc2047(*block.get("X-Broken")) == "=?utf-8?B?not base64!?= ok");
}

TEST_CASE("eml fixture: Message-IDs without brackets, duplicates, comments, quoted strings", "[eml]") {
  const auto h = parse_headers(fixture("missing_brackets.eml"));
  CHECK(h.message_id == std::optional<std::string>("abc123@host.example.net"));
  CHECK(h.in_reply_to == std::optional<std::string>("parent@host.example.net"));
  CHECK(h.references ==
        std::vector<std::string>{"a@x.example", "b@y.example", "c@z.example", "d@w.example"});
  CHECK_FALSE(h.date_ms.has_value());  // "garbage date"
  REQUIRE(h.from.size() == 1);
  CHECK(h.from[0] == Address{"", "dave@example.net"});
}

TEST_CASE("eml fixture: raw 8-bit header bytes fall back to GB18030, then Latin-1", "[eml]") {
  const std::string raw = fixture("raw_8bit_gbk.eml");
  const auto h = parse_headers(raw);
  CHECK(h.subject == std::optional<std::string>("会议纪要"));
  const auto block = parse_header_block(raw);
  CHECK(decode_rfc2047(*block.get("X-Latin1")) == "café");
}

TEST_CASE("eml: header block parsing edge cases", "[eml]") {
  SECTION("CRLF and LF, folding, trimming, case-insensitive lookup, duplicates") {
    const auto b = parse_header_block(
        "From someone@example Mon Oct  6 00:00:00 2026\n"  // mbox separator: skipped
        " stray continuation\r\n"                          // before any header: ignored
        "X-A: one\r\n"
        "x-a:two  \r\n"
        "Subject :  spaced\r\n"
        "  more\r\n"
        "Bad Header: x\r\n"  // space in the name: skipped, with its fold
        " folded-bad\r\n"
        "\r\n"
        "X-Body: no\r\n");
    CHECK(b.get_all("X-A") == std::vector<std::string>{"one", "two"});
    CHECK(b.get("x-a") == std::optional<std::string>("one"));
    CHECK(b.get("SUBJECT") == std::optional<std::string>("spaced  more"));
    CHECK_FALSE(b.get("X-Body").has_value());
    CHECK_FALSE(b.get("Bad Header").has_value());
    CHECK(b.headers.size() == 3);
    CHECK_FALSE(b.truncated);
  }
  SECTION("empty input / immediate blank line") {
    CHECK(parse_header_block("").headers.empty());
    CHECK(parse_header_block("\r\nSubject: x\r\n").headers.empty());
  }
  SECTION("byte limit") {
    const std::string big = "Subject: " + std::string(1000, 'a') + "\r\nX-Later: y\r\n\r\n";
    auto b = parse_header_block(big, 100);
    CHECK(b.truncated);
    CHECK(b.get("Subject")->size() == 91);  // only the first 100 bytes were looked at
    CHECK_FALSE(b.get("X-Later").has_value());
    b = parse_header_block("Subject: x\r\n\r\n" + std::string(1000, 'b'), 100);
    CHECK_FALSE(b.truncated);  // the block ended inside the limit
    CHECK(b.get("Subject") == std::optional<std::string>("x"));
  }
  SECTION("headers only, no terminating blank line") {
    const auto b = parse_header_block("A: 1\nB: 2");
    CHECK(b.get("B") == std::optional<std::string>("2"));
    CHECK_FALSE(b.truncated);
  }
}

TEST_CASE("eml: read_file_prefix", "[eml]") {
  const auto dir = std::filesystem::temp_directory_path() / "azmail-eml-prefix-test";
  std::filesystem::create_directories(dir);
  const auto f = dir / "x.eml";
  {
    std::ofstream o(f, std::ios::binary);
    o << std::string(2000, 'z');
  }
  CHECK(read_file_prefix(f, 100).size() == 100);
  CHECK(read_file_prefix(f).size() == 2000);
  CHECK_THROWS_AS(read_file_prefix(dir / "missing.eml"), std::runtime_error);
  std::filesystem::remove_all(dir);
}

TEST_CASE("eml: unfold", "[eml]") {
  CHECK(unfold("a\r\n b") == "a b");
  CHECK(unfold("a\n\tb\r\n  c") == "a\tb  c");
  CHECK(unfold("plain") == "plain");
}

TEST_CASE("eml: RFC 2047 decoding", "[eml]") {
  CHECK(decode_rfc2047("plain ascii") == "plain ascii");
  CHECK(decode_rfc2047("=?UTF-8?B?5Lit5paH?=") == "中文");
  CHECK(decode_rfc2047("=?utf-8?b?5Lit5paH") == "=?utf-8?b?5Lit5paH");  // unterminated → literal
  CHECK(decode_rfc2047("=?utf-8?x?abc?=") == "=?utf-8?x?abc?=");        // unknown encoding
  CHECK(decode_rfc2047("=?UTF-8?Q?a_b?=   =?UTF-8?Q?c?=") == "a bc");   // WSP between words dropped
  CHECK(decode_rfc2047("=?UTF-8?Q?a?= x =?UTF-8?Q?b?=") == "a x b");     // text between words kept
  CHECK(decode_rfc2047("Re: =?UTF-8?Q?x?=  ") == "Re: x  ");
  CHECK(decode_rfc2047("=?UTF-8*zh?Q?=E4=B8=AD?=") == "中");             // RFC 2231 language tag
  CHECK(decode_rfc2047("=?utf-8?Q?=ZZ=4?=") == "=ZZ=4");                 // malformed escapes kept
  CHECK(decode_rfc2047("=?UTF-8?B?5Lit\r\n 5paH?=") == "中文");  // lenient: fold inside a B word
  CHECK(decode_rfc2047("a=?UTF-8?Q?b?=c") == "abc");                    // lenient: no separating WSP
  // Invalid bytes in a UTF-8 word → U+FFFD replacement.
  CHECK(decode_rfc2047("=?utf-8?Q?=FF?=") == "\xEF\xBF\xBD");
  // Folded value.
  CHECK(decode_rfc2047("=?UTF-8?Q?x?=\r\n =?UTF-8?Q?y?=") == "xy");
  CHECK(decode_rfc2047("") == "");
}

TEST_CASE("eml: charset conversion via iconv", "[eml]") {
  CHECK(to_utf8("\xD6\xD0\xCE\xC4", "GB2312") == std::optional<std::string>("中文"));
  CHECK(to_utf8("\xD6\xD0\xCE\xC4", "gbk") == std::optional<std::string>("中文"));
  CHECK(to_utf8("\xD6\xD0\xCE\xC4", " \"GB18030\" ") == std::optional<std::string>("中文"));
  // GB18030 four-byte sequence (U+20AC EURO SIGN is 0xA2E3 in GB18030; U+00E9 is 4-byte 0x8130BD...)
  CHECK(to_utf8("\x81\x30\x81\x30", "gb18030") == std::optional<std::string>("\xC2\x80"));
  CHECK(to_utf8("\xA4\xA4\xA4\xE5", "big5") == std::optional<std::string>("中文"));
  CHECK(to_utf8("\x93\xFA\x96\x7B", "Shift_JIS") == std::optional<std::string>("日本"));
  CHECK(to_utf8("\xE9", "iso-8859-1") == std::optional<std::string>("é"));
  CHECK(to_utf8("\xE9", "latin1") == std::optional<std::string>("é"));
  CHECK(to_utf8("\xB9", "ISO-8859-2") == std::optional<std::string>("š"));
  CHECK(to_utf8("\x80", "windows-1252") == std::optional<std::string>("€"));
  CHECK(to_utf8("\x80", "cp1252") == std::optional<std::string>("€"));
  CHECK(to_utf8("caf\xC3\xA9", "utf-8") == std::optional<std::string>("café"));
  CHECK_FALSE(to_utf8("\xFF\xFE", "utf-8").has_value());  // invalid UTF-8
  CHECK_FALSE(to_utf8("\xE9", "us-ascii").has_value());
  CHECK_FALSE(to_utf8("abc", "x-no-such-charset").has_value());
  CHECK_FALSE(to_utf8("abc", "").has_value());
  CHECK_FALSE(to_utf8("\xD6", "gbk").has_value());  // truncated double-byte sequence
}

TEST_CASE("eml: Message-ID lists", "[eml]") {
  CHECK(parse_msgid_list("<a@b> <c@d>") == std::vector<std::string>{"a@b", "c@d"});
  CHECK(parse_msgid_list("<a@b>,<c@d>;<a@b>") == std::vector<std::string>{"a@b", "c@d"});
  CHECK(parse_msgid_list("< a@b >") == std::vector<std::string>{"a@b"});
  CHECK(parse_msgid_list("<a@\r\n b>") == std::vector<std::string>{"a@b"});  // folded inside
  CHECK(parse_msgid_list("(c (nested) <x@y>) <a@b>") == std::vector<std::string>{"a@b"});
  CHECK(parse_msgid_list("bare@id other") == std::vector<std::string>{"bare@id"});
  CHECK(parse_msgid_list("<unterminated@x") == std::vector<std::string>{"unterminated@x"});
  CHECK(parse_msgid_list("<> @ x@ @y").empty());
  CHECK(parse_msgid_list("").empty());
  CHECK(parse_msgid("<first@x> <second@x>") == std::optional<std::string>("first@x"));
  CHECK_FALSE(parse_msgid("no ids here").has_value());
}

TEST_CASE("eml: Auto-Submitted and X-AzMail-Ref normalization", "[eml]") {
  auto h = parse_headers("Auto-Submitted: no\r\nX-AzMail-Ref:\r\n\r\n");
  CHECK(h.auto_submitted == std::optional<std::string>("no"));
  CHECK_FALSE(h.x_azmail_ref.has_value());
  h = parse_headers("Auto-Submitted: (comment) AUTO-GENERATED\r\n\r\n");
  CHECK(h.auto_submitted == std::optional<std::string>("auto-generated"));
  h = parse_headers("Subject: =?UTF-8?B?IOS4reaWhyA=?=\r\n\r\n");
  CHECK(h.subject == std::optional<std::string>("中文"));  // trimmed after decoding
}
