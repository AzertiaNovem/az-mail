#include "core/strings.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace azm;

TEST_CASE("ascii helpers", "[strings]") {
  CHECK(trim("  a b \t\r\n") == "a b");
  CHECK(trim("") == "");
  CHECK(trim(" \n ") == "");
  CHECK(to_lower_ascii("HeLLo 中文") == "hello 中文");
  CHECK(to_upper_ascii("abc") == "ABC");
  CHECK(iequals("Content-Type", "content-type"));
  CHECK_FALSE(iequals("abc", "abcd"));
  CHECK(istarts_with("Re: hello", "RE:"));
  CHECK_FALSE(istarts_with("R", "Re:"));
  CHECK(iends_with("photo.JPG", ".jpg"));
  CHECK(is_ascii("abc"));
  CHECK_FALSE(is_ascii("中"));
}

TEST_CASE("split / join / replace_all", "[strings]") {
  CHECK(split("a,,b", ',') == std::vector<std::string>{"a", "", "b"});
  CHECK(split("a,,b", ',', true) == std::vector<std::string>{"a", "b"});
  CHECK(split("", ',') == std::vector<std::string>{""});
  CHECK(split("", ',', true).empty());
  CHECK(split("x,", ',') == std::vector<std::string>{"x", ""});
  CHECK(join(std::vector<std::string>{"a", "b", "c"}, ", ") == "a, b, c");
  CHECK(join(std::vector<std::string_view>{"x"}, "-") == "x");
  CHECK(join(std::vector<std::string>{}, "-") == "");
  CHECK(replace_all("a.b.c", ".", "::") == "a::b::c");
  CHECK(replace_all("aaa", "aa", "b") == "ba");
  CHECK(replace_all("abc", "", "x") == "abc");
}

TEST_CASE("utf8_valid", "[strings][utf8]") {
  CHECK(utf8_valid(""));
  CHECK(utf8_valid("hello 中文 😀"));
  CHECK_FALSE(utf8_valid("\xC3\x28"));          // bad continuation
  CHECK_FALSE(utf8_valid("\xC0\xAF"));          // overlong '/'
  CHECK_FALSE(utf8_valid("\xE0\x80\xAF"));      // overlong 3-byte
  CHECK_FALSE(utf8_valid("\xED\xA0\x80"));      // surrogate U+D800
  CHECK_FALSE(utf8_valid("\xF4\x90\x80\x80"));  // > U+10FFFF
  CHECK_FALSE(utf8_valid("\xE4\xB8"));          // truncated
  CHECK_FALSE(utf8_valid("\xFF"));
}

TEST_CASE("utf8_sanitize replaces maximal invalid subparts", "[strings][utf8]") {
  CHECK(utf8_sanitize("a\xFF" "b") == "a\xEF\xBF\xBD" "b");
  CHECK(utf8_sanitize("\xE4\xB8") == "\xEF\xBF\xBD");               // one U+FFFD for the truncated seq
  CHECK(utf8_sanitize("\xF0\x9F\x98x") == "\xEF\xBF\xBDx");
  CHECK(utf8_sanitize("\xC0\xAF") == "\xEF\xBF\xBD\xEF\xBF\xBD");   // each byte invalid
  CHECK(utf8_sanitize("中文ok") == "中文ok");
  CHECK(utf8_valid(utf8_sanitize("\x80\x81\xFE mixed \xE4\xB8\xAD")));
}

TEST_CASE("utf8_truncate never splits a code point", "[strings][utf8]") {
  const std::string s = "中文字";  // 3 x 3 bytes
  CHECK(utf8_truncate(s, 9) == s);
  CHECK(utf8_truncate(s, 100) == s);
  CHECK(utf8_truncate(s, 8) == "中文");
  CHECK(utf8_truncate(s, 6) == "中文");
  CHECK(utf8_truncate(s, 4) == "中");
  CHECK(utf8_truncate(s, 2) == "");
  CHECK(utf8_truncate(s, 0) == "");
  CHECK(utf8_truncate("ab中", 3) == "ab");
  CHECK(utf8_truncate("😀x", 3) == "");
  CHECK(utf8_truncate("😀x", 4) == "😀");
  for (std::size_t n = 0; n <= s.size(); ++n) CHECK(utf8_valid(utf8_truncate(s, n)));
}

TEST_CASE("utf8_length", "[strings][utf8]") {
  CHECK(utf8_length("") == 0);
  CHECK(utf8_length("中a😀") == 3);
  CHECK(utf8_length("\xFF\xFF") == 2);
}

TEST_CASE("url encode / decode", "[strings]") {
  CHECK(url_encode("a b/中") == "a%20b%2F%E4%B8%AD");
  CHECK(url_encode("AZaz09-._~") == "AZaz09-._~");
  CHECK(url_encode("a+b=c&d") == "a%2Bb%3Dc%26d");
  CHECK(url_decode("a%20b%2F%e4%b8%ad").value() == "a b/中");
  CHECK(url_decode("a+b").value() == "a+b");
  CHECK(url_decode("a+b", true).value() == "a b");
  CHECK_FALSE(url_decode("%zz").has_value());
  CHECK_FALSE(url_decode("%4").has_value());
  CHECK_FALSE(url_decode("abc%").has_value());
  const std::string tricky = "报告 (final) 100%.pdf";
  CHECK(url_decode(url_encode(tricky)).value() == tricky);
}

TEST_CASE("rfc5987 and content_disposition", "[strings]") {
  CHECK(rfc5987_encode("报告.pdf") == "UTF-8''%E6%8A%A5%E5%91%8A.pdf");
  CHECK(rfc5987_encode("a b'c") == "UTF-8''a%20b%27c");

  CHECK(content_disposition("attachment", "报告.pdf") ==
        "attachment; filename=\"__.pdf\"; filename*=UTF-8''%E6%8A%A5%E5%91%8A.pdf");
  CHECK(content_disposition("inline", "photo.png") == "inline; filename=\"photo.png\"");
  CHECK(content_disposition("attachment", "Q3 report.pdf") ==
        "attachment; filename=\"Q3 report.pdf\"");
  // Quotes, backslashes, path separators, CR/LF are neutralised.
  CHECK(content_disposition("attachment", "a\"b\\c\r\n/d.txt") ==
        "attachment; filename=\"a_b_c_d.txt\"; filename*=UTF-8''a%22b_c_d.txt");
  CHECK(content_disposition("attachment", "100%.txt") ==
        "attachment; filename=\"100_.txt\"; filename*=UTF-8''100%25.txt");
  // Unknown dispositions fall back to attachment; empty names become "file".
  CHECK(content_disposition("evil\r\nX-Injected: 1", "x.txt") == "attachment; filename=\"x.txt\"");
  CHECK(content_disposition("INLINE", "") == "inline; filename=\"file\"");
}

TEST_CASE("html_escape", "[strings]") {
  CHECK(html_escape("<a href=\"x\">Tom & 'Jerry'</a>") ==
        "&lt;a href=&quot;x&quot;&gt;Tom &amp; &#39;Jerry&#39;&lt;/a&gt;");
  CHECK(html_escape("中文") == "中文");
}
