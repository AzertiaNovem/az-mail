// Owner: WP-A
//
// Operational helpers shared by app::App and the CLI (and unit-tested directly):
// tmp-dir sweeping, online database backup and blob migration between storage backends.
#pragma once

#include "config.hpp"
#include "core/blob_store.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

namespace azm::db {
class Pool;
}

namespace azm::app {

// True when R2 is the blob backend, or when complete R2 credentials are present (secondary
// store for a mixed installation, Addendum A).
bool r2_configured(const Config& cfg);

// Deletes regular files directly in `dir` whose mtime is older than `older_than` (crash
// leftovers of staging uploads / downloads). Missing directory → 0. Never throws.
std::size_t sweep_tmp_dir(const std::filesystem::path& dir, std::chrono::hours older_than);

// Online backup with the SQLite backup API (DESIGN F3): copies `src` (a live WAL database is
// fine) page by page into a temporary file next to `dest`, runs PRAGMA quick_check on the copy
// and renames it into place. Throws std::runtime_error when `dest` exists and !overwrite, or on
// any SQLite/IO failure (the temporary file is removed). Returns the backup's size in bytes.
std::uint64_t backup_database(const std::filesystem::path& src, const std::filesystem::path& dest,
                              bool overwrite);

// `azmail blobs-migrate`: moves every blob whose blobs.storage == from.kind() to `to`, per
// blob: copy (from.get_to_file into to.tmp_dir()) → verify sha256 → to.put_file → confirm
// to.exists → UPDATE blobs SET storage = to.kind() (only if still from.kind()) → from.remove.
// Each blob runs under blob_writer_guard(). Failures are counted and logged (the row keeps its
// storage, so a re-run retries); dry_run only counts. Readers resolve stores by blobs.storage,
// so running it against a live server is safe; at worst a concurrent GC leaks an object.
struct BlobMigrateOptions {
  bool dry_run = false;
  int batch = 200;
  // Called after each blob with (sha256, ok, error message or "").
  std::function<void(std::string_view sha, bool ok, std::string_view error)> on_blob;
};
struct BlobMigrateStats {
  std::int64_t candidates = 0;  // rows found in the source storage
  std::int64_t migrated = 0;
  std::int64_t failed = 0;
  std::int64_t bytes = 0;  // sum of blobs.size over candidates
};
BlobMigrateStats migrate_blobs(db::Pool& pool, BlobStore& from, BlobStore& to,
                               const BlobMigrateOptions& opts = {});

}  // namespace azm::app
