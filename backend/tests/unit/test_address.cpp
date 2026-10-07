#include "core/address.hpp"

#include <catch2/catch_test_macros.hpp>

using azm::Address;
using azm::format_address;
using azm::parse_address;
using azm::parse_address_list;

namespace {
Address A(std::string name, std::string email) { return Address{std::move(name), std::move(email)}; }
}  // namespace

TEST_CASE("parse_address: single mailbox forms", "[address]") {
  CHECK(parse_address("a@b.com") == A("", "a@b.com"));
  CHECK(parse_address("  a@b.com  ") == A("", "a@b.com"));
  CHECK(parse_address("Alice <alice@example.com>") == A("Alice", "alice@example.com"));
  CHECK(parse_address("<alice@example.com>") == A("", "alice@example.com"));
  CHECK(parse_address("Mary Ann Smith <mas@x.com>") == A("Mary Ann Smith", "mas@x.com"));
  CHECK(parse_address("Alice@Example.COM") == A("", "Alice@Example.COM"));  // case preserved
  CHECK(parse_address("Alice < alice@example.com >") == A("Alice", "alice@example.com"));
  CHECK(parse_address("Alice\r\n <a@x.com>") == A("Alice", "a@x.com"));  // folded header
}

TEST_CASE("parse_address: quoted display names", "[address]") {
  CHECK(parse_address("\"张三 (运营)\" <zs@team.com>") == A("张三 (运营)", "zs@team.com"));
  CHECK(parse_address("张三 <zs@team.com>") == A("张三", "zs@team.com"));
  CHECK(parse_address("\"He said \\\"hi\\\"\" <q@x.com>") == A("He said \"hi\"", "q@x.com"));
  CHECK(parse_address("\"\" <a@b.com>") == A("", "a@b.com"));
  CHECK(parse_address("John \"Q\" Public <jqp@x.com>") == A("John Q Public", "jqp@x.com"));
  CHECK(parse_address("'Single Quoted' <sq@x.com>") == A("Single Quoted", "sq@x.com"));
  CHECK(parse_address("\"back\\\\slash\" <b@x.com>") == A("back\\slash", "b@x.com"));
  // Encoded words are left as-is here (RFC 2047 decoding lives in mail/eml).
  CHECK(parse_address("=?UTF-8?B?5byg5LiJ?= <zs@x.com>") == A("=?UTF-8?B?5byg5LiJ?=", "zs@x.com"));
  // Name equal to the address is dropped.
  CHECK(parse_address("a@b.com <a@b.com>") == A("", "a@b.com"));
}

TEST_CASE("parse_address: comments", "[address]") {
  CHECK(parse_address("a@b.com (Alice)") == A("Alice", "a@b.com"));
  CHECK(parse_address("a@b.com (Alice (Ops))") == A("Alice (Ops)", "a@b.com"));
  CHECK(parse_address("Alice <alice@example.com> (work)") == A("Alice", "alice@example.com"));
  CHECK(parse_address("<a@b.com> (Bob)") == A("Bob", "a@b.com"));
}

TEST_CASE("parse_address: odd but seen in the wild", "[address]") {
  CHECK(parse_address("<mailto:a@b.com>") == A("", "a@b.com"));
  CHECK(parse_address("<@relay.example:a@b.com>") == A("", "a@b.com"));  // obsolete route
  CHECK(parse_address("\"john doe\"@example.com") == A("", "\"john doe\"@example.com"));
  CHECK(parse_address("Alice <alice@example.com") == A("Alice", "alice@example.com"));  // unterminated
}

TEST_CASE("parse_address: invalid or not exactly one", "[address]") {
  CHECK_FALSE(parse_address("not an address"));
  CHECK_FALSE(parse_address(""));
  CHECK_FALSE(parse_address("a@b"));  // no dot in domain
  CHECK_FALSE(parse_address("Alice <>"));
  CHECK_FALSE(parse_address("a@b.com, c@d.com"));
  CHECK_FALSE(parse_address("@b.com"));
  CHECK_FALSE(parse_address("a@"));
}

TEST_CASE("parse_address_list: separators inside and outside quotes", "[address]") {
  auto l = parse_address_list("\"Smith, John\" <js@x.com>, jane@x.com");
  REQUIRE(l.size() == 2);
  CHECK(l[0] == A("Smith, John", "js@x.com"));
  CHECK(l[1] == A("", "jane@x.com"));

  l = parse_address_list("\"a;b\" <ab@x.com>; c@x.com");
  REQUIRE(l.size() == 2);
  CHECK(l[0] == A("a;b", "ab@x.com"));
  CHECK(l[1] == A("", "c@x.com"));

  l = parse_address_list("Bob <bob@x.com>, , ,");
  REQUIRE(l.size() == 1);
  CHECK(l[0] == A("Bob", "bob@x.com"));

  l = parse_address_list("\"张三 (运营)\" <zs@team.com>, 李四 <ls@team.com>,王五<ww@team.com>");
  REQUIRE(l.size() == 3);
  CHECK(l[0] == A("张三 (运营)", "zs@team.com"));
  CHECK(l[1] == A("李四", "ls@team.com"));
  CHECK(l[2] == A("王五", "ww@team.com"));
}

TEST_CASE("parse_address_list: groups are flattened", "[address]") {
  auto l = parse_address_list("team: a@b.com, Carol <c@d.com>;");
  REQUIRE(l.size() == 2);
  CHECK(l[0] == A("", "a@b.com"));
  CHECK(l[1] == A("Carol", "c@d.com"));

  l = parse_address_list("team: a@b.com, c@d.com;, e@f.com");
  REQUIRE(l.size() == 3);
  CHECK(l[2] == A("", "e@f.com"));

  CHECK(parse_address_list("undisclosed-recipients:;").empty());
  l = parse_address_list("\"Team: Ops\" <ops@x.com>");  // colon inside quotes is not a group
  REQUIRE(l.size() == 1);
  CHECK(l[0] == A("Team: Ops", "ops@x.com"));
}

TEST_CASE("parse_address_list: invalid entries are skipped", "[address]") {
  auto l = parse_address_list("good@x.com, bad@, @bad.com, x@y.com, nonsense");
  REQUIRE(l.size() == 2);
  CHECK(l[0].email == "good@x.com");
  CHECK(l[1].email == "x@y.com");
  CHECK(parse_address_list("").empty());
}

TEST_CASE("format_address quoting", "[address]") {
  CHECK(format_address(A("", "a@b.com")) == "a@b.com");
  CHECK(format_address(A("Alice", "alice@example.com")) == "Alice <alice@example.com>");
  CHECK(format_address(A("张三", "zs@team.com")) == "张三 <zs@team.com>");
  CHECK(format_address(A("张三 (运营)", "zs@team.com")) == "\"张三 (运营)\" <zs@team.com>");
  CHECK(format_address(A("John Q. Public", "j@x.com")) == "\"John Q. Public\" <j@x.com>");
  CHECK(format_address(A("Smith, John", "j@x.com")) == "\"Smith, John\" <j@x.com>");
  CHECK(format_address(A("He said \"hi\"", "q@x.com")) == "\"He said \\\"hi\\\"\" <q@x.com>");
  CHECK(format_address(A("a\\b", "q@x.com")) == "\"a\\\\b\" <q@x.com>");
  // Header injection attempts are neutralised.
  CHECK(format_address(A("Evil\r\nBcc: x@y.com", "e@x.com")) == "\"Evil Bcc: x@y.com\" <e@x.com>");
  CHECK(format_address(A("   ", "a@b.com")) == "a@b.com");
}

TEST_CASE("format/parse roundtrip", "[address]") {
  const std::vector<Address> samples = {
      A("张三 (运营)", "zs@team.com"), A("He said \"hi\"", "q@x.com"), A("Smith, John", "j@x.com"),
      A("a\\b", "q@x.com"),           A("Plain Name", "p@x.com"),     A("", "solo@x.com"),
      A("周报;会议", "m@x.com")};
  for (const auto& a : samples) CHECK(parse_address(format_address(a)) == a);
  CHECK(parse_address_list(azm::format_address_list(samples)) == samples);
}

TEST_CASE("normalize_email / local_part / domain_of", "[address]") {
  CHECK(azm::normalize_email("  Alice+Tag@Example.COM ") == "alice+tag@example.com");
  CHECK(azm::normalize_email("Alice+Tag@Example.COM", true) == "alice@example.com");
  CHECK(azm::normalize_email("+x@y.com", true) == "+x@y.com");
  CHECK(azm::normalize_email("a+b+c@y.com", true) == "a@y.com");
  CHECK(azm::local_part("a.b@x.com") == "a.b");
  CHECK(azm::domain_of("a.b@x.com") == "x.com");
  CHECK(azm::domain_of("nodomain").empty());
}

TEST_CASE("is_valid_email", "[address]") {
  CHECK(azm::is_valid_email("a.b-c+d@sub.example.co"));
  CHECK(azm::is_valid_email("用户@例子.中国"));
  CHECK(azm::is_valid_email("\"john doe\"@example.com"));
  CHECK(azm::is_valid_email("o'brien@example.ie"));
  CHECK_FALSE(azm::is_valid_email(""));
  CHECK_FALSE(azm::is_valid_email(".a@b.com"));
  CHECK_FALSE(azm::is_valid_email("a.@b.com"));
  CHECK_FALSE(azm::is_valid_email("a..b@c.com"));
  CHECK_FALSE(azm::is_valid_email("a@-b.com"));
  CHECK_FALSE(azm::is_valid_email("a@b..com"));
  CHECK_FALSE(azm::is_valid_email("a@b.com."));
  CHECK_FALSE(azm::is_valid_email("a b@c.com"));
  CHECK_FALSE(azm::is_valid_email("a@b_c.com"));
  CHECK_FALSE(azm::is_valid_email("a@localhost"));
  CHECK_FALSE(azm::is_valid_email(std::string(65, 'a') + "@x.com"));
}
