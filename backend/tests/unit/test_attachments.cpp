// Owner: WP-B — attachment rows, blob registration, ownership lookups and GC queries.
#include "mail/attachments.hpp"
#include "mail_fixtures.hpp"
#include "test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace azm;
using namespace azm::mail;
using namespace azm::test::mailfx;

namespace {

struct Fx {
  test::TestServices ts;
  int64_t alice = 0, bob = 0;
  Fx() {
    ts.db.write([&](db::Tx& tx) {
      alice = test::seed_user(tx, "alice@team.example");
      bob = test::seed_user(tx, "bob@team.example");
    });
  }
  BlobRef blob(std::string_view content, std::string storage = "local") {
    return BlobRef{crypto::sha256_hex(content), static_cast<int64_t>(content.size()), std::move(storage)};
  }
};

}  // namespace

TEST_CASE("register_blob is idempotent", "[attachments]") {
  Fx f;
  const BlobRef b = f.blob("x");
  f.ts.db.write([&](db::Tx& tx) {
    register_blob(tx, b, 10);
    register_blob(tx, BlobRef{b.sha256, 999, "r2"}, 20);  // first registration wins
    register_blob(tx, BlobRef{crypto::sha256_hex("y"), 1, ""}, 30);  // empty storage → local
  });
  f.ts.db.read([&](db::Conn& c) {
    CHECK(count(c, "SELECT COUNT(*) FROM blobs") == 2);
    CHECK(c.scalar<int64_t>("SELECT size FROM blobs WHERE sha256=?", b.sha256) == 1);
    CHECK(c.scalar<std::string>("SELECT storage FROM blobs WHERE sha256=?", b.sha256) == "local");
    CHECK(c.scalar<int64_t>("SELECT created_at FROM blobs WHERE sha256=?", b.sha256) == 10);
    CHECK(c.scalar<std::string>("SELECT storage FROM blobs WHERE sha256=?", crypto::sha256_hex("y")) == "local");
  });
}

TEST_CASE("create_upload sanitizes and returns a signed view", "[attachments]") {
  Fx f;
  const int64_t now = f.ts.clock.now_ms();
  const auto v = f.ts.db.write([&](db::Tx& tx) {
    return create_upload(tx, f.ts.urls, f.alice, f.blob("png-bytes"), "C:\\fakepath\\截图 1.png",
                         "Image/PNG; name=x", true, now);
  });
  CHECK(v.filename == "截图 1.png");
  CHECK(v.content_type == "image/png");
  CHECK(v.size == 9);
  CHECK(v.is_inline);
  REQUIRE(v.content_id);
  CHECK(v.content_id->ends_with("@azmail"));
  CHECK(v.content_id->size() == 24 + 7);
  CHECK(v.download_url == f.ts.urls.file_url(v.id, f.alice, 'a', f.ts.urls.expiry(now)));
  CHECK(v.view_url == f.ts.urls.file_url(v.id, f.alice, 'i', f.ts.urls.expiry(now)));

  const auto plain = f.ts.db.write([&](db::Tx& tx) {
    return create_upload(tx, f.ts.urls, f.alice, f.blob("zip"), std::string("a\x01/b\x7f\n"), "", false, now);
  });
  CHECK(plain.filename == "b");
  CHECK(plain.content_type == "application/octet-stream");
  CHECK_FALSE(plain.is_inline);
  CHECK_FALSE(plain.content_id);
  CHECK_FALSE(plain.view_url);

  const auto noname = f.ts.db.write([&](db::Tx& tx) {
    return create_upload(tx, f.ts.urls, f.alice, f.blob("q"), "  /// ", "text/html", false, now);
  });
  CHECK(noname.filename == "attachment");
  CHECK(noname.content_type == "text/html");

  std::string longname(400, 'a');
  longname += ".txt";
  const auto lng = f.ts.db.write([&](db::Tx& tx) {
    return create_upload(tx, f.ts.urls, f.alice, f.blob("l"), longname, "text/plain", false, now);
  });
  CHECK(lng.filename.size() == 255);

  f.ts.db.read([&](db::Conn& c) {
    const auto rec = find_attachment(c, f.alice, v.id);
    REQUIRE(rec);
    CHECK_FALSE(rec->message_id);
    CHECK(rec->storage == "local");
    CHECK(rec->blob_sha256 == crypto::sha256_hex("png-bytes"));
    CHECK(rec->created_at == now);
    CHECK_FALSE(find_attachment(c, f.bob, v.id));  // IDOR
    CHECK_FALSE(find_attachment(c, f.alice, 999999));
  });
}

TEST_CASE("message_attachments and find_raw", "[attachments]") {
  Fx f;
  std::string raw_sha = crypto::sha256_hex("raw");
  Inserted in, out;
  f.ts.db.write([&](db::Tx& tx) {
    register_blob(tx, BlobRef{raw_sha, 3, "r2"}, 1);
    const int64_t ib = insert_inbound_email(tx, "re_1", raw_sha);
    Msg m;
    m.owner = f.alice;
    m.subject = "周报/第 3 周:\"总结\"";
    m.inbound_id = ib;
    m.atts = {Att{.filename = "b.pdf"}, Att{.filename = "a.png", .content_type = "image/png"}};
    in = insert_message(tx, m);
    Msg o;
    o.owner = f.alice;
    o.direction = "out";
    o.subject = "out";
    out = insert_message(tx, o);
  });
  f.ts.db.read([&](db::Conn& c) {
    const auto atts = message_attachments(c, f.alice, in.message_id);
    REQUIRE(atts.size() == 2);
    CHECK(atts[0].filename == "b.pdf");
    CHECK(atts[1].filename == "a.png");
    CHECK(atts[0].message_id == in.message_id);
    CHECK(message_attachments(c, f.bob, in.message_id).empty());

    const auto ta = thread_attachments(c, f.alice, in.thread_id);
    REQUIRE(ta.size() == 2);
    CHECK(ta[0].id == atts[0].id);
    CHECK(ta[1].storage == "local");
    CHECK(thread_attachments(c, f.bob, in.thread_id).empty());  // IDOR
    CHECK(thread_attachments(c, f.alice, out.thread_id).empty());

    const auto raw = find_raw(c, f.alice, in.message_id);
    REQUIRE(raw);
    CHECK(raw->sha256 == raw_sha);
    CHECK(raw->storage == "r2");
    CHECK(raw->size == 3);
    CHECK(raw->filename == "第 3 周:\"总结\".eml");  // path separators split, quoting left to RFC 6266
    CHECK_FALSE(find_raw(c, f.bob, in.message_id));
    CHECK_FALSE(find_raw(c, f.alice, out.message_id));  // outbound: no raw
  });
  // Inbound without a raw blob, and an empty subject.
  f.ts.db.write([&](db::Tx& tx) {
    const int64_t ib = insert_inbound_email(tx, "re_2", std::nullopt);
    Msg m;
    m.owner = f.alice;
    m.subject = "";
    m.inbound_id = ib;
    in = insert_message(tx, m);
    const int64_t ib3 = insert_inbound_email(tx, "re_3", raw_sha);
    m.inbound_id = ib3;
    out = insert_message(tx, m);
  });
  f.ts.db.read([&](db::Conn& c) {
    CHECK_FALSE(find_raw(c, f.alice, in.message_id));
    const auto raw = find_raw(c, f.alice, out.message_id);
    REQUIRE(raw);
    CHECK(raw->filename == "message.eml");
  });
}

TEST_CASE("orphan uploads and blob GC queries", "[attachments][gc]") {
  Fx f;
  const BlobRef shared = f.blob("shared"), lonely = f.blob("lonely"), raw = f.blob("raw"), fresh = f.blob("fresh");
  int64_t old_upload = 0, new_upload = 0;
  f.ts.db.write([&](db::Tx& tx) {
    old_upload = create_upload(tx, f.ts.urls, f.alice, shared, "a", "text/plain", false, 100).id;
    new_upload = create_upload(tx, f.ts.urls, f.alice, shared, "b", "text/plain", false, 5000).id;
    register_blob(tx, lonely, 100);
    register_blob(tx, raw, 100);
    insert_inbound_email(tx, "re_x", raw.sha256);
    // Still being processed: a delivered row whose copies are all gone no longer pins its raw
    // (review R3, covered in test_review_be_mail.cpp).
    tx.run("UPDATE inbound_emails SET state = 'pending' WHERE resend_id = 're_x'");
    register_blob(tx, fresh, 9000);
  });

  // Only the old unattached upload goes.
  CHECK(f.ts.db.write([&](db::Tx& tx) { return purge_orphan_uploads(tx, 1000, 10); }) == 1);
  f.ts.db.read([&](db::Conn& c) {
    CHECK_FALSE(find_attachment(c, f.alice, old_upload));
    CHECK(find_attachment(c, f.alice, new_upload));
  });
  CHECK(f.ts.db.write([&](db::Tx& tx) { return purge_orphan_uploads(tx, 1000, 10); }) == 0);
  CHECK(f.ts.db.write([&](db::Tx& tx) { return purge_orphan_uploads(tx, 99999, 0); }) == 0);

  f.ts.db.read([&](db::Conn& c) {
    // shared: still referenced by new_upload; raw: referenced by inbound_emails; fresh: too new.
    const auto cands = unreferenced_blobs(c, 8000, 10);
    REQUIRE(cands.size() == 1);
    CHECK(cands[0].sha256 == lonely.sha256);
    CHECK(cands[0].storage == "local");
    CHECK(cands[0].size == lonely.size);
    CHECK(unreferenced_blobs(c, 99999, 10).size() == 2);
    CHECK(unreferenced_blobs(c, 99999, 1).size() == 1);
    CHECK(unreferenced_blobs(c, 99999, 0).empty());
    CHECK(is_blob_unreferenced(c, lonely.sha256));
    CHECK(is_blob_unreferenced(c, fresh.sha256));
    CHECK_FALSE(is_blob_unreferenced(c, shared.sha256));
    CHECK_FALSE(is_blob_unreferenced(c, raw.sha256));
    CHECK_FALSE(is_blob_unreferenced(c, crypto::sha256_hex("never registered")));
  });

  CHECK(f.ts.db.write([&](db::Tx& tx) { return forget_blob_if_unreferenced(tx, lonely.sha256); }));
  CHECK_FALSE(f.ts.db.write([&](db::Tx& tx) { return forget_blob_if_unreferenced(tx, lonely.sha256); }));
  CHECK_FALSE(f.ts.db.write([&](db::Tx& tx) { return forget_blob_if_unreferenced(tx, shared.sha256); }));
  CHECK_FALSE(f.ts.db.write([&](db::Tx& tx) { return forget_blob_if_unreferenced(tx, raw.sha256); }));
  f.ts.db.read([&](db::Conn& c) {
    CHECK(count(c, "SELECT COUNT(*) FROM blobs") == 3);
    CHECK(c.scalar<int64_t>("SELECT 1 FROM blobs WHERE sha256=?", shared.sha256));
  });

  // Once the last reference goes, the shared blob becomes collectable.
  f.ts.db.write([&](db::Tx& tx) { tx.run("DELETE FROM attachments WHERE id=?", new_upload); });
  f.ts.db.read([&](db::Conn& c) { CHECK(is_blob_unreferenced(c, shared.sha256)); });
}
