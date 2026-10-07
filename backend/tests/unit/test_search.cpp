// Owner: WP-B — search grammar (parse_search / compile_search) and execution against FTS5.
#include "mail/mailbox.hpp"
#include "mail/search.hpp"
#include "mail_fixtures.hpp"
#include "test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <set>
#include <string>
#include <variant>
#include <vector>

using namespace azm;
using namespace azm::mail;
using namespace azm::test::mailfx;

namespace {

ParsedQuery P(std::string_view q) { return parse_search(q); }

bool where_has(const SqlFilter& f, std::string_view s) { return f.where.find(s) != std::string::npos; }

std::vector<std::string> string_binds(const SqlFilter& f) {
  std::vector<std::string> out;
  for (const auto& b : f.binds)
    if (const auto* s = std::get_if<std::string>(&b)) out.push_back(*s);
  return out;
}

std::vector<int64_t> int_binds(const SqlFilter& f) {
  std::vector<int64_t> out;
  for (const auto& b : f.binds)
    if (const auto* i = std::get_if<int64_t>(&b)) out.push_back(*i);
  return out;
}

int count_placeholders(std::string_view sql) {
  return static_cast<int>(std::count(sql.begin(), sql.end(), '?'));
}

}  // namespace

// =============================================================================================
// Tokenizer
// =============================================================================================

TEST_CASE("parse_search: bare terms, phrases, negation", "[search]") {
  auto q = P("hello world");
  REQUIRE(q.terms.size() == 2);
  CHECK(q.terms[0].field.empty());
  CHECK(q.terms[0].value == "hello");
  CHECK(q.terms[1].value == "world");

  q = P("\"exact phrase\" -spam");
  REQUIRE(q.terms.size() == 2);
  CHECK(q.terms[0].phrase);
  CHECK(q.terms[0].value == "exact phrase");
  CHECK(q.terms[1].negated);
  CHECK(q.terms[1].value == "spam");

  q = P("-\"no way\"");
  REQUIRE(q.terms.size() == 1);
  CHECK(q.terms[0].negated);
  CHECK(q.terms[0].phrase);
  CHECK(q.terms[0].value == "no way");

  q = P("\"unbalanced quote");
  REQUIRE(q.terms.size() == 1);
  CHECK(q.terms[0].value == "unbalanced quote");

  CHECK(P("").terms.empty());
  CHECK(P("   \t ").terms.empty());
  CHECK(P("\"\"").terms.empty());
  CHECK(P("- -- &").terms.empty());  // punctuation-only terms carry nothing searchable
}

TEST_CASE("parse_search: operators", "[search]") {
  auto q = P("from:alice@x.com to:bob subject:\"weekly report\" has:attachment");
  REQUIRE(q.terms.size() == 4);
  CHECK(q.terms[0].field == "from");
  CHECK(q.terms[0].value == "alice@x.com");
  CHECK(q.terms[1].field == "to");
  CHECK(q.terms[2].field == "subject");
  CHECK(q.terms[2].value == "weekly report");
  CHECK(q.terms[2].phrase);
  CHECK(q.terms[3].field == "has");

  q = P("FROM:Alice");  // operator names are case-insensitive
  REQUIRE(q.terms.size() == 1);
  CHECK(q.terms[0].field == "from");
  CHECK(q.terms[0].value == "Alice");

  q = P("-label:work");
  REQUIRE(q.terms.size() == 1);
  CHECK(q.terms[0].negated);
  CHECK(q.terms[0].field == "label");

  q = P("foo:bar http://x.cn");  // unknown operators are plain text
  REQUIRE(q.terms.size() == 2);
  CHECK(q.terms[0].field.empty());
  CHECK(q.terms[0].value == "foo:bar");
  CHECK(q.terms[1].value == "http://x.cn");

  CHECK(P("from:").terms.empty());  // operator without a value is ignored
  CHECK(P("subject:\"\"").terms.empty());
}

TEST_CASE("parse_search: OR groups", "[search]") {
  auto q = P("apple OR banana cherry");
  REQUIRE(q.terms.size() == 3);
  CHECK(q.terms[0].or_group != 0);
  CHECK(q.terms[0].or_group == q.terms[1].or_group);
  CHECK(q.terms[2].or_group == 0);

  q = P("a1 OR b2 OR c3");
  REQUIRE(q.terms.size() == 3);
  CHECK(q.terms[0].or_group == q.terms[2].or_group);

  q = P("x1 OR y1 z1 OR w1");
  REQUIRE(q.terms.size() == 4);
  CHECK(q.terms[0].or_group != q.terms[2].or_group);
  CHECK(q.terms[2].or_group == q.terms[3].or_group);

  q = P("OR leading trailing OR");  // dangling OR is ignored
  REQUIRE(q.terms.size() == 2);
  CHECK(q.terms[0].or_group == 0);
  CHECK(q.terms[1].or_group == 0);

  q = P("cats or dogs");  // lowercase "or" is a word
  REQUIRE(q.terms.size() == 3);
  CHECK(q.terms[1].value == "or");

  q = P("a1 AND b2");  // explicit AND is the default
  REQUIRE(q.terms.size() == 2);
}

TEST_CASE("parse_search: Chinese text and full-width spaces", "[search]") {
  auto q = P("周报　项目进展");  // U+3000 separates terms
  REQUIRE(q.terms.size() == 2);
  CHECK(q.terms[0].value == "周报");
  CHECK(q.terms[1].value == "项目进展");
  q = P("from:张三");
  REQUIRE(q.terms.size() == 1);
  CHECK(q.terms[0].field == "from");
  CHECK(q.terms[0].value == "张三");
}

// =============================================================================================
// Value parsers
// =============================================================================================

TEST_CASE("parse_size", "[search]") {
  CHECK(parse_size("100") == 100);
  CHECK(parse_size("10K") == 10240);
  CHECK(parse_size("10k") == 10240);
  CHECK(parse_size("5M") == 5LL * 1024 * 1024);
  CHECK(parse_size("2mb") == 2LL * 1024 * 1024);
  CHECK(parse_size("1G") == 1024LL * 1024 * 1024);
  CHECK_FALSE(parse_size(""));
  CHECK_FALSE(parse_size("M"));
  CHECK_FALSE(parse_size("-5"));
  CHECK_FALSE(parse_size("5X"));
  CHECK_FALSE(parse_size("99999999999999999999K"));
}

TEST_CASE("parse_relative_age_ms", "[search]") {
  CHECK(parse_relative_age_ms("3d") == 3 * kDay);
  CHECK(parse_relative_age_ms("2m") == 60 * kDay);
  CHECK(parse_relative_age_ms("1y") == 365 * kDay);
  CHECK(parse_relative_age_ms("0d") == 0);
  CHECK(parse_relative_age_ms("7D") == 7 * kDay);
  CHECK_FALSE(parse_relative_age_ms("d"));
  CHECK_FALSE(parse_relative_age_ms("3"));
  CHECK_FALSE(parse_relative_age_ms("3w"));
  CHECK_FALSE(parse_relative_age_ms("-3d"));
}

TEST_CASE("parse_search_date with tzoff", "[search]") {
  const int64_t utc_midnight = *utc_ms(2026, 10, 7);
  CHECK(parse_search_date("2026/10/07", 0) == utc_midnight);
  CHECK(parse_search_date("2026-10-07", 0) == utc_midnight);
  CHECK(parse_search_date("2026/10/7", 0) == utc_midnight);
  // Asia/Shanghai (UTC+8, tzoff=+480): local midnight is 16:00 UTC the previous day.
  CHECK(parse_search_date("2026/10/07", 480) == utc_midnight - 8 * 3600 * 1000);
  // UTC-5 (tzoff=-300): local midnight is 05:00 UTC.
  CHECK(parse_search_date("2026-10-07", -300) == utc_midnight + 5 * 3600 * 1000);
  CHECK_FALSE(parse_search_date("2026/13/01", 0));
  CHECK_FALSE(parse_search_date("2026/02/30", 0));
  CHECK_FALSE(parse_search_date("26/10/07", 0));
  CHECK_FALSE(parse_search_date("2026/10", 0));
  CHECK_FALSE(parse_search_date("yesterday", 0));
  CHECK_FALSE(parse_search_date("2026.10.07", 0));
}

// =============================================================================================
// Compiler (pure)
// =============================================================================================

TEST_CASE("compile_search: default scope excludes spam and trash", "[search]") {
  auto f = compile_search("", 0, kT0);
  CHECK(f.where == "m.trashed_at IS NULL AND m.is_spam = 0");
  CHECK(f.binds.empty());
  CHECK_FALSE(f.include_spam_trash);
  CHECK_FALSE(f.in_folder);

  f = compile_search("hello", 0, kT0);
  CHECK(where_has(f, "m.trashed_at IS NULL AND m.is_spam = 0 AND "));
  CHECK(where_has(f, "message_fts MATCH ?"));
  CHECK(string_binds(f) == std::vector<std::string>{"\"hello\""});
}

TEST_CASE("compile_search: long vs short terms", "[search]") {
  auto f = compile_search("周报", 0, kT0);  // 2 code points → LIKE over the FTS columns
  CHECK(where_has(f, "f.rowid = m.id"));
  CHECK(where_has(f, "LIKE ? ESCAPE"));
  CHECK_FALSE(where_has(f, "MATCH"));
  CHECK(string_binds(f).size() == 5);
  CHECK(string_binds(f)[0] == "%周报%");

  f = compile_search("项目进展", 0, kT0);  // 4 code points → trigram MATCH
  CHECK(where_has(f, "MATCH ?"));
  CHECK(string_binds(f) == std::vector<std::string>{"\"项目进展\""});

  f = compile_search("ab", 0, kT0);
  CHECK(where_has(f, "LIKE"));
  f = compile_search("abc", 0, kT0);
  CHECK(where_has(f, "MATCH"));

  f = compile_search("50%_off", 0, kT0);  // LIKE metacharacters never reach LIKE unescaped
  CHECK(string_binds(f) == std::vector<std::string>{"\"50%_off\""});
  f = compile_search("%_", 0, kT0);
  CHECK(f.binds.empty());  // punctuation only → no term
  f = compile_search("a%", 0, kT0);
  CHECK(string_binds(f)[0] == "%a\\%%");
}

TEST_CASE("compile_search: FTS quoting and column filters", "[search]") {
  auto f = compile_search("say \"he said \"\"hi\"\"\"", 0, kT0);
  // Embedded quotes are doubled inside the FTS5 string.
  CHECK(fts_quote("a\"b") == "\"a\"\"b\"");
  CHECK(fts_quote("") == "\"\"");

  f = compile_search("from:alice", 0, kT0);
  CHECK(string_binds(f) == std::vector<std::string>{"from_text : \"alice\""});
  f = compile_search("to:bob@x.com", 0, kT0);
  CHECK(string_binds(f) == std::vector<std::string>{"to_text : \"bob@x.com\""});
  f = compile_search("subject:周报汇总", 0, kT0);
  CHECK(string_binds(f) == std::vector<std::string>{"subject : \"周报汇总\""});
  f = compile_search("filename:report.pdf", 0, kT0);
  CHECK(string_binds(f) == std::vector<std::string>{"attach_names : \"report.pdf\""});
  f = compile_search("subject:周报", 0, kT0);  // short → LIKE on messages.subject
  CHECK(where_has(f, "m.subject LIKE ?"));
  f = compile_search("from:张三", 0, kT0);
  CHECK(where_has(f, "m.from_name LIKE ?"));
  f = compile_search("cc:carol", 0, kT0);
  CHECK(where_has(f, "json_each(m.cc_json)"));
  CHECK(where_has(f, "MATCH"));
  f = compile_search("bcc:dave", 0, kT0);
  CHECK(where_has(f, "json_each(m.bcc_json)"));
  CHECK_FALSE(where_has(f, "MATCH"));
  f = compile_search("filename:ab", 0, kT0);
  CHECK(where_has(f, "a.filename LIKE ?"));
}

TEST_CASE("compile_search: in:, is:, has:", "[search]") {
  auto f = compile_search("in:inbox", 0, kT0);
  CHECK(where_has(f, "m.in_inbox = 1"));
  CHECK(f.in_folder == Folder::Inbox);
  CHECK_FALSE(f.include_spam_trash);

  f = compile_search("in:spam", 0, kT0);
  CHECK(f.include_spam_trash);
  CHECK(f.in_folder == Folder::Spam);
  CHECK(f.where.find("m.trashed_at IS NULL AND m.is_spam = 0") == std::string::npos);

  f = compile_search("in:trash", 0, kT0);
  CHECK(f.include_spam_trash);
  CHECK(f.in_folder == Folder::Trash);

  f = compile_search("in:anywhere x1y", 0, kT0);
  CHECK(f.include_spam_trash);
  CHECK_FALSE(f.in_folder);

  f = compile_search("-in:spam", 0, kT0);  // negation never widens the scope
  CHECK_FALSE(f.include_spam_trash);
  CHECK(where_has(f, "NOT ("));

  CHECK(where_has(compile_search("in:sent", 0, kT0), "m.direction = 'out'"));
  CHECK(where_has(compile_search("in:drafts", 0, kT0), "m.is_draft = 1"));
  CHECK(where_has(compile_search("in:starred", 0, kT0), "m.is_starred = 1"));
  CHECK(where_has(compile_search("in:scheduled", 0, kT0), "o.scheduled_at IS NOT NULL"));
  CHECK(compile_search("in:sent in:drafts", 0, kT0).in_folder == Folder::Drafts);  // last wins

  CHECK(where_has(compile_search("is:unread", 0, kT0), "m.is_read = 0"));
  CHECK(where_has(compile_search("is:read", 0, kT0), "m.is_read = 1"));
  CHECK(where_has(compile_search("is:starred", 0, kT0), "m.is_starred = 1"));
  CHECK(where_has(compile_search("has:attachment", 0, kT0), "m.has_attachments = 1"));

  // Invalid operator values fall back to literal text.
  f = compile_search("in:nowhere", 0, kT0);
  CHECK(string_binds(f) == std::vector<std::string>{"\"in:nowhere\""});
  f = compile_search("is:important", 0, kT0);
  CHECK(string_binds(f) == std::vector<std::string>{"\"is:important\""});
  f = compile_search("after:yesterday", 0, kT0);
  CHECK(string_binds(f) == std::vector<std::string>{"\"after:yesterday\""});
}

TEST_CASE("compile_search: dates, ages and sizes", "[search]") {
  auto f = compile_search("after:2026/10/01 before:2026/10/08", 480, kT0);
  REQUIRE(int_binds(f).size() == 2);
  CHECK(int_binds(f)[0] == *utc_ms(2026, 10, 1) - 480 * 60'000);
  CHECK(int_binds(f)[1] == *utc_ms(2026, 10, 8) - 480 * 60'000);
  CHECK(where_has(f, "m.date >= ?"));
  CHECK(where_has(f, "m.date < ?"));

  f = compile_search("newer_than:2d", 0, kT0);
  CHECK(int_binds(f) == std::vector<int64_t>{kT0 - 2 * kDay});
  f = compile_search("older_than:1y", 0, kT0);
  CHECK(int_binds(f) == std::vector<int64_t>{kT0 - 365 * kDay});
  CHECK(where_has(f, "m.date < ?"));

  f = compile_search("larger:10K smaller:5M", 0, kT0);
  CHECK(int_binds(f) == std::vector<int64_t>{10240, 5LL * 1024 * 1024});
  CHECK(where_has(f, "m.size_bytes >= ?"));
  CHECK(where_has(f, "m.size_bytes < ?"));
}

TEST_CASE("compile_search: OR, negation and bind order", "[search]") {
  auto f = compile_search("alpha OR beta -gamma", 0, kT0);
  CHECK(where_has(f, " OR "));
  CHECK(where_has(f, "NOT ("));
  CHECK(string_binds(f) == std::vector<std::string>{"\"alpha\"", "\"beta\"", "\"gamma\""});
  CHECK(count_placeholders(f.where) == static_cast<int>(f.binds.size()));

  f = compile_search("from:a1x OR label:work subject:hey has:attachment larger:1M 周报", 480, kT0);
  CHECK(count_placeholders(f.where) == static_cast<int>(f.binds.size()));

  f = compile_search("label:my-label", 0, kT0);
  CHECK(where_has(f, "replace(l.name, ' ', '-')"));
  CHECK(string_binds(f) == std::vector<std::string>{"my-label", "my-label"});
}

TEST_CASE("compile_search: never throws on hostile input", "[search]") {
  for (std::string_view q : {"\"", "-", "-\"", "OR", "OR OR", ":", "::", "from:\"", "in:", "\xFF\xFE",
                             "label:\"", "))((", "\"a\" OR", "a OR -b", "subject:'x'", "NEAR(a b)"}) {
    const SqlFilter f = compile_search(q, 0, kT0);
    CHECK(count_placeholders(f.where) == static_cast<int>(f.binds.size()));
  }
}

// =============================================================================================
// Execution against a real database (FTS5 trigram)
// =============================================================================================

namespace {

struct SearchDb {
  test::TestServices ts;
  int64_t alice = 0, bob = 0;
  int64_t work_label = 0;
  // message ids
  int64_t m_weekly = 0, m_progress = 0, m_invoice = 0, m_spam = 0, m_trash = 0, m_bob_private = 0,
          m_draft = 0, m_old = 0, m_cc = 0, m_big = 0;

  SearchDb() {
    ts.db.write([&](db::Tx& tx) {
      alice = test::seed_user(tx, "alice@team.example", false, "Alice");
      bob = test::seed_user(tx, "bob@team.example", false, "Bob");
      work_label = insert_label(tx, alice, "Work Items");
      const Address carol{"Carol 王", "carol@ext.example"};
      const Address zhang{"张三", "zhangsan@ext.example"};
      const Address me{"Alice", "alice@team.example"};

      Msg m;
      m.owner = alice;
      m.from = zhang;
      m.to = {me};
      m.subject = "本周周报";
      m.text = "本周完成了项目进展汇总。";
      m.date = kT0;
      m_weekly = insert_message(tx, m).message_id;

      m = Msg{};
      m.owner = alice;
      m.from = carol;
      m.to = {me};
      m.subject = "Project progress";
      m.text = "The QUARTERLY numbers look great";
      m.date = kT0 + 1000;
      m.labels = {work_label};
      m_progress = insert_message(tx, m).message_id;

      m = Msg{};
      m.owner = alice;
      m.from = carol;
      m.to = {me};
      m.subject = "Invoice";
      m.text = "please find attached";
      m.atts = {Att{.filename = "发票2026.pdf"}};
      m.date = kT0 + 2000;
      m.size_bytes = 3 * 1024 * 1024;
      m_invoice = insert_message(tx, m).message_id;

      m = Msg{};
      m.owner = alice;
      m.from = {"Spammer", "spam@bad.example"};
      m.to = {me};
      m.subject = "quarterly prize";
      m.text = "win quarterly";
      m.is_spam = true;
      m.in_inbox = false;
      m.date = kT0 + 3000;
      m_spam = insert_message(tx, m).message_id;

      m = Msg{};
      m.owner = alice;
      m.from = carol;
      m.to = {me};
      m.subject = "old quarterly memo";
      m.text = "trashed";
      m.trashed_at = kT0;
      m.date = kT0 + 4000;
      m_trash = insert_message(tx, m).message_id;

      m = Msg{};
      m.owner = bob;  // another owner's mail must never match alice's search (IDOR)
      m.from = carol;
      m.to = {{"Bob", "bob@team.example"}};
      m.subject = "quarterly for bob";
      m.text = "bob only 周报";
      m.date = kT0 + 5000;
      m_bob_private = insert_message(tx, m).message_id;

      m = Msg{};
      m.owner = alice;
      m.direction = "out";
      m.is_draft = true;
      m.in_inbox = false;
      m.is_read = true;
      m.from = me;
      m.to = {carol};
      m.bcc = {{"Dave", "dave@ext.example"}};
      m.subject = "draft about lunch";
      m.text = "lunch plans";
      m.date = kT0 + 6000;
      m_draft = insert_message(tx, m).message_id;

      m = Msg{};
      m.owner = alice;
      m.from = zhang;
      m.to = {me};
      m.subject = "ancient";
      m.text = "very old message";
      m.date = kT0 - 400 * kDay;
      m.is_read = true;
      m_old = insert_message(tx, m).message_id;

      m = Msg{};
      m.owner = alice;
      m.from = zhang;
      m.to = {{"Erin", "erin@ext.example"}};
      m.cc = {me, {"Frank", "frank@ext.example"}};
      m.subject = "cc test";
      m.text = "carbon copy";
      m.date = kT0 + 7000;
      m.is_starred = true;
      m_cc = insert_message(tx, m).message_id;

      m = Msg{};
      m.owner = alice;
      m.from = zhang;
      m.to = {me};
      m.subject = "big one";
      m.text = "huge";
      m.date = kT0 + 8000;
      m.size_bytes = 20 * 1024 * 1024;
      m.is_read = true;
      m_big = insert_message(tx, m).message_id;
    });
  }

  std::set<int64_t> run(std::string_view q, int tzoff = 0, int64_t owner = 0) {
    if (owner == 0) owner = alice;
    const SqlFilter f = compile_search(q, tzoff, kT0 + kDay);
    return ts.db.read([&](db::Conn& c) {
      auto s = c.prepare("SELECT m.id FROM messages m WHERE m.owner_id = ? AND (" + f.where + ")");
      int idx = 1;
      s.bind(idx++, owner);
      for (const auto& b : f.binds) s.bind(idx++, b);
      std::set<int64_t> out;
      while (s.step()) out.insert(s.i64(0));
      return out;
    });
  }
};

}  // namespace

TEST_CASE("search execution: text terms incl. Chinese", "[search][db]") {
  SearchDb d;
  CHECK(d.run("项目进展") == std::set<int64_t>{d.m_weekly});           // 4-char Chinese, MATCH
  CHECK(d.run("周报") == std::set<int64_t>{d.m_weekly});               // 2-char Chinese, LIKE
  CHECK(d.run("quarterly") == std::set<int64_t>{d.m_progress});        // spam/trash excluded
  CHECK(d.run("QUARTERLY") == std::set<int64_t>{d.m_progress});        // case-insensitive
  CHECK(d.run("quarterly in:anywhere") == std::set<int64_t>{d.m_progress, d.m_spam, d.m_trash});
  CHECK(d.run("\"numbers look\"") == std::set<int64_t>{d.m_progress});
  CHECK(d.run("\"look numbers\"").empty());
  CHECK(d.run("lunch") == std::set<int64_t>{d.m_draft});               // drafts are searchable
  CHECK(d.run("nonexistentterm").empty());
  CHECK(d.run("quarterly", 0, d.bob) == std::set<int64_t>{d.m_bob_private});
  CHECK(d.run("周报", 0, d.bob) == std::set<int64_t>{d.m_bob_private});
}

TEST_CASE("search execution: operators", "[search][db]") {
  SearchDb d;
  CHECK(d.run("from:carol") == std::set<int64_t>{d.m_progress, d.m_invoice});
  CHECK(d.run("from:张三 ancient") == std::set<int64_t>{d.m_old});
  CHECK(d.run("from:zhangsan@ext.example in:inbox").size() == 4);
  CHECK(d.run("to:erin") == std::set<int64_t>{d.m_cc});
  CHECK(d.run("cc:frank") == std::set<int64_t>{d.m_cc});
  CHECK(d.run("cc:erin").empty());  // erin is in To, not Cc
  CHECK(d.run("bcc:dave") == std::set<int64_t>{d.m_draft});
  CHECK(d.run("subject:invoice") == std::set<int64_t>{d.m_invoice});
  CHECK(d.run("subject:周报") == std::set<int64_t>{d.m_weekly});
  CHECK(d.run("filename:发票2026") == std::set<int64_t>{d.m_invoice});
  CHECK(d.run("filename:发票") == std::set<int64_t>{d.m_invoice});
  CHECK(d.run("has:attachment") == std::set<int64_t>{d.m_invoice});
  CHECK(d.run("label:\"work items\"") == std::set<int64_t>{d.m_progress});
  CHECK(d.run("label:work-items") == std::set<int64_t>{d.m_progress});
  CHECK(d.run("label:nolabel").empty());
  CHECK(d.run("is:starred") == std::set<int64_t>{d.m_cc});
  CHECK(d.run("in:drafts") == std::set<int64_t>{d.m_draft});
  CHECK(d.run("in:spam") == std::set<int64_t>{d.m_spam});
  CHECK(d.run("in:trash") == std::set<int64_t>{d.m_trash});
  CHECK(d.run("larger:10M") == std::set<int64_t>{d.m_big});
  CHECK(d.run("larger:1M smaller:10M") == std::set<int64_t>{d.m_invoice});
  CHECK(d.run("older_than:1y") == std::set<int64_t>{d.m_old});
  CHECK(d.run("is:read -in:drafts") == std::set<int64_t>{d.m_old, d.m_big});
}

TEST_CASE("search execution: negation and OR", "[search][db]") {
  SearchDb d;
  CHECK(d.run("from:carol -invoice") == std::set<int64_t>{d.m_progress});
  CHECK(d.run("invoice OR ancient") == std::set<int64_t>{d.m_invoice, d.m_old});
  CHECK(d.run("from:carol OR from:张三 -has:attachment").count(d.m_invoice) == 0);
  CHECK(d.run("-周报").count(d.m_weekly) == 0);
  CHECK(d.run("-周报").count(d.m_progress) == 1);
}

TEST_CASE("search execution: after/before honour tzoff", "[search][db]") {
  SearchDb d;
  // A message at 2026-10-07 20:00 UTC is on 10-08 in Shanghai (UTC+8) but 10-07 in UTC.
  const int64_t at = *utc_ms(2026, 10, 7, 20, 0);
  int64_t id = 0;
  d.ts.db.write([&](db::Tx& tx) {
    Msg m;
    m.owner = d.alice;
    m.subject = "tz probe";
    m.text = "tzprobe";
    m.date = at;
    id = insert_message(tx, m).message_id;
  });
  CHECK(d.run("tzprobe after:2026/10/08", 480) == std::set<int64_t>{id});
  CHECK(d.run("tzprobe after:2026/10/08", 0).empty());
  CHECK(d.run("tzprobe before:2026/10/08", 0) == std::set<int64_t>{id});
  CHECK(d.run("tzprobe before:2026/10/08", 480).empty());
  CHECK(d.run("tzprobe after:2026-10-07 before:2026-10-08", 0) == std::set<int64_t>{id});
}

TEST_CASE("search execution: list_threads search paging, tzoff and unread", "[search][db]") {
  SearchDb d;
  std::vector<int64_t> threads;
  d.ts.db.write([&](db::Tx& tx) {
    for (int i = 0; i < 12; ++i) {
      Msg m;
      m.owner = d.alice;
      m.subject = "page " + std::to_string(i);
      m.text = "pagingterm";
      m.date = kT0 + (i / 3) * 1000;  // ties broken by thread id
      m.is_read = i != 4;
      threads.push_back(insert_message(tx, m).thread_id);
    }
  });
  std::vector<int64_t> seen;
  std::optional<std::string> cursor;
  int pages = 0;
  bool saw_unread = false;
  do {
    ThreadQuery q;
    q.q = "pagingterm";
    q.limit = 5;
    q.cursor = cursor;
    q.now_ms = kT0 + kDay;
    const auto p = d.ts.db.read([&](db::Conn& c) { return list_threads(c, d.alice, q); });
    CHECK_FALSE(p.total);
    for (const auto& it : p.items) {
      seen.push_back(it.id);
      if (it.unread) {
        saw_unread = true;
        CHECK(it.id == threads[4]);
      }
    }
    cursor = p.next_cursor;
    ++pages;
    REQUIRE(pages < 10);
  } while (cursor);
  CHECK(pages == 3);
  CHECK(saw_unread);
  REQUIRE(seen.size() == 12);
  std::vector<int64_t> expected = threads;
  std::sort(expected.begin(), expected.end(), [&](int64_t a, int64_t b) {
    const auto ia = std::find(threads.begin(), threads.end(), a) - threads.begin();
    const auto ib = std::find(threads.begin(), threads.end(), b) - threads.begin();
    const int64_t da = kT0 + (ia / 3) * 1000, db = kT0 + (ib / 3) * 1000;
    return da != db ? da > db : a > b;
  });
  CHECK(seen == expected);

  // tzoff reaches the date operators.
  const int64_t at = *utc_ms(2026, 10, 7, 20, 0);
  d.ts.db.write([&](db::Tx& tx) {
    Msg m;
    m.owner = d.alice;
    m.subject = "tz";
    m.text = "tzlistprobe";
    m.date = at;
    insert_message(tx, m);
  });
  auto search = [&](int tzoff) {
    ThreadQuery q;
    q.q = "tzlistprobe after:2026/10/08";
    q.tzoff_min = tzoff;
    return d.ts.db.read([&](db::Conn& c) { return list_threads(c, d.alice, q); }).items.size();
  };
  CHECK(search(480) == 1);
  CHECK(search(0) == 0);

  // in:spam lists spam threads with spam-aware unread and spam_last_at.
  ThreadQuery q;
  q.q = "in:spam";
  const auto sp = d.ts.db.read([&](db::Conn& c) { return list_threads(c, d.alice, q); });
  REQUIRE(sp.items.size() == 1);
  CHECK(sp.items[0].unread);
  CHECK(sp.items[0].last_at == kT0 + 3000);
}

TEST_CASE("search execution: list_threads search view", "[search][db]") {
  SearchDb d;
  const auto page = d.ts.db.read([&](db::Conn& c) {
    ThreadQuery q;
    q.q = "quarterly in:anywhere";
    q.now_ms = kT0 + kDay;
    return list_threads(c, d.alice, q);
  });
  CHECK_FALSE(page.total.has_value());  // total is null for search
  REQUIRE(page.items.size() == 3);
  // Newest matching message first.
  CHECK(page.items[0].last_at == kT0 + 4000);
  CHECK(page.items[2].last_at == kT0 + 1000);
}
