// Owner: WP-C — jobs::Runner (lanes, leases, recovery, retries, periodic successors, wake-up,
// stop) and the queue helpers. Hermetic: TestServices (temp SQLite) + ManualClock.
#include "config.hpp"
#include "jobs/jobs.hpp"
#include "jobs/kinds.hpp"
#include "resend/rate_limiter.hpp"
#include "test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

using namespace azm;
using namespace azm::jobs;
using namespace std::chrono_literals;

namespace {

struct JobRow {
  std::string state;
  int attempts = 0;
  int64_t run_at = 0;
  std::optional<int64_t> locked_until;
  std::optional<std::string> last_error;
  std::optional<std::string> dedupe_key;
};

JobRow row(test::TestServices& ts, int64_t id) {
  return ts.db.read([&](db::Conn& c) {
    auto s = c.prepare("SELECT state, attempts, run_at, locked_until, last_error, dedupe_key FROM jobs WHERE id=?");
    s.bind_all(id);
    REQUIRE(s.step());
    return JobRow{s.text(0), static_cast<int>(s.i64(1)), s.i64(2), s.opt_i64(3), s.opt_text(4), s.opt_text(5)};
  });
}

int64_t add(test::TestServices& ts, std::string_view kind, boost::json::object payload = {}, EnqueueOpts o = {}) {
  if (o.now_ms == 0) o.now_ms = ts.clock.now_ms();
  return ts.db.write([&](db::Tx& tx) { return enqueue(tx, kind, std::move(payload), o); });
}

int64_t count(test::TestServices& ts, std::string_view where) {
  return ts.db.read([&](db::Conn& c) {
    return c.scalar<int64_t>("SELECT COUNT(*) FROM jobs WHERE " + std::string(where)).value_or(0);
  });
}

RunnerConfig no_threads() {
  RunnerConfig rc;
  rc.lane_threads = {};
  return rc;
}

const std::string kSend(kinds::kOutboundSend);
const std::string kMeta(kinds::kOutboundFetchMeta);
const std::string kPoll(kinds::kPollReceiving);
const std::string kFetch(kinds::kInboundFetch);
const std::string kGc(kinds::kGcBlobs);

}  // namespace

TEST_CASE("jobs: default backoff schedule", "[jobs]") {
  CHECK(default_backoff(0) == 5s);
  CHECK(default_backoff(1) == 5s);
  CHECK(default_backoff(2) == 30s);
  CHECK(default_backoff(3) == 2min);
  CHECK(default_backoff(4) == 10min);
  CHECK(default_backoff(5) == 30min);
  CHECK(default_backoff(6) == 1h);
  CHECK(default_backoff(7) == 2h);
  CHECK(default_backoff(100) == 2h);
}

TEST_CASE("jobs: runner config from Config", "[jobs]") {
  Config cfg;
  cfg.jobs_outbound_threads = 3;
  cfg.jobs_inbound_threads = 4;
  cfg.jobs_sync_threads = 0;
  cfg.jobs_maintenance_threads = 1;
  cfg.shutdown_grace_sec = 7;
  const auto rc = runner_config_from(cfg);
  CHECK(rc.lane_threads.at("outbound") == 3);
  CHECK(rc.lane_threads.at("inbound") == 4);
  CHECK(rc.lane_threads.at("sync") == 0);
  CHECK(rc.lane_threads.at("maintenance") == 1);
  CHECK(rc.stop_grace == 7s);
}

TEST_CASE("jobs: registration is validated", "[jobs]") {
  test::TestServices ts;
  Runner r(ts.db, ts.svc, no_threads());
  auto noop = [](Services&, const Job&, std::stop_token) {};
  CHECK_THROWS_AS(r.on("no.such.kind", "sync", noop), std::invalid_argument);
  CHECK_THROWS_AS(r.on(kSend, "sync", noop), std::invalid_argument);  // wrong lane
  CHECK_THROWS_AS(r.on(kPoll, "sync", noop, 0s), std::invalid_argument);
  CHECK_THROWS_AS(r.on(kSend, "outbound", JobFn{}), std::invalid_argument);
  r.on(kSend, "outbound", noop);
  CHECK_THROWS_AS(r.on(kSend, "outbound", noop), std::invalid_argument);  // duplicate
  CHECK(&r.services() == &ts.svc);
  r.start();
  CHECK_THROWS_AS(r.on(kMeta, "sync", noop), std::logic_error);  // after start
  r.stop();
  r.stop();  // idempotent
}

TEST_CASE("jobs: run_one claims by priority then run_at, handler sees the claim", "[jobs]") {
  test::TestServices ts;
  Runner r(ts.db, ts.svc, no_threads());
  std::vector<int64_t> order;
  bool token_installed = false;
  r.on(kSend, "outbound", [&](Services& svc, const Job& job, std::stop_token st) {
    CHECK(&svc == &ts.svc);
    CHECK(job.attempts == 1);
    CHECK(job.lane == "outbound");
    CHECK(job.kind == kSend);
    CHECK_FALSE(st.stop_requested());
    token_installed = resend::current_stop_token().stop_possible();
    order.push_back(job.payload.at("outbound_id").as_int64());
  });
  const int64_t now = ts.clock.now_ms();
  add(ts, kSend, {{"outbound_id", 1}}, {.priority = 0});
  add(ts, kSend, {{"outbound_id", 2}}, {.run_at_ms = now - 1000, .priority = 0});
  const int64_t high = add(ts, kSend, {{"outbound_id", 3}}, {.priority = kPriorityHigh});
  const int64_t future = add(ts, kSend, {{"outbound_id", 4}}, {.run_at_ms = now + 60'000, .priority = kPriorityHigh});
  add(ts, kMeta, {{"outbound_id", 5}});  // no handler registered for this kind: never claimed

  while (r.run_one("outbound")) {
  }
  CHECK(order == std::vector<int64_t>{3, 2, 1});
  CHECK(token_installed);
  CHECK(row(ts, high).state == "done");
  CHECK_FALSE(row(ts, high).locked_until.has_value());
  CHECK(row(ts, future).state == "pending");
  CHECK_FALSE(r.run_one("sync"));
  CHECK_FALSE(r.run_one("no-such-lane"));

  ts.clock.advance(60'000);
  CHECK(r.run_one("outbound"));
  CHECK(order.back() == 4);
  CHECK_FALSE(resend::current_stop_token().stop_possible());  // restored after the handler
}

TEST_CASE("jobs: Retry, Permanent, exceptions and attempt accounting", "[jobs]") {
  test::TestServices ts;
  Runner r(ts.db, ts.svc, no_threads());
  std::function<void(const Job&)> behaviour;
  r.on(kFetch, "inbound", [&](Services&, const Job& job, std::stop_token) { behaviour(job); });
  const int64_t t0 = ts.clock.now_ms();

  SECTION("Retry with an explicit delay") {
    const int64_t id = add(ts, kFetch, {}, {.max_attempts = 3});
    behaviour = [](const Job&) { throw Retry(10s, "try later"); };
    REQUIRE(r.run_one("inbound"));
    auto j = row(ts, id);
    CHECK(j.state == "pending");
    CHECK(j.attempts == 1);
    CHECK(j.run_at == t0 + 10'000);
    CHECK(j.last_error == std::optional<std::string>("try later"));
    CHECK_FALSE(j.locked_until.has_value());
  }
  SECTION("Retry{} uses default_backoff; attempts exhausted → dead") {
    const int64_t id = add(ts, kFetch, {}, {.max_attempts = 2});
    behaviour = [](const Job&) { throw Retry(); };
    REQUIRE(r.run_one("inbound"));
    CHECK(row(ts, id).run_at == t0 + 5'000);
    CHECK_FALSE(r.run_one("inbound"));  // not due yet
    ts.clock.advance(5'000);
    REQUIRE(r.run_one("inbound"));
    const auto j = row(ts, id);
    CHECK(j.state == "dead");
    CHECK(j.attempts == 2);
    CHECK(j.last_error == std::optional<std::string>("retry"));
  }
  SECTION("429-style retries do not consume attempts") {
    const int64_t id = add(ts, kFetch, {}, {.max_attempts = 1});
    int calls = 0;
    behaviour = [&](const Job& job) {
      CHECK(job.attempts == 1);  // the attempt is given back every time
      if (++calls < 6) throw Retry(1s, "rate_limited", false);
    };
    for (int i = 0; i < 6; ++i) {
      REQUIRE(r.run_one("inbound"));
      ts.clock.advance(1'000);
    }
    CHECK(calls == 6);
    const auto j = row(ts, id);
    CHECK(j.state == "done");
    CHECK(j.attempts == 1);
  }
  SECTION("Permanent → dead immediately") {
    const int64_t id = add(ts, kFetch, {}, {.max_attempts = 8});
    behaviour = [](const Job&) { throw Permanent("bad payload"); };
    REQUIRE(r.run_one("inbound"));
    const auto j = row(ts, id);
    CHECK(j.state == "dead");
    CHECK(j.attempts == 1);
    CHECK(j.last_error == std::optional<std::string>("bad payload"));
  }
  SECTION("unexpected exceptions retry with backoff, message kept (truncated)") {
    const int64_t id = add(ts, kFetch);
    behaviour = [](const Job&) { throw std::runtime_error(std::string(2000, 'x') + "\nsecond line"); };
    REQUIRE(r.run_one("inbound"));
    const auto j = row(ts, id);
    CHECK(j.state == "pending");
    CHECK(j.run_at == t0 + 5'000);
    REQUIRE(j.last_error.has_value());
    CHECK(j.last_error->size() == 500);
  }
  SECTION("malformed payload JSON reaches the handler as {}") {
    const int64_t id = add(ts, kFetch);
    ts.db.write([&](db::Tx& tx) { tx.run("UPDATE jobs SET payload='not json' WHERE id=?", id); });
    behaviour = [](const Job& job) { CHECK(job.payload.empty()); };
    REQUIRE(r.run_one("inbound"));
    CHECK(row(ts, id).state == "done");
  }
}

TEST_CASE("jobs: a stale worker's outcome is ignored after its claim was taken over", "[jobs]") {
  test::TestServices ts;
  Runner r(ts.db, ts.svc, no_threads());
  int64_t id = 0;
  r.on(kFetch, "inbound", [&](Services&, const Job& job, std::stop_token) {
    // Simulate: our lease expired, recovery re-queued the job and another worker claimed it.
    ts.db.write([&](db::Tx& tx) { tx.run("UPDATE jobs SET attempts=attempts+1 WHERE id=?", job.id); });
    CHECK_FALSE(ts.db.write([&](db::Tx& tx) { return extend_lease(tx, job.id, job.attempts, ts.clock.now_ms() + 1); }));
  });
  id = add(ts, kFetch);
  REQUIRE(r.run_one("inbound"));
  const auto j = row(ts, id);
  CHECK(j.state == "running");  // our "done" did not overwrite the other claim
  CHECK(j.attempts == 2);
}

TEST_CASE("jobs: periodic jobs are seeded and always get a successor", "[jobs]") {
  test::TestServices ts;
  Runner r(ts.db, ts.svc, no_threads());
  bool fail = false;
  int runs = 0;
  r.on(kGc, "maintenance",
       [&](Services&, const Job&, std::stop_token) {
         ++runs;
         if (fail) throw Permanent("gc broken");
       },
       std::chrono::seconds(3600));
  r.start();  // no threads: seeds only
  const int64_t now = ts.clock.now_ms();
  REQUIRE(count(ts, "kind='gc.blobs' AND state='pending'") == 1);
  const int64_t seeded = ts.db.read([&](db::Conn& c) { return *c.scalar<int64_t>("SELECT id FROM jobs"); });
  CHECK(row(ts, seeded).dedupe_key == std::optional<std::string>("periodic:gc.blobs"));
  CHECK(row(ts, seeded).run_at == now);

  REQUIRE(r.run_one("maintenance"));
  CHECK(row(ts, seeded).state == "done");
  REQUIRE(count(ts, "kind='gc.blobs' AND state='pending'") == 1);
  const int64_t next = ts.db.read([&](db::Conn& c) {
    return *c.scalar<int64_t>("SELECT id FROM jobs WHERE state='pending' AND kind='gc.blobs'");
  });
  CHECK(row(ts, next).run_at == now + 3'600'000);
  CHECK(row(ts, next).dedupe_key == std::optional<std::string>("periodic:gc.blobs"));
  CHECK_FALSE(r.run_one("maintenance"));  // successor not due

  // A failing (dead) periodic job still schedules its successor.
  fail = true;
  ts.clock.advance(3'600'000);
  REQUIRE(r.run_one("maintenance"));
  CHECK(row(ts, next).state == "dead");
  CHECK(count(ts, "kind='gc.blobs' AND state='pending'") == 1);
  CHECK(runs == 2);

  // Manual triggers share the kind: the periodic successor is deduplicated.
  ts.db.write([&](db::Tx& tx) {
    enqueue(tx, kinds::kGcBlobs, {}, {.dedupe_key = std::string("gc:manual"), .now_ms = ts.clock.now_ms()});
  });
  fail = false;
  REQUIRE(r.run_one("maintenance"));
  CHECK(count(ts, "kind='gc.blobs' AND state='pending'") == 1);
}

TEST_CASE("jobs: expired leases are recovered at start and by the helper", "[jobs]") {
  test::TestServices ts;
  const int64_t now = ts.clock.now_ms();
  const int64_t a = add(ts, kFetch);
  const int64_t b = add(ts, kFetch, {}, {.max_attempts = 2});
  const int64_t c = add(ts, kFetch);
  ts.db.write([&](db::Tx& tx) {
    tx.run("UPDATE jobs SET state='running', attempts=1, locked_until=? WHERE id=?", now - 1, a);
    tx.run("UPDATE jobs SET state='running', attempts=3, locked_until=? WHERE id=?", now - 1, b);  // > max
    tx.run("UPDATE jobs SET state='running', attempts=1, locked_until=? WHERE id=?", now + 60'000, c);
  });
  std::atomic<int> ran{0};
  {
    RunnerConfig rc;
    rc.lane_threads = {{"inbound", 1}};
    Runner r(ts.db, ts.svc, rc);
    r.on(kFetch, "inbound", [&](Services&, const Job& job, std::stop_token) {
      CHECK(job.attempts == 2);
      ++ran;
    });
    r.start();
    for (int i = 0; i < 200 && ran.load() == 0; ++i) std::this_thread::sleep_for(10ms);
    std::this_thread::sleep_for(50ms);
    r.stop();
  }
  CHECK(ran == 1);
  CHECK(row(ts, a).state == "done");
  CHECK(row(ts, b).state == "dead");
  CHECK(row(ts, b).last_error == std::optional<std::string>("lease expired repeatedly"));
  CHECK(row(ts, c).state == "running");  // lease still valid

  ts.clock.advance(120'000);
  CHECK(ts.db.write([&](db::Tx& tx) { return recover_expired_leases(tx, ts.clock.now_ms()); }) == 1);
  CHECK(row(ts, c).state == "pending");
  CHECK(row(ts, c).last_error.has_value());
}

TEST_CASE("jobs: enqueue wakes idle workers well within 200 ms", "[jobs][timing]") {
  test::TestServices ts;
  RunnerConfig rc;
  rc.lane_threads = {{"outbound", 1}};
  rc.idle_poll = 10s;  // only the wake-up can explain a fast pickup
  Runner r(ts.db, ts.svc, rc);
  std::mutex mu;
  std::condition_variable cv;
  std::optional<std::chrono::steady_clock::time_point> ran_at;
  r.on(kSend, "outbound", [&](Services&, const Job&, std::stop_token) {
    std::lock_guard lk(mu);
    ran_at = std::chrono::steady_clock::now();
    cv.notify_all();
  });
  ts.db.set_hooks(db::TxHooks{&ts.notifier, [&] { r.wake(); }});
  r.start();
  std::this_thread::sleep_for(100ms);  // let the worker go idle
  const auto t0 = std::chrono::steady_clock::now();
  add(ts, kSend, {{"outbound_id", 1}});
  {
    std::unique_lock lk(mu);
    cv.wait_for(lk, 3s, [&] { return ran_at.has_value(); });
  }
  r.stop();
  ts.db.set_hooks(db::TxHooks{&ts.notifier, {}});
  REQUIRE(ran_at.has_value());
  CHECK(*ran_at - t0 < 200ms);
}

TEST_CASE("jobs: stop() signals handlers, records their outcome and joins", "[jobs]") {
  test::TestServices ts;
  RunnerConfig rc;
  rc.lane_threads = {{"inbound", 2}};
  rc.stop_grace = 5s;
  std::atomic<int> started{0};
  int64_t id = 0;
  {
    Runner r(ts.db, ts.svc, rc);
    r.on(kFetch, "inbound", [&](Services&, const Job&, std::stop_token st) {
      ++started;
      while (!st.stop_requested()) std::this_thread::sleep_for(5ms);
      CHECK(resend::current_stop_token().stop_requested());
      throw Retry(1s, "shutdown", false);
    });
    id = add(ts, kFetch);
    r.start();
    for (int i = 0; i < 300 && started.load() == 0; ++i) std::this_thread::sleep_for(10ms);
    REQUIRE(started == 1);
    const auto t0 = std::chrono::steady_clock::now();
    r.stop();
    CHECK(std::chrono::steady_clock::now() - t0 < 2s);
    CHECK_FALSE(r.run_one("inbound"));  // not due (retry in 1 s)
  }  // destructor after stop(): no-op
  const auto j = row(ts, id);
  CHECK(j.state == "pending");
  CHECK(j.attempts == 0);
}

TEST_CASE("jobs: retry_dead and purge_finished", "[jobs]") {
  test::TestServices ts;
  const int64_t now = ts.clock.now_ms();
  const int64_t dead = add(ts, kFetch, {}, {.dedupe_key = std::string("in:a")});
  const int64_t done = add(ts, kFetch);
  const int64_t old_done = add(ts, kFetch);
  const int64_t canceled = add(ts, kFetch);
  const int64_t pending = add(ts, kFetch);
  ts.db.write([&](db::Tx& tx) {
    tx.run("UPDATE jobs SET state='dead', attempts=8, last_error='x' WHERE id=?", dead);
    tx.run("UPDATE jobs SET state='done', updated_at=? WHERE id=?", now, done);
    tx.run("UPDATE jobs SET state='done', updated_at=? WHERE id=?", now - 8 * 86'400'000LL, old_done);
    tx.run("UPDATE jobs SET state='canceled', updated_at=? WHERE id=?", now - 8 * 86'400'000LL, canceled);
  });
  CHECK_FALSE(ts.db.write([&](db::Tx& tx) { return retry_dead(tx, pending, now); }));
  CHECK_FALSE(ts.db.write([&](db::Tx& tx) { return retry_dead(tx, 999999, now); }));

  // Another active job holds the dedupe key → cannot retry.
  const int64_t holder = add(ts, kFetch, {}, {.dedupe_key = std::string("in:a")});
  CHECK(holder != dead);
  CHECK_FALSE(ts.db.write([&](db::Tx& tx) { return retry_dead(tx, dead, now); }));
  ts.db.write([&](db::Tx& tx) { tx.run("UPDATE jobs SET state='done' WHERE id=?", holder); });
  CHECK(ts.db.write([&](db::Tx& tx) { return retry_dead(tx, dead, now + 5); }));
  const auto j = row(ts, dead);
  CHECK(j.state == "pending");
  CHECK(j.attempts == 0);
  CHECK(j.run_at == now + 5);
  CHECK(j.last_error == std::optional<std::string>("x"));

  CHECK(ts.db.write([&](db::Tx& tx) { return purge_finished(tx, now - 7 * 86'400'000LL); }) == 2);
  CHECK(count(ts, "id=" + std::to_string(old_done)) == 0);
  CHECK(count(ts, "id=" + std::to_string(canceled)) == 0);
  CHECK(count(ts, "id=" + std::to_string(done)) == 1);
}

TEST_CASE("jobs: dedupe holds while running, frees once finished", "[jobs]") {
  test::TestServices ts;
  Runner r(ts.db, ts.svc, no_threads());
  int64_t dup_inside = 0;
  r.on(kFetch, "inbound", [&](Services&, const Job&, std::stop_token) {
    dup_inside = add(ts, kFetch, {}, {.dedupe_key = std::string("in:x")});
  });
  const int64_t first = add(ts, kFetch, {}, {.dedupe_key = std::string("in:x")});
  REQUIRE(r.run_one("inbound"));
  CHECK(dup_inside == first);  // the running job still owns the key
  const int64_t again = add(ts, kFetch, {}, {.dedupe_key = std::string("in:x")});
  CHECK(again != first);
}

TEST_CASE("jobs: many jobs across worker threads run exactly once", "[jobs]") {
  test::TestServices ts(azm::now_ms(), 8);
  RunnerConfig rc;
  rc.lane_threads = {{"inbound", 4}};
  rc.idle_poll = 50ms;
  Runner r(ts.db, ts.svc, rc);
  std::mutex mu;
  std::map<int64_t, int> seen;
  r.on(kFetch, "inbound", [&](Services&, const Job& job, std::stop_token) {
    std::lock_guard lk(mu);
    ++seen[job.id];
  });
  ts.db.set_hooks(db::TxHooks{&ts.notifier, [&] { r.wake(); }});
  r.start();
  for (int i = 0; i < 40; ++i) add(ts, kFetch, {{"i", i}});
  for (int i = 0; i < 300 && count(ts, "state='done'") < 40; ++i) std::this_thread::sleep_for(10ms);
  r.stop();
  ts.db.set_hooks(db::TxHooks{&ts.notifier, {}});
  CHECK(count(ts, "state='done'") == 40);
  CHECK(seen.size() == 40);
  for (const auto& [id, n] : seen) CHECK(n == 1);
}
