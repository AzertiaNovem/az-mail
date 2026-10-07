#include "db/sqlite.hpp"
#include "notifier.hpp"
#include "test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <thread>

using namespace azm;
using namespace azm::db;

namespace {

void create_schema(Pool& pool) {
  pool.write([](Tx& tx) {
    tx.exec(
        "CREATE TABLE t(id INTEGER PRIMARY KEY, v TEXT, n INTEGER, d REAL, b BLOB);"
        "CREATE TABLE counter(id INTEGER PRIMARY KEY, n INTEGER NOT NULL);"
        "INSERT INTO counter(id, n) VALUES(1, 0);"
        "CREATE TABLE u(email TEXT NOT NULL UNIQUE);"
        "CREATE TABLE parent(id INTEGER PRIMARY KEY);"
        "CREATE TABLE child(id INTEGER PRIMARY KEY, parent_id INTEGER NOT NULL REFERENCES parent(id));");
  });
}

int64_t count_rows(Pool& pool, std::string_view table) {
  return pool.read([&](Conn& c) {
    return c.scalar<int64_t>("SELECT COUNT(*) FROM " + std::string(table)).value_or(-1);
  });
}

// Notifier that records the order of hook invocations into a shared log.
struct LoggingNotifier final : Notifier {
  std::vector<std::string>* log;
  explicit LoggingNotifier(std::vector<std::string>* l) : log(l) {}
  void publish(int64_t user_id, std::string type, boost::json::object) override {
    log->push_back("emit:" + std::to_string(user_id) + ":" + type);
  }
  void revoke_session(int64_t) override {}
  void revoke_user(int64_t) override {}
};

}  // namespace

TEST_CASE("connection pragmas", "[db]") {
  test::TempDir td;
  Pool pool(td / "p.db", 2);
  pool.read([](Conn& c) {
    CHECK(c.scalar<std::string>("PRAGMA journal_mode").value() == "wal");
    CHECK(c.scalar<int64_t>("PRAGMA synchronous").value() == 1);  // NORMAL
    CHECK(c.scalar<int64_t>("PRAGMA foreign_keys").value() == 1);
    CHECK(c.scalar<int64_t>("PRAGMA busy_timeout").value() == 5000);
    CHECK(c.scalar<int64_t>("PRAGMA temp_store").value() == 2);  // MEMORY
    CHECK(c.scalar<int64_t>("PRAGMA journal_size_limit").value() == 67108864);
    CHECK(c.in_transaction());  // read() runs inside BEGIN DEFERRED
  });
  CHECK(pool.size() == 2);
  CHECK(pool.path() == td / "p.db");
  Pool mem(":memory:", 8);
  CHECK(mem.size() == 1);
}

TEST_CASE("write/read with every bind type", "[db]") {
  test::TempDir td;
  Pool pool(td / "b.db", 2);
  create_schema(pool);

  const int64_t id = pool.write([](Tx& tx) {
    tx.run("INSERT INTO t(v, n, d, b) VALUES(?, ?, ?, ?)", std::string("str"), 1, 2.5,
           std::vector<uint8_t>{0, 1, 2, 255});
    tx.run("INSERT INTO t(v, n, d, b) VALUES(?, ?, ?, ?)", std::string_view("sv"), int64_t{1} << 40,
           1.0f, nullptr);
    tx.run("INSERT INTO t(v, n, d, b) VALUES(?, ?, ?, ?)", "cstr", 7u, std::optional<double>{},
           std::nullopt);
    tx.run("INSERT INTO t(v, n) VALUES(?, ?)", std::optional<std::string>{"opt"}, true);
    tx.run("INSERT INTO t(v, n) VALUES(?, ?)", static_cast<const char*>(nullptr),
           std::optional<int64_t>{});
    tx.run("INSERT INTO t(v, n, b) VALUES(?, ?, ?)", 'i', Value{int64_t{9}}, std::vector<uint8_t>{});
    tx.run("INSERT INTO t(v, n) VALUES(?, ?)", std::string(), uint64_t{42});
    return tx.last_insert_id();
  });
  CHECK(id == 7);

  pool.read([](Conn& c) {
    Stmt s = c.prepare("SELECT id, v, n, d, b FROM t ORDER BY id");
    REQUIRE(s.step());
    CHECK(s.text(1) == "str");
    CHECK(s.i64(2) == 1);
    CHECK(s.dbl(3) == 2.5);
    CHECK(s.blob(4) == std::vector<uint8_t>{0, 1, 2, 255});
    CHECK(s.blob_str(4) == std::string("\0\1\2\xff", 4));
    REQUIRE(s.step());
    CHECK(s.text(1) == "sv");
    CHECK(s.i64(2) == (int64_t{1} << 40));
    CHECK(s.is_null(4));
    REQUIRE(s.step());
    CHECK(s.text(1) == "cstr");
    CHECK(s.i64(2) == 7);
    CHECK_FALSE(s.opt_dbl(3).has_value());
    CHECK(s.is_null(4));
    REQUIRE(s.step());
    CHECK(s.opt_text(1) == "opt");
    CHECK(s.boolean(2));
    REQUIRE(s.step());
    CHECK_FALSE(s.opt_text(1).has_value());
    CHECK_FALSE(s.opt_i64(2).has_value());
    CHECK(s.text(1) == "");
    CHECK(s.i64(2) == 0);
    REQUIRE(s.step());
    CHECK(s.text(1) == "i");  // char binds as 1-char text
    CHECK(s.i64(2) == 9);
    CHECK_FALSE(s.is_null(4));  // empty vector → X'' not NULL
    CHECK(s.blob(4).empty());
    REQUIRE(s.step());
    CHECK_FALSE(s.is_null(1));  // empty string → '' not NULL
    CHECK(s.get<int>(2) == 42);
    CHECK(std::get<int64_t>(s.value(2)) == 42);
    CHECK(std::holds_alternative<std::string>(s.value(1)));
    CHECK(s.column_count() == 5);
    CHECK(s.column_name(1) == "v");
    CHECK_FALSE(s.step());
  });

  // scalar(): no row / NULL → nullopt.
  pool.read([](Conn& c) {
    CHECK_FALSE(c.scalar<int64_t>("SELECT n FROM t WHERE id = ?", 999).has_value());
    CHECK_FALSE(c.scalar<int64_t>("SELECT NULL").has_value());
    CHECK(c.scalar<std::string>("SELECT v FROM t WHERE id = ?", 1).value() == "str");
    CHECK(c.scalar<double>("SELECT d FROM t WHERE id = 1").value() == 2.5);
    CHECK(c.scalar<bool>("SELECT 1").value());
  });

  CHECK_THROWS_AS(pool.write([](Tx& tx) {
    tx.run("INSERT INTO t(n) VALUES(?)", std::numeric_limits<uint64_t>::max());
  }), std::out_of_range);
}

TEST_CASE("statement cache and prepare errors", "[db]") {
  test::TempDir td;
  Pool pool(td / "c.db", 1);
  create_schema(pool);
  auto lease = pool.acquire();
  Conn& c = *lease;
  const std::size_t before = c.cached_statements();
  for (int i = 0; i < 10; ++i) c.run("INSERT INTO t(n) VALUES(?)", i);
  CHECK(c.cached_statements() == before + 1);  // one cached statement reused

  {  // Same SQL prepared while another instance is still alive (nested iteration).
    Stmt outer = c.prepare("SELECT n FROM t ORDER BY id");
    int rows = 0;
    while (outer.step()) {
      Stmt inner = c.prepare("SELECT n FROM t ORDER BY id");
      REQUIRE(inner.step());
      CHECK(inner.i64(0) == 0);
      ++rows;
    }
    CHECK(rows == 10);
  }
  // Re-used statements start with cleared bindings.
  Stmt s = c.prepare("SELECT ?");
  s.bind_all(5);
  REQUIRE(s.step());
  CHECK(s.i64(0) == 5);
  s = c.prepare("SELECT 1");
  Stmt s2 = c.prepare("SELECT ?");
  REQUIRE(s2.step());
  CHECK(s2.is_null(0));

  CHECK_THROWS_AS(c.prepare("SELECT 1; SELECT 2"), Error);
  CHECK_NOTHROW(c.prepare("SELECT 1;  \n"));
  try {
    c.prepare("SELEC nonsense FROM nowhere");
    FAIL("expected db::Error");
  } catch (const Error& e) {
    CHECK(std::string(e.what()).find("SELEC nonsense") != std::string::npos);
    CHECK(e.code == 1);  // SQLITE_ERROR
  }
  c.run("INSERT INTO u(email) VALUES(?)", "a@x.com");
  try {
    c.run("INSERT INTO u(email) VALUES(?)", "a@x.com");
    FAIL("expected unique violation");
  } catch (const Error& e) {
    CHECK(e.is_constraint());
    CHECK(e.is_unique_violation());
  }
  try {
    c.run("INSERT INTO child(parent_id) VALUES(?)", 12345);
    FAIL("expected foreign key violation");
  } catch (const Error& e) {
    CHECK(e.is_foreign_key_violation());
  }
}

TEST_CASE("rollback discards emits, after_commit and wake", "[db][tx]") {
  test::TempDir td;
  RecordingNotifier rec;
  std::atomic<int> wakes{0};
  Pool pool(td / "r.db", 2, TxHooks{&rec, [&] { ++wakes; }});
  create_schema(pool);
  bool after_called = false;

  CHECK_THROWS_AS(pool.write([&](Tx& tx) {
    tx.run("INSERT INTO t(v) VALUES('doomed')");
    tx.emit(1, "mail.new", {{"x", 1}});
    tx.after_commit([&] { after_called = true; });
    tx.wake_jobs();
    throw std::runtime_error("boom");
  }), std::runtime_error);

  CHECK(rec.size() == 0);
  CHECK_FALSE(after_called);
  CHECK(wakes == 0);
  CHECK(count_rows(pool, "t") == 0);

  // A db::Error inside the closure also rolls back.
  CHECK_THROWS_AS(pool.write([&](Tx& tx) {
    tx.run("INSERT INTO t(v) VALUES('doomed2')");
    tx.emit(1, "mail.new", {});
    tx.run("INSERT INTO nope VALUES(1)");
  }), Error);
  CHECK(rec.size() == 0);
  CHECK(count_rows(pool, "t") == 0);
}

TEST_CASE("emits are flushed only after commit, in order", "[db][tx]") {
  test::TempDir td;
  RecordingNotifier rec;
  Pool pool(td / "e.db", 1, TxHooks{&rec, {}});
  create_schema(pool);
  int64_t rows_seen_after_commit = -1;

  pool.write([&](Tx& tx) {
    tx.run("INSERT INTO t(v) VALUES('a')");
    tx.emit(10, "mail.new", {{"thread_id", 1}});
    tx.emit(11, "threads.changed", {{"thread_ids", boost::json::array{1, 2}}});
    CHECK(rec.size() == 0);  // nothing published inside the transaction
    // Runs after COMMIT with the connection returned: a fresh read must see the row,
    // even though this pool has a single connection.
    tx.after_commit([&] { rows_seen_after_commit = count_rows(pool, "t"); });
  });

  CHECK(rows_seen_after_commit == 1);
  const auto ev = rec.events();
  REQUIRE(ev.size() == 2);
  CHECK(ev[0].user_id == 10);
  CHECK(ev[0].type == "mail.new");
  CHECK(ev[0].data.at("thread_id").as_int64() == 1);
  CHECK(ev[1].user_id == 11);
  CHECK(ev[1].type == "threads.changed");
  CHECK(ev[1].data.at("thread_ids").as_array().size() == 2);
  CHECK(rec.events_of("mail.new").size() == 1);
}

TEST_CASE("hook order: emits, then after_commit in order, then a single wake", "[db][tx]") {
  test::TempDir td;
  std::vector<std::string> log;
  LoggingNotifier notifier(&log);
  Pool pool(td / "o.db", 1);
  pool.set_hooks(TxHooks{&notifier, [&] { log.push_back("wake"); }});
  create_schema(pool);
  log.clear();

  pool.write([&](Tx& tx) {
    tx.after_commit([&] { log.push_back("after:1"); });
    tx.emit(1, "a", {});
    tx.wake_jobs();
    tx.after_commit([&] { log.push_back("after:2"); });
    tx.emit(2, "b", {});
    tx.wake_jobs();
    tx.wake_jobs();
    tx.after_commit([&] { throw std::runtime_error("ignored"); });  // logged, not rethrown
    tx.after_commit([&] { log.push_back("after:3"); });
  });
  CHECK(log == std::vector<std::string>{"emit:1:a", "emit:2:b", "after:1", "after:2", "after:3", "wake"});

  // No wake requested → no wake.
  log.clear();
  pool.write([&](Tx& tx) { tx.emit(3, "c", {}); });
  CHECK(log == std::vector<std::string>{"emit:3:c"});
}

TEST_CASE("nested transactions on the same pool are rejected", "[db][tx]") {
  test::TempDir td;
  Pool pool(td / "n.db", 2);
  create_schema(pool);
  CHECK_THROWS_AS(pool.write([&](Tx& tx) {
    tx.run("INSERT INTO t(v) VALUES('outer')");
    pool.write([](Tx&) {});
  }), std::logic_error);
  CHECK_THROWS_AS(pool.write([&](Tx&) { return count_rows(pool, "t"); }), std::logic_error);
  CHECK_THROWS_AS(pool.read([&](Conn&) { pool.read([](Conn&) {}); }), std::logic_error);
  CHECK(count_rows(pool, "t") == 0);  // outer rolled back
  // The guard is released afterwards.
  CHECK_NOTHROW(pool.write([](Tx& tx) { tx.run("INSERT INTO t(v) VALUES('ok')"); }));
  CHECK(count_rows(pool, "t") == 1);
}

TEST_CASE("concurrent writers from 4 threads all succeed", "[db][tx][concurrency]") {
  test::TempDir td;
  RecordingNotifier rec;
  Pool pool(td / "w.db", 4, TxHooks{&rec, {}});
  create_schema(pool);
  constexpr int kThreads = 4, kIters = 50;
  std::atomic<int> failures{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      for (int i = 0; i < kIters; ++i) {
        try {
          pool.write([&](Tx& tx) {
            // Read-modify-write: only safe because write() holds the write lock (IMMEDIATE).
            const int64_t n = tx.scalar<int64_t>("SELECT n FROM counter WHERE id = 1").value();
            tx.run("UPDATE counter SET n = ? WHERE id = 1", n + 1);
            tx.run("INSERT INTO t(v, n) VALUES(?, ?)", "thread", t);
            tx.emit(t, "tick", {});
          });
        } catch (...) {
          ++failures;
        }
      }
    });
  }
  // Concurrent readers too (no Catch2 assertions off the main thread).
  std::atomic<int> bad_reads{0};
  std::thread reader([&] {
    int64_t last = 0;
    for (int i = 0; i < 50; ++i) {
      try {
        const int64_t n = pool.read([](Conn& c) { return c.scalar<int64_t>("SELECT n FROM counter").value(); });
        if (n < last) ++bad_reads;  // committed counter never goes backwards
        last = n;
      } catch (...) {
        ++bad_reads;
      }
    }
  });
  for (auto& th : threads) th.join();
  reader.join();
  CHECK(failures == 0);
  CHECK(bad_reads == 0);
  CHECK(pool.read([](Conn& c) { return c.scalar<int64_t>("SELECT n FROM counter").value(); }) ==
        kThreads * kIters);
  CHECK(count_rows(pool, "t") == kThreads * kIters);
  CHECK(rec.size() == static_cast<std::size_t>(kThreads * kIters));
}

TEST_CASE("BusyError after retries when another connection holds the write lock", "[db][tx][busy]") {
  test::TempDir td;
  RecordingNotifier rec;
  Pool pool(td / "busy.db", 1, TxHooks{&rec, {}});
  create_schema(pool);
  pool.acquire()->set_busy_timeout(20);  // the pool's only connection

  Conn other(td / "busy.db");
  other.exec("BEGIN IMMEDIATE");
  other.exec("INSERT INTO t(v) VALUES('held')");

  int calls = 0;
  const auto t0 = std::chrono::steady_clock::now();
  CHECK_THROWS_AS(pool.write([&](Tx& tx) {
    ++calls;
    tx.emit(1, "never", {});
  }), BusyError);
  const auto elapsed = std::chrono::steady_clock::now() - t0;
  CHECK(calls == 0);  // BEGIN IMMEDIATE never succeeded
  CHECK(rec.size() == 0);
  // 5 attempts with 10+40+160+640 ms backoff (±20%) in between.
  CHECK(elapsed >= std::chrono::milliseconds(650));
  CHECK(elapsed < std::chrono::seconds(10));

  other.exec("ROLLBACK");
  CHECK_NOTHROW(pool.write([](Tx& tx) { tx.run("INSERT INTO t(v) VALUES('after')"); }));
  CHECK(count_rows(pool, "t") == 1);
}

TEST_CASE("write retries until a briefly held lock is released", "[db][tx][busy]") {
  test::TempDir td;
  Pool pool(td / "retry.db", 1);
  create_schema(pool);
  pool.acquire()->set_busy_timeout(20);

  Conn other(td / "retry.db");
  other.exec("BEGIN IMMEDIATE");
  std::thread releaser([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    other.exec("ROLLBACK");
  });
  int calls = 0;
  CHECK_NOTHROW(pool.write([&](Tx& tx) {
    ++calls;
    tx.run("INSERT INTO t(v) VALUES('eventually')");
  }));
  releaser.join();
  CHECK(calls == 1);
  CHECK(count_rows(pool, "t") == 1);
}

TEST_CASE("BusyError raised inside the closure re-runs the whole closure", "[db][tx][busy]") {
  test::TempDir td;
  RecordingNotifier rec;
  Pool pool(td / "rerun.db", 1, TxHooks{&rec, {}});
  create_schema(pool);
  int calls = 0;
  const int64_t result = pool.write([&](Tx& tx) {
    ++calls;
    tx.run("INSERT INTO t(v) VALUES(?)", "attempt");
    tx.emit(1, "attempt:" + std::to_string(calls), {});
    if (calls < 3) throw BusyError("simulated SQLITE_BUSY_SNAPSHOT");
    return int64_t{calls};
  });
  CHECK(result == 3);
  CHECK(calls == 3);
  CHECK(count_rows(pool, "t") == 1);  // failed attempts rolled back
  const auto ev = rec.events();
  REQUIRE(ev.size() == 1);  // emits of failed attempts discarded
  CHECK(ev[0].type == "attempt:3");

  // Persistent busy → gives up after kMaxAttempts.
  calls = 0;
  CHECK_THROWS_AS(pool.write([&](Tx&) {
    ++calls;
    throw BusyError("always busy");
  }), BusyError);
  CHECK(calls == Pool::kMaxAttempts);
}

TEST_CASE("leases block when the pool is exhausted and roll back on return", "[db]") {
  test::TempDir td;
  Pool pool(td / "lease.db", 1);
  create_schema(pool);
  std::atomic<bool> acquired{false};
  std::atomic<bool> was_in_tx{true};
  {
    auto lease = pool.acquire();
    lease->exec("BEGIN");
    lease->exec("INSERT INTO t(v) VALUES('uncommitted')");
    std::thread waiter([&] {
      auto l2 = pool.acquire();
      was_in_tx = l2->in_transaction();
      acquired = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK_FALSE(acquired);
    { auto moved = std::move(lease); }  // released here
    waiter.join();
  }
  CHECK(acquired);
  CHECK_FALSE(was_in_tx);  // the open transaction was rolled back on release
  CHECK(count_rows(pool, "t") == 0);
}
