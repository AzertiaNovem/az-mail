// Owner: WP-C
// Durable job queue (DESIGN §3, A6). enqueue / cancel / reschedule / extend_lease are the WP0
// implementations (unchanged semantics, WP-B depends on them); the Runner, recovery, retry and
// purge helpers are WP-C's.
#include "jobs/jobs.hpp"

#include "config.hpp"
#include "core/errors.hpp"
#include "core/log.hpp"
#include "core/strings.hpp"
#include "core/time.hpp"
#include "jobs/kinds.hpp"
#include "resend/rate_limiter.hpp"
#include "services.hpp"

#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

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

bool retry_dead(db::Tx& tx, int64_t job_id, int64_t now_ms) {
  auto s = tx.prepare("SELECT state, dedupe_key FROM jobs WHERE id=?");
  s.bind_all(job_id);
  if (!s.step()) return false;
  const std::string state = s.text(0);
  const std::optional<std::string> dedupe = s.opt_text(1);
  s.reset();
  if (state != "dead" && state != "canceled") return false;
  // The partial UNIQUE index forbids a second active job with the same dedupe key (e.g. the
  // successor of a dead periodic job): nothing to retry then.
  if (dedupe && tx.scalar<int64_t>("SELECT id FROM jobs WHERE dedupe_key=? AND state IN ('pending','running') "
                                   "AND id<>?",
                                   *dedupe, job_id))
    return false;
  tx.run(
      "UPDATE jobs SET state='pending', attempts=0, run_at=?, locked_until=NULL, updated_at=? "
      "WHERE id=? AND state IN ('dead','canceled')",
      now_ms, now_ms, job_id);
  if (tx.changes() == 0) return false;
  tx.wake_jobs();
  return true;
}

int recover_expired_leases(db::Tx& tx, int64_t now_ms) {
  // A job whose lease expired during an attempt beyond max_attempts (it crashed or stalled the
  // worker every time) is given up instead of looping forever.
  tx.run(
      "UPDATE jobs SET "
      "state=CASE WHEN attempts>max_attempts THEN 'dead' ELSE 'pending' END, "
      "last_error=CASE WHEN attempts>max_attempts THEN 'lease expired repeatedly' "
      "ELSE 'lease expired (worker stopped or stalled)' END, "
      "locked_until=NULL, updated_at=? "
      "WHERE state='running' AND locked_until IS NOT NULL AND locked_until<?",
      now_ms, now_ms);
  const int n = tx.changes();
  if (n > 0) tx.wake_jobs();
  return n;
}

int purge_finished(db::Tx& tx, int64_t older_than_ms) {
  tx.run("DELETE FROM jobs WHERE state IN ('done','canceled') AND updated_at<?", older_than_ms);
  return tx.changes();
}

bool extend_lease(db::Tx& tx, int64_t job_id, int attempts, int64_t locked_until_ms) {
  // Conditional on the caller's claim (attempts) so a stale worker cannot extend a re-claimed job.
  tx.run(
      "UPDATE jobs SET locked_until=MAX(COALESCE(locked_until,0),?) "
      "WHERE id=? AND state='running' AND attempts=?",
      locked_until_ms, job_id, attempts);
  return tx.changes() > 0;
}

std::chrono::milliseconds default_backoff(int attempts) {
  using namespace std::chrono;
  static constexpr milliseconds kSteps[] = {seconds(5), seconds(30), minutes(2), minutes(10), minutes(30), hours(1)};
  if (attempts <= 0) return kSteps[0];
  if (attempts > static_cast<int>(std::size(kSteps))) return hours(2);
  return kSteps[attempts - 1];
}

RunnerConfig runner_config_from(const Config& cfg) {
  RunnerConfig rc;
  rc.lane_threads = {{std::string(lanes::kOutbound), cfg.jobs_outbound_threads},
                     {std::string(lanes::kInbound), cfg.jobs_inbound_threads},
                     {std::string(lanes::kSync), cfg.jobs_sync_threads},
                     {std::string(lanes::kMaintenance), cfg.jobs_maintenance_threads}};
  if (cfg.shutdown_grace_sec > 0) rc.stop_grace = std::chrono::seconds(cfg.shutdown_grace_sec);
  return rc;
}

// =============================================================================================
// Runner
// =============================================================================================

namespace {

// Periodic jobs give up quickly: their successor (now + interval) is the real retry.
constexpr int kPeriodicMaxAttempts = 3;
constexpr std::size_t kMaxErrorLen = 500;

std::string short_error(std::string_view s) {
  std::string out = utf8_truncate(s, kMaxErrorLen);
  for (char& c : out)
    if (c == '\n' || c == '\r') c = ' ';
  return out;
}

struct Outcome {
  enum class Kind { Done, Retry, Dead } kind = Kind::Done;
  std::chrono::milliseconds delay{0};
  bool count_attempt = true;
  std::string error;
};

}  // namespace

struct Runner::Impl {
  struct Handler {
    std::string lane;
    JobFn fn;
    std::optional<std::chrono::seconds> periodic;
  };

  Impl(db::Pool& p, Services& s, RunnerConfig c) : pool(p), svc(s), cfg(std::move(c)) {}

  db::Pool& pool;
  Services& svc;
  RunnerConfig cfg;
  std::map<std::string, Handler, std::less<>> handlers;

  std::mutex mu;  // wake generation, lifecycle, worker count
  std::condition_variable cv;
  std::condition_variable exit_cv;
  uint64_t wake_seq = 0;
  int active_workers = 0;
  bool started = false;
  bool stopped = false;
  std::stop_source stop_src;
  std::vector<std::thread> threads;
  std::mutex recover_mu;  // one recovery pass at a time
  std::chrono::steady_clock::time_point last_recover{};

  int64_t now() const { return svc.clock.now_ms(); }

  std::vector<std::string> kinds_of(std::string_view lane) const {
    std::vector<std::string> out;
    for (const auto& [k, h] : handlers)
      if (h.lane == lane) out.push_back(k);
    return out;
  }

  static std::string in_list(std::size_t n) {
    std::string s = "(";
    for (std::size_t i = 0; i < n; ++i) s += i ? ",?" : "?";
    return s + ")";
  }

  // Enqueues the next run of every periodic kind unless one is pending/running (dedupe).
  void ensure_periodic(db::Tx& tx, int64_t now_ms) {
    for (const auto& [kind, h] : handlers) {
      if (!h.periodic) continue;
      enqueue(tx, kind, {},
              {.run_at_ms = now_ms, .dedupe_key = dedupe_periodic(kind), .max_attempts = kPeriodicMaxAttempts,
               .priority = kPriorityLow, .now_ms = now_ms});
    }
  }

  void recover(bool force) {
    std::unique_lock lk(recover_mu, std::try_to_lock);
    if (!lk.owns_lock()) return;
    const auto steady = std::chrono::steady_clock::now();
    if (!force && steady - last_recover < cfg.recover_every) return;
    last_recover = steady;
    try {
      const int64_t t = now();
      const int n = pool.write([&](db::Tx& tx) {
        const int r = recover_expired_leases(tx, t);
        ensure_periodic(tx, t);
        return r;
      });
      if (n > 0) log::warn("recovered jobs with expired leases", {{"count", n}});
    } catch (const std::exception& e) {
      log::error("job lease recovery failed", {{"error", e.what()}});
    }
  }

  std::optional<Job> claim(std::string_view lane) {
    const auto kinds = kinds_of(lane);
    if (kinds.empty()) return std::nullopt;
    const int64_t t = now();
    const int64_t lease_ms = std::max<int64_t>(1, cfg.lease.count());
    const std::string sql =
        "SELECT id, kind, payload, attempts, max_attempts, priority FROM jobs "
        "WHERE state='pending' AND lane=? AND run_at<=? AND kind IN " +
        in_list(kinds.size()) + " ORDER BY priority DESC, run_at, id LIMIT 1";
    return pool.write([&](db::Tx& tx) -> std::optional<Job> {
      Job j;
      {
        auto s = tx.prepare(sql);
        s.bind(1, std::string(lane));
        s.bind(2, t);
        for (std::size_t i = 0; i < kinds.size(); ++i) s.bind(static_cast<int>(i + 3), kinds[i]);
        if (!s.step()) return std::nullopt;
        j.id = s.i64(0);
        j.kind = s.text(1);
        boost::system::error_code ec;
        auto v = boost::json::parse(s.text(2), ec);
        if (!ec && v.is_object()) j.payload = std::move(v.as_object());
        j.attempts = static_cast<int>(s.i64(3));
        j.max_attempts = static_cast<int>(s.i64(4));
        j.priority = static_cast<int>(s.i64(5));
        j.lane = std::string(lane);
      }
      tx.run(
          "UPDATE jobs SET state='running', attempts=attempts+1, locked_until=?, updated_at=? "
          "WHERE id=? AND state='pending'",
          t + lease_ms, t, j.id);
      if (tx.changes() == 0) return std::nullopt;
      j.attempts += 1;
      return j;
    });
  }

  Outcome execute(const Handler& h, const Job& job, std::stop_token st) {
    resend::ScopedStopToken scoped(st);  // Resend calls blocked in the rate limiter end on stop
    Outcome out;
    try {
      h.fn(svc, job, st);
    } catch (const Retry& r) {
      out.kind = Outcome::Kind::Retry;
      out.delay = r.delay;
      out.count_attempt = r.count_attempt;
      out.error = short_error(r.reason.empty() ? "retry" : r.reason);
    } catch (const Permanent& p) {
      out.kind = Outcome::Kind::Dead;
      out.error = short_error(p.reason.empty() ? "permanent failure" : p.reason);
    } catch (const std::exception& e) {
      out.kind = Outcome::Kind::Retry;
      out.error = short_error(e.what());
      log::warn("job failed", {{"job_id", job.id}, {"kind", job.kind}, {"attempt", job.attempts},
                               {"error", out.error}});
    } catch (...) {
      out.kind = Outcome::Kind::Retry;
      out.error = "unknown exception";
    }
    return out;
  }

  void finalize(const Handler& h, const Job& job, const Outcome& o) {
    const int64_t t = now();
    Outcome::Kind kind = o.kind;
    if (kind == Outcome::Kind::Retry && o.count_attempt && job.attempts >= job.max_attempts)
      kind = Outcome::Kind::Dead;  // attempts exhausted
    pool.write([&](db::Tx& tx) {
      switch (kind) {
        case Outcome::Kind::Done:
          tx.run(
              "UPDATE jobs SET state='done', locked_until=NULL, updated_at=? "
              "WHERE id=? AND state='running' AND attempts=?",
              t, job.id, job.attempts);
          break;
        case Outcome::Kind::Retry: {
          const auto delay = o.delay.count() > 0 ? o.delay : default_backoff(job.attempts);
          tx.run(
              "UPDATE jobs SET state='pending', run_at=?, attempts=?, locked_until=NULL, last_error=?, "
              "updated_at=? WHERE id=? AND state='running' AND attempts=?",
              t + delay.count(), o.count_attempt ? job.attempts : job.attempts - 1, o.error, t, job.id,
              job.attempts);
          break;
        }
        case Outcome::Kind::Dead:
          tx.run(
              "UPDATE jobs SET state='dead', locked_until=NULL, last_error=?, updated_at=? "
              "WHERE id=? AND state='running' AND attempts=?",
              o.error, t, job.id, job.attempts);
          break;
      }
      if (tx.changes() == 0) {
        // Our lease expired and the job was recovered/re-claimed meanwhile: the other claim owns it.
        log::warn("stale job outcome ignored", {{"job_id", job.id}, {"kind", job.kind}});
        return;
      }
      if (kind != Outcome::Kind::Retry && h.periodic) {
        // Successor in the SAME transaction that ends this run (partial unique index, A6).
        enqueue(tx, job.kind, {},
                {.run_at_ms = t + std::chrono::duration_cast<std::chrono::milliseconds>(*h.periodic).count(),
                 .dedupe_key = dedupe_periodic(job.kind), .max_attempts = kPeriodicMaxAttempts,
                 .priority = kPriorityLow, .now_ms = t});
      }
    });
    if (kind == Outcome::Kind::Dead)
      log::warn("job dead", {{"job_id", job.id}, {"kind", job.kind}, {"attempts", job.attempts}, {"error", o.error}});
  }

  // Claims and runs one job of `lane`; false when none was ready.
  bool run_one(std::string_view lane, std::stop_token st) {
    auto job = claim(lane);
    if (!job) return false;
    const auto it = handlers.find(job->kind);
    if (it == handlers.end()) return true;  // unreachable: claim filters registered kinds
    const Outcome o = execute(it->second, *job, st);
    try {
      finalize(it->second, *job, o);
    } catch (const std::exception& e) {
      // The job stays 'running' until its lease expires and recovery re-queues it.
      log::error("cannot record job outcome", {{"job_id", job->id}, {"error", e.what()}});
    }
    return true;
  }

  std::chrono::milliseconds idle_wait(std::string_view lane) {
    auto wait = std::max(cfg.idle_poll, std::chrono::milliseconds(5));
    try {
      const auto kinds = kinds_of(lane);
      if (kinds.empty()) return wait;
      const std::string sql = "SELECT MIN(run_at) FROM jobs WHERE state='pending' AND lane=? AND kind IN " +
                              in_list(kinds.size());
      const auto next = pool.read([&](db::Conn& c) -> std::optional<int64_t> {
        auto s = c.prepare(sql);
        s.bind(1, std::string(lane));
        for (std::size_t i = 0; i < kinds.size(); ++i) s.bind(static_cast<int>(i + 2), kinds[i]);
        if (!s.step() || s.is_null(0)) return std::nullopt;
        return s.i64(0);
      });
      if (next) {
        const int64_t until = *next - now();
        // Even a "due" job keeps a small floor: another worker may own it and a ManualClock never moves.
        const auto floor = std::chrono::milliseconds(5);
        wait = std::clamp(std::chrono::milliseconds(until), floor, std::max(floor, cfg.idle_poll));
      }
    } catch (const std::exception& e) {
      log::error("job queue poll failed", {{"error", e.what()}});
    }
    return wait;
  }

  void worker(std::string lane, std::stop_token st) {
    while (!st.stop_requested()) {
      uint64_t seen;
      {
        std::lock_guard lk(mu);
        seen = wake_seq;
      }
      recover(false);
      bool ran = false;
      try {
        ran = run_one(lane, st);
      } catch (const std::exception& e) {
        log::error("job claim failed", {{"lane", lane}, {"error", e.what()}});
      }
      if (ran) continue;
      const auto wait = idle_wait(lane);
      std::unique_lock lk(mu);
      cv.wait_for(lk, wait, [&] { return st.stop_requested() || wake_seq != seen; });
    }
  }
};

Runner::Runner(db::Pool& pool, Services& svc, RunnerConfig cfg)
    : impl_(std::make_unique<Impl>(pool, svc, std::move(cfg))) {}

Runner::~Runner() { stop(); }

void Runner::on(std::string kind, std::string lane, JobFn fn, std::optional<std::chrono::seconds> periodic) {
  const auto expected = lane_for(kind);
  if (!expected) throw std::invalid_argument("Runner::on: unknown job kind " + kind);
  if (*expected != lane)
    throw std::invalid_argument("Runner::on: kind " + kind + " belongs to lane " + std::string(*expected));
  if (!fn) throw std::invalid_argument("Runner::on: empty handler for " + kind);
  if (periodic && periodic->count() <= 0) throw std::invalid_argument("Runner::on: periodic interval must be > 0");
  std::lock_guard lk(impl_->mu);
  if (impl_->started) throw std::logic_error("Runner::on: register handlers before start()");
  if (impl_->handlers.count(kind)) throw std::invalid_argument("Runner::on: duplicate job kind " + kind);
  impl_->handlers.emplace(std::move(kind), Impl::Handler{std::move(lane), std::move(fn), periodic});
}

void Runner::start() {
  Impl& m = *impl_;
  {
    std::lock_guard lk(m.mu);
    if (m.started || m.stopped) return;
    m.started = true;
  }
  m.recover(true);  // crash recovery + periodic seeds before any worker runs
  std::lock_guard lk(m.mu);
  for (const auto& [lane, n] : m.cfg.lane_threads) {
    if (n <= 0 || m.kinds_of(lane).empty()) continue;
    for (int i = 0; i < n; ++i) {
      ++m.active_workers;
      m.threads.emplace_back([&m, lane = lane, st = m.stop_src.get_token()] {
        try {
          m.worker(lane, st);
        } catch (const std::exception& e) {
          log::error("job worker crashed", {{"lane", lane}, {"error", e.what()}});
        }
        std::lock_guard g(m.mu);
        --m.active_workers;
        m.exit_cv.notify_all();
      });
    }
  }
  log::info("job runner started", {{"threads", static_cast<int64_t>(m.threads.size())}});
}

void Runner::stop() {
  Impl& m = *impl_;
  {
    std::lock_guard lk(m.mu);
    if (m.stopped) return;
    m.stopped = true;
  }
  m.stop_src.request_stop();
  {
    std::unique_lock lk(m.mu);
    m.cv.notify_all();
    const bool drained = m.exit_cv.wait_for(lk, m.cfg.stop_grace, [&] { return m.active_workers == 0; });
    if (!drained)
      log::warn("job handlers still running after the stop grace period; waiting for them",
                {{"running", m.active_workers}});
  }
  for (auto& t : m.threads)
    if (t.joinable()) t.join();
  m.threads.clear();
}

void Runner::wake() {
  {
    std::lock_guard lk(impl_->mu);
    ++impl_->wake_seq;
  }
  impl_->cv.notify_all();
}

bool Runner::run_one(std::string_view lane) { return impl_->run_one(lane, impl_->stop_src.get_token()); }

Services& Runner::services() const { return impl_->svc; }

}  // namespace azm::jobs
