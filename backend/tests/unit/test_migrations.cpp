#include "db/migrations.hpp"
#include "db/sqlite.hpp"
#include "test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <sqlite3.h>

using namespace azm;
using namespace azm::db;

namespace {

std::vector<int64_t> fts_match(Conn& c, std::string_view query) {
  std::vector<int64_t> ids;
  Stmt s = c.prepare("SELECT rowid FROM message_fts WHERE message_fts MATCH ? ORDER BY rowid");
  s.bind_all(query);
  while (s.step()) ids.push_back(s.i64(0));
  return ids;
}

// Minimal user + thread + message so FK constraints and the FTS delete trigger can be exercised.
int64_t insert_message(Conn& c, const std::string& subject) {
  c.run("INSERT OR IGNORE INTO users(id, email, password_hash, password_changed_at, created_at, updated_at) "
        "VALUES(1, 'alice@team.com', 'x', 0, 0, 0)");
  c.run("INSERT INTO threads(owner_id, subject, created_at, updated_at) VALUES(1, ?, 0, 0)", subject);
  const int64_t thread_id = c.last_insert_id();
  c.run("INSERT INTO messages(owner_id, thread_id, direction, subject, date, created_at, updated_at) "
        "VALUES(1, ?, 'in', ?, 0, 0, 0)",
        thread_id, subject);
  return c.last_insert_id();
}

}  // namespace

TEST_CASE("capability check passes on the linked SQLite", "[migrations]") {
  test::TempDir td;
  Conn c(td / "cap.db");
  CHECK(sqlite3_libversion_number() >= 3034000);
  CHECK_NOTHROW(check_capabilities(c));
  // The probe table is gone afterwards.
  CHECK(c.scalar<int64_t>("SELECT COUNT(*) FROM temp.sqlite_master WHERE name='azm_fts_probe'").value() == 0);
}

TEST_CASE("migrate is idempotent", "[migrations]") {
  test::TempDir td;
  Conn c(td / "m.db");
  CHECK(current_version(c) == 0);
  CHECK(latest_version() == 1);
  REQUIRE(migrations().size() >= 1);
  CHECK(migrations()[0].version == 1);

  CHECK(migrate(c) == 1);
  CHECK(current_version(c) == 1);
  CHECK(migrate(c) == 1);
  CHECK(migrate(c) == 1);
  CHECK(c.scalar<int64_t>("SELECT COUNT(*) FROM schema_migrations").value() == 1);
  CHECK(c.scalar<int64_t>("SELECT applied_at FROM schema_migrations WHERE version=1").value() > 0);
  CHECK_FALSE(c.in_transaction());

  // A second connection (fresh process) sees the same state and migrates as a no-op.
  Conn c2(td / "m.db");
  CHECK(current_version(c2) == 1);
  CHECK(migrate(c2) == 1);
}

TEST_CASE("migration refuses a newer database", "[migrations]") {
  test::TempDir td;
  Conn c(td / "new.db");
  migrate(c);
  c.run("INSERT INTO schema_migrations(version, applied_at) VALUES(99, 0)");
  CHECK_THROWS_AS(migrate(c), Error);
  CHECK_FALSE(c.in_transaction());
}

TEST_CASE("all schema tables, indexes and triggers exist", "[migrations]") {
  test::TempDir td;
  Conn c(td / "t.db");
  migrate(c);
  const char* tables[] = {"schema_migrations", "kv", "domains", "users", "addresses", "alias_members",
                          "sessions", "user_settings", "trusted_image_senders", "labels", "threads",
                          "blobs", "inbound_emails", "outbound", "messages", "message_bodies",
                          "message_refs", "message_labels", "attachments", "webhook_events",
                          "delivery_events", "jobs", "contacts", "audit_log", "message_fts"};
  for (const char* t : tables) {
    INFO("table " << t);
    CHECK(c.scalar<int64_t>("SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name=?", t).value() == 1);
  }
  const char* indexes[] = {"threads_inbox", "threads_trash", "messages_in_msgid", "jobs_dedupe",
                           "jobs_ready", "message_refs_lookup", "attachments_unattached",
                           "outbound_msgid", "sessions_expires", "webhook_events_email"};
  for (const char* i : indexes) {
    INFO("index " << i);
    CHECK(c.scalar<int64_t>("SELECT COUNT(*) FROM sqlite_master WHERE type='index' AND name=?", i).value() == 1);
  }
  CHECK(c.scalar<int64_t>("SELECT COUNT(*) FROM sqlite_master WHERE type='trigger' AND name='messages_fts_ad'")
            .value() == 1);
}

TEST_CASE("blobs.storage column (Addendum A)", "[migrations]") {
  test::TempDir td;
  Conn c(td / "b.db");
  migrate(c);
  c.run("INSERT INTO blobs(sha256, size, created_at) VALUES('aa', 1, 0)");
  CHECK(c.scalar<std::string>("SELECT storage FROM blobs WHERE sha256='aa'").value() == "local");
  c.run("INSERT INTO blobs(sha256, size, storage, created_at) VALUES('bb', 1, 'r2', 0)");
  CHECK_THROWS_AS(c.run("INSERT INTO blobs(sha256, size, storage, created_at) VALUES('cc', 1, 's3', 0)"), Error);
  c.run("INSERT OR IGNORE INTO blobs(sha256, size, created_at) VALUES('aa', 1, 0)");  // dedupe
  CHECK(c.scalar<int64_t>("SELECT COUNT(*) FROM blobs").value() == 2);
}

TEST_CASE("schema constraints: foreign keys, CHECKs and partial unique indexes", "[migrations]") {
  test::TempDir td;
  Conn c(td / "k.db");
  migrate(c);
  // FK: message for a missing owner/thread.
  try {
    c.run("INSERT INTO messages(owner_id, thread_id, direction, date, created_at, updated_at) "
          "VALUES(42, 42, 'in', 0, 0, 0)");
    FAIL("expected FK violation");
  } catch (const Error& e) {
    CHECK(e.is_foreign_key_violation());
  }
  // CHECK on undo_send_seconds.
  c.run("INSERT INTO users(id, email, password_hash, password_changed_at, created_at, updated_at) "
        "VALUES(1, 'a@team.com', 'x', 0, 0, 0)");
  CHECK_THROWS_AS(c.run("INSERT INTO user_settings(user_id, undo_send_seconds, updated_at) VALUES(1, 7, 0)"), Error);
  c.run("INSERT INTO user_settings(user_id, updated_at) VALUES(1, 0)");
  CHECK(c.scalar<std::string>("SELECT timezone FROM user_settings WHERE user_id=1").value() == "Asia/Shanghai");
  // users.email is case-insensitive unique.
  try {
    c.run("INSERT INTO users(email, password_hash, password_changed_at, created_at, updated_at) "
          "VALUES('A@TEAM.COM', 'x', 0, 0, 0)");
    FAIL("expected unique violation");
  } catch (const Error& e) {
    CHECK(e.is_unique_violation());
  }
  // jobs dedupe: unique only among pending/running.
  const char* ins = "INSERT INTO jobs(kind, lane, state, run_at, dedupe_key, created_at, updated_at) "
                    "VALUES('inbound.fetch', 'inbound', ?, 0, 'in:re_1', 0, 0)";
  c.run(ins, "done");
  c.run(ins, "pending");
  try {
    c.run(ins, "running");
    FAIL("expected dedupe violation");
  } catch (const Error& e) {
    CHECK(e.is_unique_violation());
  }
  c.run(ins, "canceled");
  CHECK(c.scalar<int64_t>("SELECT COUNT(*) FROM jobs").value() == 3);
}

TEST_CASE("FTS5 trigram search: Chinese and case-insensitive English", "[migrations][fts]") {
  test::TempDir td;
  Conn c(td / "f.db");
  migrate(c);
  const int64_t m1 = insert_message(c, "周报会 安排");
  const int64_t m2 = insert_message(c, "Quarterly Report");
  const int64_t m3 = insert_message(c, "周报");
  const char* ins = "INSERT INTO message_fts(rowid, subject, from_text, to_text, body, attach_names) "
                    "VALUES(?, ?, ?, ?, ?, ?)";
  c.run(ins, m1, "周报会 安排", "张三 zs@team.com", "", "下周一的周报会改到 10 点", "议程.docx");
  c.run(ins, m2, "Quarterly Report", "Alice alice@team.com", "bob@team.com", "Numbers for Q3 are in.", "");
  c.run(ins, m3, "周报", "李四 ls@team.com", "", "本周进展", "");

  CHECK(fts_match(c, "\"周报会\"") == std::vector<int64_t>{m1});
  CHECK(fts_match(c, "\"report\"") == std::vector<int64_t>{m2});
  CHECK(fts_match(c, "\"REPORT\"") == std::vector<int64_t>{m2});
  CHECK(fts_match(c, "\"rtERly\"") == std::vector<int64_t>{m2});  // substring match
  CHECK(fts_match(c, "subject : \"周报会\"") == std::vector<int64_t>{m1});
  CHECK(fts_match(c, "body : \"周报会\"") == std::vector<int64_t>{m1});
  CHECK(fts_match(c, "attach_names : \"议程.d\"") == std::vector<int64_t>{m1});
  CHECK(fts_match(c, "from_text : \"alice@team\"") == std::vector<int64_t>{m2});
  CHECK(fts_match(c, "\"周报表\"").empty());
  CHECK(fts_match(c, "\"进展\"").empty());  // < 3 chars never match via MATCH (trigram)
  CHECK(fts_match(c, "\"进展\" OR \"report\"") == std::vector<int64_t>{m2});
  CHECK(fts_match(c, "\"本周进展\"") == std::vector<int64_t>{m3});

  // 1-2 character terms fall back to LIKE on the FTS columns (DESIGN §2 search queries).
  CHECK(c.scalar<int64_t>("SELECT COUNT(*) FROM message_fts WHERE subject LIKE '%周报%'").value() == 2);

  // Deleting a message removes its FTS row via the trigger.
  c.run("DELETE FROM messages WHERE id = ?", m1);
  CHECK(fts_match(c, "\"周报会\"").empty());
  CHECK(c.scalar<int64_t>("SELECT COUNT(*) FROM message_fts").value() == 2);
}

TEST_CASE("migrate through a Pool lease, then use the pool", "[migrations]") {
  test::TempDir td;
  Pool pool(td / "p.db", 2);
  {
    auto lease = pool.acquire();
    check_capabilities(*lease);
    migrate(*lease);
  }
  const int64_t domain_id = pool.write([](Tx& tx) {
    tx.run("INSERT INTO domains(name, created_at) VALUES(?, ?)", "team.com", 1);
    return tx.last_insert_id();
  });
  CHECK(pool.read([](Conn& c) { return current_version(c); }) == 1);
  CHECK(pool.read([&](Conn& c) {
    return c.scalar<std::string>("SELECT name FROM domains WHERE id=?", domain_id).value();
  }) == "team.com");
}
