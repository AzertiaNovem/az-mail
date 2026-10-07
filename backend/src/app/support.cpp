// Owner: WP-A
// Operational helpers (support.hpp): tmp sweeping, online backup, blob migration.
#include "app/support.hpp"

#include "core/crypto.hpp"
#include "core/log.hpp"
#include "db/sqlite.hpp"

#include <sqlite3.h>

#include <stdexcept>
#include <thread>
#include <vector>

namespace azm::app {

namespace fs = std::filesystem;

bool r2_configured(const Config& c) {
  return c.blob_backend == BlobBackend::R2 ||
         (!c.r2_access_key_id.empty() && !c.r2_secret_access_key.empty() && !c.r2_bucket.empty() &&
          !c.effective_r2_endpoint().empty());
}

std::size_t sweep_tmp_dir(const fs::path& dir, std::chrono::hours older_than) {
  std::size_t removed = 0;
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) return 0;
  const auto cutoff = fs::file_time_type::clock::now() - older_than;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code fe;
    if (!it->is_regular_file(fe) || fe) continue;
    const auto mtime = it->last_write_time(fe);
    if (fe || mtime >= cutoff) continue;
    if (fs::remove(it->path(), fe)) ++removed;
  }
  return removed;
}

// ---- backup ----------------------------------------------------------------------------------

namespace {

struct SqliteHandle {
  sqlite3* db = nullptr;
  ~SqliteHandle() {
    if (db != nullptr) sqlite3_close_v2(db);
  }
};

[[noreturn]] void sqlite_fail(sqlite3* db, const std::string& what) {
  throw std::runtime_error(what + ": " + (db != nullptr ? sqlite3_errmsg(db) : "out of memory"));
}

}  // namespace

std::uint64_t backup_database(const fs::path& src, const fs::path& dest, bool overwrite) {
  std::error_code ec;
  if (!fs::is_regular_file(src, ec)) throw std::runtime_error("database not found: " + src.string());
  if (fs::exists(dest, ec) && !overwrite)
    throw std::runtime_error("backup target exists (use --force to overwrite): " + dest.string());
  if (fs::weakly_canonical(src, ec) == fs::weakly_canonical(dest, ec))
    throw std::runtime_error("backup target is the database itself");
  if (auto parent = dest.parent_path(); !parent.empty()) fs::create_directories(parent, ec);

  const fs::path tmp = dest.string() + ".tmp-" + crypto::hex_encode(crypto::random_bytes(4));
  struct TmpGuard {
    fs::path p;
    bool keep = false;
    ~TmpGuard() {
      if (!keep) {
        std::error_code e;
        fs::remove(p, e);
      }
    }
  } guard{tmp};

  {
    SqliteHandle s;
    if (sqlite3_open_v2(src.c_str(), &s.db, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK)
      sqlite_fail(s.db, "cannot open database");
    sqlite3_busy_timeout(s.db, 5000);
    SqliteHandle d;
    if (sqlite3_open_v2(tmp.c_str(), &d.db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK)
      sqlite_fail(d.db, "cannot create backup file");
    sqlite3_backup* b = sqlite3_backup_init(d.db, "main", s.db, "main");
    if (b == nullptr) sqlite_fail(d.db, "cannot start backup");
    int rc = SQLITE_OK;
    // Small steps so concurrent writers of the live database are never blocked for long; a
    // write by another connection restarts the copy automatically (SQLite semantics).
    while (true) {
      rc = sqlite3_backup_step(b, 512);
      if (rc == SQLITE_DONE) break;
      if (rc == SQLITE_OK) continue;
      if (rc == SQLITE_BUSY || rc == SQLITE_LOCKED) {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        continue;
      }
      break;
    }
    const int frc = sqlite3_backup_finish(b);
    if (rc != SQLITE_DONE || frc != SQLITE_OK) sqlite_fail(d.db, "backup failed");

    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(d.db, "PRAGMA quick_check", -1, &st, nullptr) != SQLITE_OK)
      sqlite_fail(d.db, "cannot verify backup");
    std::string result;
    if (sqlite3_step(st) == SQLITE_ROW) {
      const auto* t = sqlite3_column_text(st, 0);
      result = t != nullptr ? reinterpret_cast<const char*>(t) : "";
    }
    sqlite3_finalize(st);
    if (result != "ok") throw std::runtime_error("backup verification failed: " + result);
    // A self-contained file: no -wal/-shm companions needed to restore it.
    sqlite3_exec(d.db, "PRAGMA journal_mode=DELETE", nullptr, nullptr, nullptr);
  }
  fs::rename(tmp, dest, ec);
  if (ec) throw std::runtime_error("cannot move backup into place: " + ec.message());
  guard.keep = true;
  return fs::file_size(dest, ec);
}

// ---- blobs-migrate ------------------------------------------------------------------------------

BlobMigrateStats migrate_blobs(db::Pool& pool, BlobStore& from, BlobStore& to, const BlobMigrateOptions& opts) {
  const std::string from_kind(from.kind());
  const std::string to_kind(to.kind());
  if (from_kind == to_kind) throw std::invalid_argument("source and destination stores are the same kind");
  BlobMigrateStats stats;
  std::string after;  // keyset pagination: failed rows are not revisited in this run
  const int batch = std::max(opts.batch, 1);
  for (;;) {
    struct Row {
      std::string sha;
      int64_t size;
    };
    auto rows = pool.read([&](db::Conn& c) {
      std::vector<Row> out;
      auto s = c.prepare("SELECT sha256, size FROM blobs WHERE storage=? AND sha256>? ORDER BY sha256 LIMIT ?");
      s.bind_all(from_kind, after, batch);
      while (s.step()) out.push_back({s.text(0), s.i64(1)});
      return out;
    });
    if (rows.empty()) break;
    for (const auto& row : rows) {
      after = row.sha;
      ++stats.candidates;
      stats.bytes += row.size;
      if (opts.dry_run) continue;
      std::string error;
      try {
        auto guard = blob_writer_guard();
        if (!to.exists(row.sha)) {
          const fs::path tmp = make_staging_path(to.tmp_dir());
          try {
            from.get_to_file(row.sha, tmp);
            const std::string actual = crypto::sha256_file_hex(tmp);
            if (actual != row.sha) throw BlobError("source object is corrupt (sha256 mismatch)");
            to.put_file(tmp, row.sha);  // removes tmp on success
          } catch (...) {
            std::error_code ec;
            fs::remove(tmp, ec);
            throw;
          }
          if (!to.exists(row.sha)) throw BlobError("object missing at the destination after upload");
        }
        const bool flipped = pool.write([&](db::Tx& tx) {
          tx.run("UPDATE blobs SET storage=? WHERE sha256=? AND storage=?", to_kind, row.sha, from_kind);
          return tx.changes() > 0;
        });
        ++stats.migrated;
        if (flipped) {
          try {
            from.remove(row.sha);
          } catch (const std::exception& e) {
            // Migrated (the row points at the destination); only the old copy is left behind.
            log::warn("blob migrated but the source copy could not be removed",
                      {{"sha256", row.sha}, {"error", e.what()}});
          }
        }
      } catch (const std::exception& e) {
        error = e.what();
        ++stats.failed;
        log::warn("blob migration failed", {{"sha256", row.sha}, {"error", error}});
      }
      if (opts.on_blob) opts.on_blob(row.sha, error.empty(), error);
    }
  }
  return stats;
}

}  // namespace azm::app
