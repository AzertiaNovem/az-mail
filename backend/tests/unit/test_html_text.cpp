// Owner: WP-B — html_to_text, strip_quoted_text, make_snippet and the HTML scanner.
#include "core/strings.hpp"
#include "mail/html_scan.hpp"
#include "mail/html_text.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace azm;
using namespace azm::mail;

TEST_CASE("html_to_text: blocks, br and whitespace collapse", "[html_text]") {
  CHECK(html_to_text("") == "");
  CHECK(html_to_text("plain") == "plain");
  CHECK(html_to_text("<p>Hello</p><p>World</p>") == "Hello\n\nWorld");
  CHECK(html_to_text("<div>a</div><div>b</div>") == "a\nb");
  CHECK(html_to_text("line1<br>line2<br/>line3<BR />x") == "line1\nline2\nline3\nx");
  CHECK(html_to_text("  lots   of \n\t  space  ") == "lots of space");
  CHECK(html_to_text("<p>  padded  </p>") == "padded");
  CHECK(html_to_text("<h1>Title</h1>body") == "Title\n\nbody");
  CHECK(html_to_text("a<br><br><br><br><br>b") == "a\n\n\nb");  // ≤ 2 blank lines
  CHECK(html_to_text("<span>in</span><b>line</b> <i>tags</i>") == "inline tags");
  CHECK(html_to_text("<table><tr><td>a</td><td>b</td></tr><tr><td>c</td></tr></table>") == "a b\nc");
}

TEST_CASE("html_to_text: head, script, style, comments are dropped", "[html_text]") {
  CHECK(html_to_text("<html><head><title>T</title><style>p{color:red}</style></head>"
                     "<body><p>Body</p></body></html>") == "Body");
  CHECK(html_to_text("a<script>alert('<b>x</b>')</script>b") == "ab");
  CHECK(html_to_text("a<!-- hidden <p>x</p> -->b") == "ab");
  CHECK(html_to_text("<!DOCTYPE html><p>x</p>") == "x");
  // A head that is never closed ends at <body>.
  CHECK(html_to_text("<head><meta charset=utf-8><body>正文</body>") == "正文");
  CHECK(html_to_text("<style>unterminated") == "");
  CHECK(html_to_text("x<STYLE type=text/css>a{}</STYLE>y") == "xy");
}

TEST_CASE("html_to_text: lists", "[html_text]") {
  CHECK(html_to_text("<ul><li>one</li><li>two</li></ul>") == "- one\n- two");
  CHECK(html_to_text("<ol><li>a</li><li>b</li><li>c</li></ol>") == "1. a\n2. b\n3. c");
  CHECK(html_to_text("<p>intro</p><ul><li>x</li></ul><p>end</p>") == "intro\n\n- x\n\nend");
}

TEST_CASE("html_to_text: links", "[html_text]") {
  CHECK(html_to_text("<a href=\"https://example.com/a\">click</a>") == "click <https://example.com/a>");
  CHECK(html_to_text("<a href=\"https://example.com\">https://example.com</a>") == "https://example.com");
  CHECK(html_to_text("<a href=\"https://example.com/\">example.com</a>") == "example.com");
  CHECK(html_to_text("<a href=\"mailto:a@b.cn\">a@b.cn</a>") == "a@b.cn");
  CHECK(html_to_text("<a href=\"mailto:a@b.cn?subject=x\">联系我</a>") == "联系我 <mailto:a@b.cn?subject=x>");
  CHECK(html_to_text("<a href=\"#top\">top</a>") == "top");
  CHECK(html_to_text("<a href=\"javascript:void(0)\">js</a>") == "js");
  CHECK(html_to_text("<a href=\"https://x.cn/\"><img src=cid:a></a>") == "");  // image-only link
  CHECK(html_to_text("<a href=\"https://x.cn/?a=1&amp;b=2\">q</a>") == "q <https://x.cn/?a=1&b=2>");
}

TEST_CASE("html_to_text: entities", "[html_text]") {
  CHECK(html_to_text("a &amp; b &lt;c&gt; &quot;d&quot; &#39;e&#39;") == "a & b <c> \"d\" 'e'");
  CHECK(html_to_text("&#20013;&#x6587;") == "中文");
  CHECK(html_to_text("&copy; 2026 &mdash; &hellip;") == "© 2026 — …");
  CHECK(html_to_text("x&nbsp;&nbsp;y") == "x y");
  CHECK(html_to_text("&unknown; &amp") == "&unknown; &");
  CHECK(html_to_text("&#0;&#xD800;&#x110000;") == "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD");
  CHECK(html_to_text("&#150;") == "–");  // Windows-1252 remap
  CHECK(html_to_text("&#;&#x;") == "&#;&#x;");
}

TEST_CASE("html_to_text: Chinese, pre, malformed input", "[html_text]") {
  CHECK(html_to_text("<p>你好，世界！</p><p>第二段</p>") == "你好，世界！\n\n第二段");
  CHECK(html_to_text("<pre>  keep\n    indent</pre>after") == "  keep\n    indent\n\nafter");
  CHECK(html_to_text("<p>x</p><pre>a  b</pre>") == "x\n\na  b");
  CHECK(html_to_text("a < b and c > d") == "a < b and c > d");
  CHECK(html_to_text("<p>unclosed <b>bold") == "unclosed bold");
  CHECK(html_to_text("text <a href=\"x") == "text");
  CHECK(html_to_text("<3 love") == "<3 love");
  const std::string bad = "\xFF\xFEok";
  const std::string out = html_to_text(bad);
  CHECK(utf8_valid(out));
  CHECK(out.find("ok") != std::string::npos);
}

TEST_CASE("strip_quoted_text: English and Chinese attributions", "[html_text]") {
  CHECK(strip_quoted_text("Thanks!\n\nOn Tue, Oct 7, 2026 at 8:03 PM Bob <bob@x.com> wrote:\n> old\n> text") ==
        "Thanks!");
  CHECK(strip_quoted_text("好的\n\n在 2026年10月7日 周三 20:03，张三 <a@b.cn> 写道：\n> 原文") == "好的");
  CHECK(strip_quoted_text("ok\n在 2026年10月7日 周三 20:03，张三 <a@b.cn> 写道:\n原文") == "ok");
  // Wrapped attribution (Gmail breaks long ones).
  CHECK(strip_quoted_text("reply\nOn Tue, Oct 7, 2026 at 8:03 PM Very Long Name\n<long@x.com> wrote:\n\nquoted") ==
        "reply");
  CHECK(strip_quoted_text("top\n> a\nmiddle\n> b\nbottom") == "top\nmiddle\nbottom");
  CHECK(strip_quoted_text("first\nBob wrote:\n> quoted") == "first");
  CHECK(strip_quoted_text("no quotes here\nsecond line") == "no quotes here\nsecond line");
}

TEST_CASE("strip_quoted_text: Outlook / Foxmail separators", "[html_text]") {
  CHECK(strip_quoted_text("Answer\n-----Original Message-----\nFrom: a\nSent: b\nTo: c\n\nold") == "Answer");
  CHECK(strip_quoted_text("回答\n------------------ 原始邮件 ------------------\n发件人: x") == "回答");
  CHECK(strip_quoted_text("Hi\n\nFrom: Bob <b@x.com>\nSent: Tuesday\nTo: me\nSubject: x\n\nold") == "Hi");
  CHECK(strip_quoted_text("你好\n发件人: 张三\n发送时间: 2026年10月7日\n收件人: 李四\n主题: 周报\nold") == "你好");
  CHECK(strip_quoted_text("Body\n________________________________\nFrom: X\nSent: Y\nTo: Z\nold") == "Body");
  // A lone "From:" line without the other headers is not a quote.
  CHECK(strip_quoted_text("From: the team\nwelcome") == "From: the team\nwelcome");
}

TEST_CASE("make_snippet", "[html_text]") {
  CHECK(make_snippet("Hello\n\n  world  ", false) == "Hello world");
  CHECK(make_snippet("<p>Hi <b>there</b></p><p>second</p>", true) == "Hi there second");
  CHECK(make_snippet("Thanks\nOn Mon, Bob wrote:\n> old", false) == "Thanks");
  CHECK(make_snippet("好的，收到\n\n在 2026年10月7日，张三 写道：\n> 原文", false) == "好的，收到");
  CHECK(make_snippet("Body text\n-- \nBob\nCEO", false) == "Body text");
  // Gmail quote container in HTML.
  CHECK(make_snippet("<div>New text</div><div class=\"gmail_quote\"><div>On x wrote:</div>"
                     "<blockquote class=\"gmail_quote\">old <div>nested</div></blockquote></div><div>after</div>",
                     true) == "New text after");
  CHECK(make_snippet("<p>reply</p><blockquote type=\"cite\">quoted</blockquote>", true) == "reply");
  CHECK(make_snippet("<p>Outlook reply</p><div id=\"divRplyFwdMsg\">From: x</div><div>old body</div>", true) ==
        "Outlook reply");
  // Only a quote: fall back to the quoted text instead of an empty snippet.
  CHECK(make_snippet("> only quoted", false) == "> only quoted");
  // Truncation counts code points and never splits UTF-8.
  std::string zh;
  for (int i = 0; i < 300; ++i) zh += "中";
  const std::string s = make_snippet(zh, false);
  CHECK(utf8_length(s) == kSnippetChars);
  CHECK(utf8_valid(s));
  CHECK(make_snippet("abcdef", false, 3) == "abc");
  CHECK(make_snippet("", false) == "");
  CHECK(make_snippet("<style>x</style>", true) == "");
}

TEST_CASE("html scanner: tags, attributes and entities", "[html_text][html_scan]") {
  const std::string h = "<img SRC='a.png' alt=\"x > y\" data-x=1 hidden/>";
  auto m = html::parse_markup(h, 0);
  REQUIRE(m);
  REQUIRE(m->kind == html::MarkupKind::Tag);
  const auto& t = m->tag;
  CHECK(t.name == "img");
  CHECK(t.self_closing);
  CHECK(t.end == h.size());
  REQUIRE(t.attrs.size() == 4);
  CHECK(t.attrs[0].name == "src");
  CHECK(t.attrs[0].raw_value(h) == "a.png");
  CHECK(t.attrs[1].raw_value(h) == "x > y");
  CHECK(t.attrs[2].raw_value(h) == "1");
  CHECK_FALSE(t.attrs[3].has_value);
  CHECK(t.find("alt") != nullptr);
  CHECK(t.find("nope") == nullptr);

  CHECK_FALSE(html::parse_markup("< b", 0));
  CHECK_FALSE(html::parse_markup("<", 0));
  auto c = html::parse_markup("<!-- x -->y", 0);
  REQUIRE(c);
  CHECK(c->kind == html::MarkupKind::Comment);
  CHECK(c->end == 10);
  auto close = html::parse_markup("</DIV >", 0);
  REQUIRE(close);
  CHECK(close->tag.closing);
  CHECK(close->tag.name == "div");
  auto partial = html::parse_markup("<a href=\"x", 0);
  REQUIRE(partial);
  CHECK_FALSE(partial->tag.complete);

  CHECK(html::find_close_tag("abc</Script >", 0, "script") == 3);
  CHECK(html::find_close_tag("</scripts></script>", 0, "script") == 10);
  CHECK(html::decode_entities("&lt;&gt;&amp;&apos;&eacute;") == "<>&'é");
  CHECK(html::escape_attr("a\"b<c>&") == "a&quot;b&lt;c&gt;&amp;");
}
