// Owner: WP-C
// purge.trash, gc.blobs, gc.housekeeping, db.optimize (DESIGN A5, A6, Addendum A GC; CONTRACTS §C).
#include "core/blob_store.hpp"
#include "core/log.hpp"
#include "jobs/handlers.hpp"
#include "jobs/kinds.hpp"
#include "mail/attachments.hpp"
#include "mail/mailbox.hpp"
#include "repo/accounts.hpp"
#include "services.hpp"

#include <exception>
#include <set>
#include <system_error>

namespace azm::jobs {
namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

constexpr int kPurgeBatch = 500;
constexpr int kGcBatch = 500;
constexpr int kOrphanUploadBatch = 1000;
constexpr int kMaxPurgeRounds = 200;  // 100k messages per run at most; the rest next hour
constexpr auto kTmpFileMaxAge = 24h;
constexpr int64_t kHourMs = 3600LL * 1000;
constexpr int64_t kDayMs = 24 * kHourMs;

// Deletes regular files in `dir` whose mtime is older than `max_age`. Returns the count.
int sweep_tmp_dir(const fs::path& dir, std::chrono::hours max_age) {
  if (dir.empty()) return 0;
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) return 0;
  const auto cutoff = fs::file_time_type::clock::now() - max_age;
  int removed = 0;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code e2;
    if (!it->is_regular_file(e2)) continue;
    const auto mtime = it->last_write_time(e2);
    if (e2 || mtime >= cutoff) continue;
    if (fs::remove(it->path(), e2)) ++removed;
  }
  return removed;
}

}  // namespace

void run_purge_trash(Services& svc, const Job&, std::stop_token st) {
  const int64_t now = svc.now_ms();
  int64_t total = 0;
  for (int round = 0; round < kMaxPurgeRounds && !st.stop_requested(); ++round) {
    const auto r = svc.db.write([&](db::Tx& tx) {
      return mail::purge_trash(tx, now, svc.cfg.trash_purge_days, svc.cfg.spam_purge_days, kPurgeBatch);
    });
    total += r.messages_deleted;
    if (!r.more) break;
  }
  if (total > 0) log::info("purged trash/spam", {{"messages", total}});
}

void run_gc_blobs(Services& svc, const Job&, std::stop_token st) {
  const int64_t cutoff = svc.now_ms() - static_cast<int64_t>(svc.cfg.blob_gc_grace_hours) * kHourMs;
  const auto candidates =
      svc.db.read([&](db::Conn& c) { return mail::unreferenced_blobs(c, cutoff, kGcBatch); });
  int removed = 0, kept = 0;
  for (const auto& blob : candidates) {
    if (st.stop_requested()) break;
    // One exclusive guard per blob (never across the batch): writers wait for one DELETE at most.
    auto guard = blob_gc_guard();
    if (!svc.db.read([&](db::Conn& c) { return mail::is_blob_unreferenced(c, blob.sha256); })) continue;
    try {
      svc.blobs_for(blob.storage).remove(blob.sha256);  // outside any transaction
    } catch (const BlobError& e) {
      log::warn("blob removal failed; kept for the next run", {{"sha256", blob.sha256}, {"error", e.what()}});
      ++kept;
      continue;
    }
    if (svc.db.write([&](db::Tx& tx) { return mail::forget_blob_if_unreferenced(tx, blob.sha256); })) ++removed;
  }
  if (removed > 0 || kept > 0) log::info("blob gc", {{"removed", removed}, {"failed", kept}});
}

void run_gc_housekeeping(Services& svc, const Job&, std::stop_token) {
  const int64_t now = svc.now_ms();
  const Config& cfg = svc.cfg;
  std::exception_ptr first_error;
  // Independent steps: one failing (e.g. busy DB) does not skip the others.
  auto step = [&](std::string_view name, const std::function<int(db::Tx&)>& fn) {
    try {
      const int n = svc.db.write([&](db::Tx& tx) { return fn(tx); });
      if (n > 0) log::info("housekeeping", {{"step", name}, {"rows", n}});
    } catch (const std::exception& e) {
      log::error("housekeeping step failed", {{"step", name}, {"error", e.what()}});
      if (!first_error) first_error = std::current_exception();
    }
  };
  step("sessions", [&](db::Tx& tx) { return repo::purge_expired_sessions(tx, now); });
  step("jobs", [&](db::Tx& tx) {
    return purge_finished(tx, now - static_cast<int64_t>(cfg.jobs_done_retention_days) * kDayMs);
  });
  step("webhook_events", [&](db::Tx& tx) {
    tx.run("DELETE FROM webhook_events WHERE received_at<?",
           now - static_cast<int64_t>(cfg.webhook_events_retention_days) * kDayMs);
    return tx.changes();
  });
  step("orphan_uploads", [&](db::Tx& tx) {
    return mail::purge_orphan_uploads(tx, now - static_cast<int64_t>(cfg.unattached_upload_ttl_hours) * kHourMs,
                                      kOrphanUploadBatch);
  });
  step("leases", [&](db::Tx& tx) { return recover_expired_leases(tx, now); });

  // Staging leftovers of crashed uploads/downloads (outside any transaction).
  std::set<fs::path> dirs{svc.blobs.tmp_dir()};
  if (svc.secondary_blobs != nullptr) dirs.insert(svc.secondary_blobs->tmp_dir());
  int files = 0;
  for (const auto& d : dirs) files += sweep_tmp_dir(d, std::chrono::duration_cast<std::chrono::hours>(kTmpFileMaxAge));
  if (files > 0) log::info("housekeeping", {{"step", "tmp_files"}, {"rows", files}});
  if (first_error) std::rethrow_exception(first_error);
}

void run_db_optimize(Services& svc, const Job&, std::stop_token) {
  auto lease = svc.db.acquire();
  lease->exec("PRAGMA optimize");
  lease->exec("PRAGMA wal_checkpoint(TRUNCATE)");
}

void register_maintenance_jobs(Runner& runner) {
  const std::string lane(lanes::kMaintenance);
  runner.on(std::string(kinds::kPurgeTrash), lane, run_purge_trash, kPurgeTrashEvery);
  runner.on(std::string(kinds::kGcBlobs), lane, run_gc_blobs, kGcBlobsEvery);
  runner.on(std::string(kinds::kGcHousekeeping), lane, run_gc_housekeeping, kHousekeepingEvery);
  runner.on(std::string(kinds::kDbOptimize), lane, run_db_optimize, kDbOptimizeEvery);
}

}  // namespace azm::jobs
