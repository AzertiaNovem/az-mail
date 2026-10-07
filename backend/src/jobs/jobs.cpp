// Owner: WP-C
// WP0: enqueue / cancel / reschedule / extend_lease are implemented here because WP-B's
// queue_send / undo_send and WP-C's handlers depend on them (DESIGN §7 "jobs::enqueue (WP0
// header)"); everything else is a stub that WP-C implements (WP-C may refine these without
// changing their semantics).
#include "jobs/jobs.hpp"

#include "config.hpp"
#include "core/errors.hpp"
#include "core/time.hpp"
#include "jobs/kinds.hpp"
#include "services.hpp"

#include <boost/json/serialize.hpp>

#include <stdexcept>

namespace azm::jobs {

int64_t enqueue(db::Tx& tx, std::string_view kind, boost::json::object payload, EnqueueOpts opts) {
  const auto lane = lane_for(kind);
  if (!lane) throw std::invalid_argument("jobs::enqueue: unknown kind " + std::string(kind));
  const int64_t now = opts.now_ms > 0 ? opts.now_ms : azm::now_ms();
  // BEGIN IMMEDIATE makes this check-then-insert race-free (single writer).
  if (opts.dedupe_key) {
    if (auto existing = tx.scalar<int64_t>(
            "SELECT id FROM jobs WHERE dedupe_key=? AND state IN ('pending','running')",
            *opts.dedupe_key))
      return *existing;
  }
  tx.run(
      "INSERT INTO jobs(kind,lane,priority,payload,state,run_at,attempts,max_attempts,dedupe_key,"
      "created_at,updated_at) VALUES(?,?,?,?,'pending',?,0,?,?,?,?)",
      kind, *lane, opts.priority, boost::json::serialize(payload),
      opts.run_at_ms > 0 ? opts.run_at_ms : now, opts.max_attempts, opts.dedupe_key, now, now);
  const int64_t id = tx.last_insert_id();
  tx.wake_jobs();
  return id;
}

bool cancel(db::Tx& tx, int64_t job_id) {
  tx.run("UPDATE jobs SET state='canceled', updated_at=? WHERE id=? AND state='pending'",
         azm::now_ms(), job_id);
  return tx.changes() > 0;
}

bool reschedule(db::Tx& tx, int64_t job_id, int64_t run_at_ms) {
  tx.run("UPDATE jobs SET run_at=?, updated_at=? WHERE id=? AND state='pending'", run_at_ms,
         azm::now_ms(), job_id);
  if (tx.changes() == 0) return false;
  tx.wake_jobs();
  return true;
}

bool retry_dead(db::Tx&, int64_t, int64_t) { throw NotImplemented("jobs::retry_dead"); }
int recover_expired_leases(db::Tx&, int64_t) {
  throw NotImplemented("jobs::recover_expired_leases");
}
int purge_finished(db::Tx&, int64_t) { throw NotImplemented("jobs::purge_finished"); }

bool extend_lease(db::Tx& tx, int64_t job_id, int attempts, int64_t locked_until_ms) {
  // Conditional on the caller's claim (attempts) so a stale worker cannot extend a re-claimed job.
  tx.run(
      "UPDATE jobs SET locked_until=MAX(COALESCE(locked_until,0),?) "
      "WHERE id=? AND state='running' AND attempts=?",
      locked_until_ms, job_id, attempts);
  return tx.changes() > 0;
}
std::chrono::milliseconds default_backoff(int) { throw NotImplemented("jobs::default_backoff"); }

RunnerConfig runner_config_from(const Config&) { throw NotImplemented("jobs::runner_config_from"); }

struct Runner::Impl {
  Services* svc = nullptr;
};

Runner::Runner(db::Pool&, Services& svc, RunnerConfig) : impl_(std::make_unique<Impl>()) {
  impl_->svc = &svc;
}
Runner::~Runner() = default;

void Runner::on(std::string, std::string, JobFn, std::optional<std::chrono::seconds>) {
  throw NotImplemented("jobs::Runner::on");
}
void Runner::start() { throw NotImplemented("jobs::Runner::start"); }
void Runner::stop() {}
void Runner::wake() {}
bool Runner::run_one(std::string_view) { throw NotImplemented("jobs::Runner::run_one"); }
Services& Runner::services() const { return *impl_->svc; }

}  // namespace azm::jobs
