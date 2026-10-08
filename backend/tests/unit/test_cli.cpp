// Owner: WP-A — CLI parsing/exit codes and the commands that only need WP0 + WP-A (migrate,
// backup, doctor --offline, version, help); operational helpers (blob migration, tmp sweep).
// Commands that need WP-D/WP-B implementations are tagged [.integration].
#include "app/cli.hpp"
#include "app/support.hpp"
#include "core/blob_store.hpp"
#include "core/crypto.hpp"
#include "db/migrations.hpp"
#include "db/sqlite.hpp"
#include "test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <thread>

#include <atomic>

#include <fstream>
#include <sstream>

using namespace azm;

namespace {

struct CliResult {
  int code = -1;
  std::string out, err;
};

CliResult cli(std::vector<std::string> args, const std::string& stdin_text = "") {
  args.insert(args.begin(), "azmail");
  std::vector<char*> argv;
  for (auto& a : args) argv.push_back(a.data());
  argv.push_back(nullptr);
  std::istringstream in(stdin_text);
  std::ostringstream out, err;
  CliResult r;
  r.code = app::run_cli(static_cast<int>(args.size()), argv.data(), in, out, err);
  r.out = out.str();
  r.err = err.str();
  return r;
}

bool has(const std::string& s, std::string_view needle) { return s.find(needle) != std::string::npos; }

// 64 hex characters, like `openssl rand -hex 32` (passes the AZMAIL_SECRET strength check).
constexpr std::string_view kStrongSecret = "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08";

// An env file making `serve`/`doctor` validation pass, rooted in `dir`.
std::string write_env(const test::TempDir& dir) {
  const auto path = dir / "azmail.env";
  std::ofstream f(path);
  f << "AZMAIL_DATA_DIR=" << (dir / "data").string() << "\n"
    << "AZMAIL_SECRET=" << kStrongSecret << "\n"
    << "RESEND_API_KEY=re_test\n"
    << "AZMAIL_LOG_LEVEL=error\n";
  return path.string();
}

// A LocalBlobStore that reports another kind (stands in for R2 in migration tests).
class KindAs final : public BlobStore {
 public:
  KindAs(std::unique_ptr<BlobStore> inner, std::string kind) : inner_(std::move(inner)), kind_(std::move(kind)) {}
  std::string_view kind() const override { return kind_; }
  std::filesystem::path tmp_dir() const override { return inner_->tmp_dir(); }
  BlobRef put_file(const std::filesystem::path& p, std::optional<std::string> sha) override {
    auto r = inner_->put_file(p, std::move(sha));
    r.storage = kind_;
    return r;
  }
  BlobRef put_bytes(std::string_view b) override {
    auto r = inner_->put_bytes(b);
    r.storage = kind_;
    return r;
  }
  void get_to_file(std::string_view sha, const std::filesystem::path& d) override { inner_->get_to_file(sha, d); }
  std::string get_bytes(std::string_view sha, std::size_t max) override { return inner_->get_bytes(sha, max); }
  bool exists(std::string_view sha) override { return inner_->exists(sha); }
  void remove(std::string_view sha) override { inner_->remove(sha); }
  ServePlan serve(std::string_view sha, const ServeOptions& o) override { return inner_->serve(sha, o); }
  std::vector<std::string> public_origins() const override { return {}; }

 private:
  std::unique_ptr<BlobStore> inner_;
  std::string kind_;
};

}  // namespace

TEST_CASE("cli: version, help and usage errors", "[cli]") {
  auto r = cli({"version"});
  CHECK(r.code == 0);
  CHECK(r.out == std::string("azmail ") + AZMAIL_VERSION + "\n");
  CHECK(cli({"--version"}).code == 0);

  r = cli({"--help"});
  CHECK(r.code == 0);
  CHECK(has(r.out, "blobs-migrate"));
  CHECK(has(r.out, "用法"));
  r = cli({});
  CHECK(r.code == 2);
  CHECK(has(r.out, "Usage"));
  CHECK(cli({"help"}).code == 0);

  r = cli({"create-user", "--help"});
  CHECK(r.code == 0);
  CHECK(has(r.out, "--create-domain"));
  r = cli({"--help", "backup"});
  CHECK(r.code == 0);
  CHECK(has(r.out, "--out"));

  r = cli({"frobnicate"});
  CHECK(r.code == 2);
  CHECK(has(r.err, "unknown command"));
  r = cli({"migrate", "--bogus"});
  CHECK(r.code == 2);
  r = cli({"--set", "NOEQUALS", "migrate"});
  CHECK(r.code == 2);
  CHECK(has(r.err, "KEY=VALUE"));
  r = cli({"--env-file", "/nonexistent/azmail.env", "migrate"});
  CHECK(r.code == 2);
  CHECK(has(r.err, "cannot read env file"));
  // Guessing of abbreviated options is disabled ("--out" must be spelled out).
  CHECK(cli({"backup", "--ou", "x"}).code == 2);
}

TEST_CASE("cli: configuration errors are usage errors listing every problem", "[cli]") {
  test::TempDir td;
  auto r = cli({"serve", "--set", "AZMAIL_DATA_DIR=" + (td / "d").string(), "--set", "AZMAIL_SECRET=",
                "--set", "RESEND_API_KEY=", "--set", "AZMAIL_CORS_ORIGINS=*", "--set", "AZMAIL_BLOB_BACKEND=s3"});
  CHECK(r.code == 2);
  // Unparseable values and semantic problems come back together.
  CHECK(has(r.err, "AZMAIL_BLOB_BACKEND"));
  CHECK(has(r.err, "AZMAIL_SECRET"));
  CHECK(has(r.err, "RESEND_API_KEY"));
  CHECK(has(r.err, "AZMAIL_CORS_ORIGINS"));
  r = cli({"migrate", "--set", "AZMAIL_PORT=notaport", "--data-dir", (td / "d").string()});
  CHECK(r.code == 2);
  CHECK(has(r.err, "AZMAIL_PORT"));
}

TEST_CASE("cli: migrate creates and upgrades the database (idempotent)", "[cli]") {
  test::TempDir td;
  const auto db = td / "nested" / "dir" / "azmail.db";
  auto r = cli({"migrate", "--db-path", db.string(), "--data-dir", (td / "data").string()});
  REQUIRE(r.code == 0);
  CHECK(has(r.out, "schema version " + std::to_string(db::latest_version())));
  CHECK(has(r.out, "migrated"));
  CHECK(std::filesystem::exists(db));
  r = cli({"migrate", "--db-path", db.string()});
  CHECK(r.code == 0);
  CHECK(has(r.out, "up to date"));
  {
    db::Conn c(db);
    CHECK(db::current_version(c) == db::latest_version());
  }
}

TEST_CASE("cli: commands needing the database fail clearly before migrate", "[cli]") {
  test::TempDir td;
  const std::string db = (td / "missing.db").string();
  auto r = cli({"backup", "--out", (td / "b.db").string(), "--db-path", db});
  CHECK(r.code == 1);
  CHECK(has(r.err, "azmail migrate"));
  CHECK_FALSE(std::filesystem::exists(db));
  r = cli({"reindex", "--db-path", db});
  CHECK(r.code == 1);
}

TEST_CASE("cli: argument validation for account commands", "[cli]") {
  test::TempDir td;
  const std::string db = (td / "a.db").string();
  REQUIRE(cli({"migrate", "--db-path", db}).code == 0);
  auto r = cli({"create-user", "--db-path", db});
  CHECK(r.code == 2);
  CHECK(has(r.err, "--email"));
  r = cli({"create-user", "--email", "not-an-email", "--db-path", db});
  CHECK(r.code == 2);
  r = cli({"create-user", "--email", "a@x.cn", "--db-path", db}, "");
  CHECK(r.code == 2);
  CHECK(has(r.err, "password"));
  r = cli({"create-user", "--email", "a@x.cn", "--password-file", (td / "nope").string(), "--db-path", db});
  CHECK(r.code == 2);
  r = cli({"reset-password", "--db-path", db});
  CHECK(r.code == 2);
  r = cli({"add-domain", "--db-path", db});
  CHECK(r.code == 2);
  r = cli({"blobs-migrate", "--db-path", db});
  CHECK(r.code == 2);
  r = cli({"blobs-migrate", "--to", "s3", "--db-path", db});
  CHECK(r.code == 2);
  r = cli({"blobs-migrate", "--to", "r2", "--db-path", db});
  CHECK(r.code == 2);
  CHECK(has(r.err, "R2"));
  r = cli({"backup", "--db-path", db});
  CHECK(r.code == 2);
}

TEST_CASE("cli: backup uses the online backup API and verifies the copy", "[cli]") {
  test::TempDir td;
  const std::string db = (td / "live.db").string();
  REQUIRE(cli({"migrate", "--db-path", db}).code == 0);
  {
    db::Pool pool(db, 1);
    pool.write([](db::Tx& tx) { tx.run("INSERT INTO kv(key,value,updated_at) VALUES('k','v',1)"); });
    // Keep a connection open with WAL content while the backup runs.
    auto lease = pool.acquire();
    const std::string out = (td / "backups" / "b1.db").string();
    auto r = cli({"backup", "--out", out, "--db-path", db});
    REQUIRE(r.code == 0);
    CHECK(has(r.out, "backup written"));
    db::Conn copy(out);
    CHECK(copy.scalar<std::string>("SELECT value FROM kv WHERE key='k'") == std::optional<std::string>("v"));
    CHECK(db::current_version(copy) == db::latest_version());
  }
  const std::string out = (td / "backups" / "b1.db").string();
  auto r = cli({"backup", "--out", out, "--db-path", db});
  CHECK(r.code == 1);
  CHECK(has(r.err, "--force"));
  r = cli({"backup", "--to", out, "--force", "--db-path", db});
  CHECK(r.code == 0);
  // No temporary files are left next to the backup.
  std::size_t files = 0;
  for (const auto& e : std::filesystem::directory_iterator(td / "backups")) files += e.is_regular_file() ? 1 : 0;
  CHECK(files == 1);
  CHECK_THROWS_AS(app::backup_database(db, db, true), std::runtime_error);
}

TEST_CASE("support: backup finishes while another connection keeps writing (RT-9)", "[cli][support]") {
  test::TempDir td;
  const std::string db = (td / "live.db").string();
  REQUIRE(cli({"migrate", "--db-path", db}).code == 0);
  db::Pool pool(db, 2);
  // ~24 MB (≈ 6000 pages): a stepwise copy would need many steps, each restarted by a write.
  pool.write([](db::Tx& tx) {
    const std::string blob(4000, 'z');
    for (int i = 0; i < 6000; ++i)
      tx.run("INSERT INTO kv(key,value,updated_at) VALUES(?,?,1)", "fill" + std::to_string(i), blob);
  });
  std::atomic<bool> done{false};
  std::atomic<int> writes{0};
  std::thread writer([&] {
    for (int i = 0; !done.load(); ++i) {
      pool.write([&](db::Tx& tx) {
        tx.run("INSERT OR REPLACE INTO kv(key,value,updated_at) VALUES('tick',?,?)", std::to_string(i), i);
      });
      ++writes;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  });
  const auto t0 = std::chrono::steady_clock::now();
  std::uint64_t size = 0;
  CHECK_NOTHROW(size = app::backup_database(db, td / "copy.db", false));
  const auto took = std::chrono::steady_clock::now() - t0;
  done = true;
  writer.join();
  CHECK(took < std::chrono::seconds(60));
  CHECK(writes.load() > 0);
  CHECK(size > 20'000'000u);
  db::Conn copy((td / "copy.db").string());
  CHECK(copy.scalar<int64_t>("SELECT count(*) FROM kv WHERE key LIKE 'fill%'") == std::optional<int64_t>(6000));
  CHECK(copy.scalar<std::string>("PRAGMA quick_check") == std::optional<std::string>("ok"));
}

TEST_CASE("cli: doctor --offline", "[cli]") {
  test::TempDir td;
  const auto env = write_env(td);
  auto r = cli({"--env-file", env, "doctor", "--offline"});
  CHECK(r.code == 1);  // no database yet
  CHECK(has(r.out, "[FAIL] database"));
  CHECK(has(r.out, "[ OK ] configuration"));
  REQUIRE(cli({"--env-file", env, "migrate"}).code == 0);
  r = cli({"--env-file", env, "doctor", "--offline"});
  CHECK(r.code == 0);
  CHECK(has(r.out, "[ OK ] database schema"));
  CHECK(has(r.out, "[ OK ] local storage writable"));
  CHECK(has(r.out, "[WARN] RESEND_WEBHOOK_SECRET"));
  CHECK(has(r.out, "[WARN] no mail domain yet"));
  CHECK(has(r.out, "offline"));
  CHECK(has(r.out, "0 failure(s)"));
  {
    db::Pool pool(td / "data" / "azmail.db", 1);
    pool.write([](db::Tx& tx) { test::seed_domain(tx, "team.example"); });
  }
  r = cli({"--env-file", env, "--set", "AZMAIL_LOCAL_DOMAINS=team.example,other.example", "doctor", "--offline"});
  CHECK(r.code == 0);
  CHECK_FALSE(has(r.out, "no mail domain yet"));
  CHECK(has(r.out, "lists other.example"));
  CHECK_FALSE(has(r.out, "lists team.example"));
  // Serve-level problems are reported (not fatal to running doctor itself).
  r = cli({"--env-file", env, "--set", "AZMAIL_SECRET=short", "doctor", "--offline"});
  CHECK(r.code == 1);
  CHECK(has(r.out, "[FAIL] configuration: AZMAIL_SECRET is too short"));
  // F2: the unedited placeholder of deploy/azmail.env.example is an error, not "[ OK ]".
  r = cli({"--env-file", env, "--set", "AZMAIL_SECRET=CHANGE_ME_generate_with_openssl_rand_hex_32", "doctor",
           "--offline"});
  CHECK(r.code == 1);
  CHECK(has(r.out, "[FAIL] configuration: AZMAIL_SECRET still holds the example placeholder"));
  CHECK_FALSE(has(r.out, "[ OK ] configuration"));
  r = cli({"--env-file", env, "--set", "RESEND_WEBHOOK_SECRET=whsec_xxxxxxxxxxxxxxxxxxxxxxxx", "doctor", "--offline"});
  CHECK(r.code == 1);
  CHECK(has(r.out, "[FAIL] configuration: RESEND_WEBHOOK_SECRET is not a real signing secret"));
}

TEST_CASE("support: r2_configured and sweep_tmp_dir", "[cli][support]") {
  Config c;
  CHECK_FALSE(app::r2_configured(c));
  c.r2_access_key_id = "a";
  c.r2_secret_access_key = "b";
  c.r2_bucket = "c";
  CHECK_FALSE(app::r2_configured(c));  // no endpoint / account
  c.r2_account_id = "acct";
  CHECK(app::r2_configured(c));
  Config d;
  d.blob_backend = BlobBackend::R2;
  CHECK(app::r2_configured(d));

  test::TempDir td;
  const auto old_file = td / "old.part";
  const auto new_file = td / "new.part";
  std::ofstream(old_file) << "x";
  std::ofstream(new_file) << "y";
  std::filesystem::create_directories(td / "subdir");
  std::filesystem::last_write_time(old_file, std::filesystem::file_time_type::clock::now() - std::chrono::hours(48));
  CHECK(app::sweep_tmp_dir(td.path(), std::chrono::hours(24)) == 1);
  CHECK_FALSE(std::filesystem::exists(old_file));
  CHECK(std::filesystem::exists(new_file));
  CHECK(std::filesystem::exists(td / "subdir"));
  CHECK(app::sweep_tmp_dir(td / "missing", std::chrono::hours(24)) == 0);
}

TEST_CASE("support: migrate_blobs copies, verifies, flips and deletes", "[cli][support]") {
  test::TempDir td;
  db::Pool pool(td / "m.db", 2);
  test::migrate(pool);
  auto src = make_local_blob_store(td / "src");
  KindAs dst(make_local_blob_store(td / "dst"), "r2");

  std::vector<BlobRef> refs;
  for (const char* content : {"alpha", "bravo", "charlie", "delta"}) refs.push_back(src->put_bytes(content));
  pool.write([&](db::Tx& tx) {
    for (const auto& b : refs)
      tx.run("INSERT INTO blobs(sha256,size,storage,created_at) VALUES(?,?,?,1)", b.sha256, b.size, "local");
    // A row already in r2 is not touched.
    tx.run("INSERT INTO blobs(sha256,size,storage,created_at) VALUES(?,?,?,1)", std::string(64, 'e'), 3, "r2");
  });
  // Corrupt one source object and delete another.
  {
    std::ofstream(td / "src" / blob_relative_key(refs[2].sha256), std::ios::binary | std::ios::trunc) << "tampered";
    src->remove(refs[3].sha256);
  }

  app::BlobMigrateOptions dry;
  dry.dry_run = true;
  auto st = app::migrate_blobs(pool, *src, dst, dry);
  CHECK(st.candidates == 4);
  CHECK(st.migrated == 0);
  CHECK(st.bytes == 5 + 5 + 7 + 5);
  CHECK_FALSE(dst.exists(refs[0].sha256));

  std::vector<std::string> failed;
  app::BlobMigrateOptions opts;
  opts.batch = 1;  // exercise pagination
  opts.on_blob = [&](std::string_view sha, bool ok, std::string_view) {
    if (!ok) failed.emplace_back(sha);
  };
  st = app::migrate_blobs(pool, *src, dst, opts);
  CHECK(st.candidates == 4);
  CHECK(st.migrated == 2);
  CHECK(st.failed == 2);
  CHECK(failed.size() == 2);
  for (int i : {0, 1}) {
    CHECK(dst.exists(refs[i].sha256));
    CHECK_FALSE(src->exists(refs[i].sha256));
    CHECK(dst.get_bytes(refs[i].sha256, 100) == (i == 0 ? "alpha" : "bravo"));
  }
  pool.read([&](db::Conn& c) {
    CHECK(c.scalar<int64_t>("SELECT COUNT(*) FROM blobs WHERE storage='r2'") == std::optional<int64_t>(3));
    CHECK(c.scalar<std::string>("SELECT storage FROM blobs WHERE sha256=?", refs[2].sha256) ==
          std::optional<std::string>("local"));
  });
  CHECK_FALSE(dst.exists(refs[2].sha256));  // the corrupt object was never uploaded
  // Nothing left in the staging dir.
  CHECK(std::filesystem::is_empty(dst.tmp_dir()));

  // Back again: r2 → local (all three r2 rows; the fake 'eee…' object does not exist → failed).
  st = app::migrate_blobs(pool, dst, *src);
  CHECK(st.candidates == 3);
  CHECK(st.migrated == 2);
  CHECK(st.failed == 1);
  CHECK(src->get_bytes(refs[0].sha256, 100) == "alpha");

  CHECK_THROWS_AS(app::migrate_blobs(pool, *src, *src), std::invalid_argument);
}

// ---- needs WP-D (repo) / WP-B (fts) ------------------------------------------------------------

TEST_CASE("cli: create-user, add-domain, reset-password", "[cli][.integration]") {
  test::TempDir td;
  const std::string db = (td / "u.db").string();
  REQUIRE(cli({"migrate", "--db-path", db}).code == 0);
  auto r = cli({"create-user", "--email", "Admin@Team.example", "--name", "管理员", "--admin", "--db-path", db},
               "correct horse battery\n");
  CHECK(r.code == 1);  // unknown domain
  CHECK(has(r.err, "unknown_domain"));
  r = cli({"create-user", "--email", "admin@team.example", "--admin", "--create-domain", "--db-path", db},
          "correct horse battery\n");
  REQUIRE(r.code == 0);
  CHECK(has(r.out, "admin@team.example (admin)"));
  r = cli({"add-domain", "--name", "Other.example", "--db-path", db});
  CHECK(r.code == 0);
  CHECK(has(r.out, "other.example"));
  CHECK(cli({"add-domain", "--name", "other.example", "--db-path", db}).code == 1);  // domain_exists
  r = cli({"create-user", "--email", "weak@team.example", "--db-path", db}, "short\n");
  CHECK(r.code == 1);
  CHECK(has(r.err, "weak_password"));
  {
    db::Pool pool(db, 1);
    pool.write([](db::Tx& tx) {
      const auto uid = tx.scalar<int64_t>("SELECT id FROM users WHERE email='admin@team.example'").value();
      test::seed_session(tx, uid);
    });
  }
  const auto pwfile = td / "pw.txt";
  std::ofstream(pwfile) << "another good password\n";
  r = cli({"reset-password", "--email", "admin@team.example", "--password-file", pwfile.string(), "--db-path", db});
  CHECK(r.code == 0);
  CHECK(has(r.out, "1 session(s) revoked"));
  CHECK(cli({"reset-password", "--email", "ghost@team.example", "--db-path", db}, "whatever pass\n").code == 1);
}

TEST_CASE("cli: reindex", "[cli][.integration]") {
  test::TempDir td;
  const std::string db = (td / "r.db").string();
  REQUIRE(cli({"migrate", "--db-path", db}).code == 0);
  auto r = cli({"reindex", "--db-path", db});
  CHECK(r.code == 0);
  CHECK(has(r.out, "reindexed 0"));
}
