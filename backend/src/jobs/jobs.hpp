// Owner: WP-C
//
// Durable job queue over the `jobs` table (DESIGN §3, A6).
//  * enqueue/cancel/reschedule are plain SQL inside the caller's write transaction; enqueue
//    calls tx.wake_jobs() so the Runner wakes right after COMMIT (undo-send precision ~100 ms).
//  * Runner: one thread group per lane (outbound, inbound, sync, maintenance); each worker
//    claims the best ready job of its lane (state='pending' AND run_at<=now, ORDER BY priority
//    DESC, run_at, id) by setting state='running', attempts=attempts+1, locked_until=now+lease
//    in a short write transaction, then runs the handler OUTSIDE any transaction. "now" is
//    always services().clock.now_ms() (ManualClock in tests), for claiming and for outcomes.
//  * Each handler call runs under resend::ScopedStopToken(stop_token) (resend/rate_limiter.hpp)
//    so Resend calls blocked in the rate limiter return as soon as stop() is requested.
//  * Outcomes: return → 'done'. Retry → 'pending' at now + (delay, or default_backoff(attempts)
//    when delay is 0); with count_attempt=false the attempt is given back (429s, B6). When
//    attempts reach max_attempts → 'dead' (last_error kept). Permanent → 'dead' immediately.
//    Any other exception → treated as Retry with backoff (what() stored in last_error, without
//    payload contents). Outcomes are written conditionally (WHERE id=? AND state='running' AND
//    attempts=<claimed attempts>): a stale worker whose lease expired and whose job was
//    re-claimed changes nothing.
//  * Leases: running jobs whose locked_until passed go back to 'pending' at start() and every
//    RunnerConfig::recover_every (crash recovery). Handlers with long transfers extend their
//    lease with extend_lease(); additionally (RT-8) the Runner renews the lease of every job its
//    workers are executing each RunnerConfig::heartbeat_every (for at most max_heartbeat), so a
//    live handler keeps its claim however long a transfer takes, and only a dead/stalled process
//    lets it expire. A job given up by recovery (lease expired on an attempt beyond
//    max_attempts) runs its kind's on_abandoned hook in the same transaction.
//  * Periodic kinds (Runner::on with `periodic`): start() seeds one pending job (dedupe
//    "periodic:<kind>") if none exists; whenever a periodic job ENDS — done or dead (Permanent,
//    or attempts exhausted) — its successor (now + interval) is enqueued inside the SAME
//    transaction that finalizes it (avoids the partial-unique-index conflict), so a failing
//    periodic job never stops polling / reconcile / GC until a restart.
//  * Done/canceled jobs are purged after cfg.jobs_done_retention_days (gc.housekeeping).
#pragma once

#include "db/sqlite.hpp"

#include <boost/json/object.hpp>

#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

namespace azm {
struct Config;
struct Services;
struct Clock;
}  // namespace azm

namespace azm::jobs {

struct EnqueueOpts {
  int64_t run_at_ms = 0;                  // 0 = now
  std::optional<std::string> dedupe_key;  // unique among pending+running jobs
  int max_attempts = 8;
  int priority = 0;                       // kinds.hpp kPriority*
  // "now" for created_at/updated_at and the default run_at; 0 → azm::now_ms(). Callers that
  // have a Clock (jobs, handlers, mail functions with a now_ms parameter) pass it so jobs
  // enqueued under a ManualClock become ready under that clock.
  int64_t now_ms = 0;
};

// Inserts a pending job; lane = kinds::lane_for(kind). When `dedupe_key` matches a pending or
// running job, nothing is inserted and that job's id is returned. Calls tx.wake_jobs().
// Throws std::invalid_argument for an unknown kind.
int64_t enqueue(db::Tx&, std::string_view kind, boost::json::object payload, EnqueueOpts = {});

// pending → canceled. False when the job is not pending (running/done/dead/missing).
// `now_ms` (additive): updated_at; 0 → azm::now_ms(). Callers with a Clock pass it (like
// EnqueueOpts::now_ms), so jobs touched under a ManualClock stay consistent with it.
bool cancel(db::Tx&, int64_t job_id, int64_t now_ms = 0);

// Moves a pending job's run_at (and wakes the runner). False when not pending.
// `now_ms` (additive): updated_at; 0 → azm::now_ms().
bool reschedule(db::Tx&, int64_t job_id, int64_t run_at_ms, int64_t now_ms = 0);

// ---- additive helpers ---------------------------------------------------------------------
// dead | canceled → pending with attempts=0, run_at=now, last_error kept (admin "retry").
// False when the job is in another state or missing.
bool retry_dead(db::Tx&, int64_t job_id, int64_t now_ms);
// running with locked_until < now → pending. Returns the number of recovered jobs.
int recover_expired_leases(db::Tx&, int64_t now_ms);
// Deletes done/canceled jobs with updated_at < older_than_ms. Returns rows deleted.
int purge_finished(db::Tx&, int64_t older_than_ms);
// Lease renewal for long-running handlers (e.g. inbound.fetch before each large download):
// locked_until = max(locked_until, `locked_until_ms`) WHERE id=? AND state='running' AND
// attempts=`attempts` (Job::attempts of the caller's claim). False when the job is no longer running under that
// claim (lease expired and recovered/re-claimed): the caller must stop without side effects.
// Implemented in WP0 (plain SQL).
bool extend_lease(db::Tx&, int64_t job_id, int attempts, int64_t locked_until_ms);
// Default retry delay after `attempts` attempts: 5 s, 30 s, 2 min, 10 min, 30 min, 1 h, then 2 h.
std::chrono::milliseconds default_backoff(int attempts);

// Thrown by a handler: try again later.
struct Retry : std::exception {
  std::chrono::milliseconds delay{0};  // 0 = default_backoff(attempts)
  std::string reason;                  // stored in jobs.last_error (no secrets / mail content)
  bool count_attempt = true;           // false: does not consume an attempt (429, B6)

  Retry() = default;
  explicit Retry(std::chrono::milliseconds delay_, std::string reason_ = {},
                 bool count_attempt_ = true)
      : delay(delay_), reason(std::move(reason_)), count_attempt(count_attempt_) {}
  const char* what() const noexcept override { return reason.empty() ? "retry" : reason.c_str(); }
};

// Thrown by a handler: give up now (job → dead with `reason`).
struct Permanent : std::exception {
  std::string reason;

  Permanent() = default;
  explicit Permanent(std::string reason_) : reason(std::move(reason_)) {}
  const char* what() const noexcept override {
    return reason.empty() ? "permanent" : reason.c_str();
  }
};

struct Job {
  int64_t id = 0;
  std::string kind;
  boost::json::object payload;
  int attempts = 0;      // including the current attempt (≥ 1 while running)
  int max_attempts = 8;
  std::string lane;      // additive
  int priority = 0;      // additive
};

using JobFn = std::function<void(Services&, const Job&, std::stop_token)>;
// Additive (RT-8): runs inside lease recovery's write transaction for a job of the hook's kind
// that recovery just set 'dead' ("lease expired repeatedly") — the handler never saw that
// attempt fail, so its last-attempt bookkeeping (e.g. mark_inbound_failed) happens here.
// `job.attempts` is the attempt that expired. Throwing rolls the whole recovery back.
using AbandonFn = std::function<void(db::Tx&, const Job&, int64_t now_ms)>;

struct RunnerConfig {
  // Worker threads per lane; lanes with 0 threads are not run (e.g. tests driving run_one()).
  std::map<std::string, int, std::less<>> lane_threads{
      {"outbound", 2}, {"inbound", 2}, {"sync", 1}, {"maintenance", 1}};
  std::chrono::milliseconds lease{std::chrono::minutes(5)};    // locked_until = now + lease
  std::chrono::milliseconds idle_poll{std::chrono::seconds(1)};  // max sleep without a wake()
  std::chrono::milliseconds recover_every{std::chrono::minutes(1)};
  std::chrono::milliseconds stop_grace{std::chrono::seconds(25)};  // stop() waits this long for handlers
  // Additive (RT-8): lease renewal of running handlers (locked_until = now + lease), at most for
  // max_heartbeat after the claim; 0 disables. Default lease/4.
  std::chrono::milliseconds heartbeat_every{std::chrono::seconds(75)};
  std::chrono::milliseconds max_heartbeat{std::chrono::hours(2)};
};

// From cfg.jobs_*_threads and cfg.shutdown_grace_sec; lease 2 min with a heartbeat every 30 s
// (additive, RT-7/RT-8: live handlers keep their claim, a crashed one is retried soon).
RunnerConfig runner_config_from(const Config& cfg);

class Runner {
 public:
  Runner(db::Pool&, Services&, RunnerConfig);
  ~Runner();  // calls stop()
  Runner(const Runner&) = delete;
  Runner& operator=(const Runner&) = delete;

  // Registers the handler for `kind` on `lane` (before start()). `periodic` makes the kind
  // self-rescheduling with that interval. Throws std::invalid_argument for a duplicate kind or
  // a lane that differs from kinds::lane_for(kind).
  void on(std::string kind, std::string lane, JobFn,
          std::optional<std::chrono::seconds> periodic = {});
  // Additive (RT-8): hook for jobs of `kind` given up by lease recovery (before start()).
  // Throws std::invalid_argument for an unknown or unregistered kind or a second hook.
  void on_abandoned(std::string kind, AbandonFn);
  // Recovers expired leases, seeds periodic jobs and starts the lane threads.
  void start();
  // Requests stop (handlers see their stop_token), waits up to stop_grace, then joins.
  // Jobs still running keep their lease and are recovered at the next start. Idempotent.
  void stop();
  // Additive (RT-7): the first half of stop() without waiting — handlers see their stop_token,
  // idle workers exit, no new job is claimed. Idempotent; stop() must still be called.
  void request_stop();
  // Additive (RT-7): waits up to `timeout` for every worker thread to exit (true = all exited).
  bool wait_idle(std::chrono::milliseconds timeout);
  // Additive (RT-8): one lease-recovery pass now (with on_abandoned hooks); the number of jobs
  // recovered. Used by gc.housekeeping instead of the bare recover_expired_leases.
  int recover_expired();
  // Wakes idle workers (Pool TxHooks::wake_jobs). Thread-safe, cheap, callable anytime.
  void wake();

  // Test hook: claims and runs at most one ready job of `lane` on the calling thread (same
  // outcome handling as the workers). False when no job was ready. Usable without start().
  bool run_one(std::string_view lane);

  Services& services() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace azm::jobs
