// Owner: WP-B — fts_reindex field rules (DESIGN §2, C2) and fts_rebuild_all.
#include "mail/fts.hpp"
#include "mail_fixtures.hpp"
#include "test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace azm;
using namespace azm::mail;
using namespace azm::test::mailfx;

namespace {

struct Row {
  std::string subject, from_text, to_text, body, attach_names;
};

std::optional<Row> fts_row(db::Conn& c, int64_t id) {
  auto s = c.prepare("SELECT subject, from_text, to_text, body, attach_names FROM message_fts WHERE rowid = ?");
  s.bind_all(id);
  if (!s.step()) return std::nullopt;
  return Row{s.text(0), s.text(1), s.text(2), s.text(3), s.text(4)};
}

struct Fx {
  test::TestServices ts;
  int64_t alice = 0;
  Fx() {
    ts.db.write([&](db::Tx& tx) { alice = test::seed_user(tx, "alice@team.example", false, "Alice"); });
  }
  Inserted add(Msg m) {
    m.owner = alice;
    return ts.db.write([&](db::Tx& tx) { return insert_message(tx, m); });
  }
};

const Address kTo{"To Person", "to@x.example"};
const Address kCc{"", "cc@x.example"};
const Address kBcc{"Hidden", "bcc@x.example"};

}  // namespace

TEST_CASE("fts_reindex: field contents and the BCC rule", "[fts]") {
  Fx f;
  Msg in;
  in.from = {"张三", "zhang@ext.example"};
  in.to = {kTo};
  in.cc = {kCc};
  in.bcc = {kBcc};  // an inbound copy's bcc ([self]) is display-only
  in.subject = "周报 Weekly";
  in.text = "text body";
  in.html = "<p>html body</p>";
  in.atts = {Att{.filename = "a.pdf"}, Att{.filename = "图片.png", .content_type = "image/png"}};
  const auto ri = f.add(in);

  Msg out = in;
  out.direction = "out";  // the sender's own copy indexes BCC
  out.text.reset();       // body falls back to html_to_text(html)
  const auto ro = f.add(out);

  Msg shared = out;
  shared.is_shared_copy = true;  // alias shared-sent copies never index BCC
  const auto rs = f.add(shared);

  f.ts.db.read([&](db::Conn& c) {
    const auto a = fts_row(c, ri.message_id).value();
    CHECK(a.subject == "周报 Weekly");
    CHECK(a.from_text == "张三 zhang@ext.example");
    CHECK(a.to_text == "To Person to@x.example cc@x.example");
    CHECK(a.body == "text body");
    CHECK(a.attach_names == "a.pdf 图片.png");

    const auto b = fts_row(c, ro.message_id).value();
    CHECK(b.to_text == "To Person to@x.example cc@x.example Hidden bcc@x.example");
    CHECK(b.body == "html body");

    CHECK(fts_row(c, rs.message_id).value().to_text == "To Person to@x.example cc@x.example");
  });
}

TEST_CASE("fts_reindex: replace, missing message, trigger delete, body limit", "[fts]") {
  Fx f;
  Msg m;
  m.subject = "first";
  m.text = "alpha";
  const auto r = f.add(m);
  f.ts.db.write([&](db::Tx& tx) {
    tx.run("UPDATE messages SET subject='second' WHERE id=?", r.message_id);
    tx.run("UPDATE message_bodies SET text='beta' WHERE message_id=?", r.message_id);
    fts_reindex(tx, r.message_id);
    fts_reindex(tx, r.message_id);  // idempotent: still exactly one row
  });
  f.ts.db.read([&](db::Conn& c) {
    CHECK(count(c, "SELECT COUNT(*) FROM message_fts") == 1);
    const auto row = fts_row(c, r.message_id).value();
    CHECK(row.subject == "second");
    CHECK(row.body == "beta");
    CHECK(c.scalar<int64_t>("SELECT rowid FROM message_fts WHERE message_fts MATCH '\"beta\"'") == r.message_id);
    CHECK_FALSE(c.scalar<int64_t>("SELECT rowid FROM message_fts WHERE message_fts MATCH '\"alpha\"'"));
  });
  // A message without a body row indexes an empty body.
  Msg nobody;
  nobody.subject = "no body";
  const auto rn = f.add(nobody);
  f.ts.db.read([&](db::Conn& c) { CHECK(fts_row(c, rn.message_id).value().body.empty()); });

  // Missing message: only removes the FTS row.
  f.ts.db.write([&](db::Tx& tx) { fts_reindex(tx, 987654); });
  // Deleting the message removes its FTS row (trigger).
  f.ts.db.write([&](db::Tx& tx) { tx.run("DELETE FROM messages WHERE id=?", r.message_id); });
  f.ts.db.read([&](db::Conn& c) { CHECK_FALSE(fts_row(c, r.message_id)); });

  // Bodies are truncated to kFtsBodyLimit bytes without splitting UTF-8.
  std::string big;
  while (big.size() < kFtsBodyLimit + 100) big += "中文";
  Msg large;
  large.subject = "large";
  large.text = big;
  const auto rl = f.add(large);
  f.ts.db.read([&](db::Conn& c) {
    const auto body = fts_row(c, rl.message_id).value().body;
    CHECK(body.size() <= kFtsBodyLimit);
    CHECK(body.size() > kFtsBodyLimit - 4);
    CHECK(utf8_valid(body));
  });
}

TEST_CASE("fts_rebuild_all", "[fts]") {
  Fx f;
  for (int i = 0; i < 7; ++i) {
    Msg m;
    m.subject = "msg " + std::to_string(i);
    m.text = "rebuildterm" + std::to_string(i);
    f.add(m);
  }
  // Corrupt the index: drop rows and add a stale one.
  f.ts.db.write([&](db::Tx& tx) {
    tx.run("DELETE FROM message_fts");
    tx.run("INSERT INTO message_fts(rowid, subject, from_text, to_text, body, attach_names) "
           "VALUES(424242, 'stale', '', '', '', '')");
  });
  std::vector<std::pair<int64_t, int64_t>> progress;
  const int64_t n = fts_rebuild_all(f.ts.db, 3, [&](int64_t done, int64_t total) { progress.emplace_back(done, total); });
  CHECK(n == 7);
  REQUIRE(progress.size() == 3);
  CHECK(progress[0] == std::pair<int64_t, int64_t>{3, 7});
  CHECK(progress[2] == std::pair<int64_t, int64_t>{7, 7});
  f.ts.db.read([&](db::Conn& c) {
    CHECK(count(c, "SELECT COUNT(*) FROM message_fts") == 7);
    CHECK_FALSE(fts_row(c, 424242));
    CHECK(count(c, "SELECT COUNT(*) FROM message_fts WHERE message_fts MATCH '\"rebuildterm5\"'") == 1);
  });
  // Empty database and default batch size.
  test::TestServices empty;
  CHECK(fts_rebuild_all(empty.db) == 0);
  CHECK(fts_rebuild_all(f.ts.db, 0) == 7);  // batch ≤ 0 → default
}
