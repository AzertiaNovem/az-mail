// Owner: WP-C — priority token bucket (DESIGN B6).
#include "config.hpp"
#include "core/time.hpp"
#include "resend/rate_limiter.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace azm;
using namespace azm::resend;
using namespace std::chrono_literals;

TEST_CASE("rate limiter: bucket starts full and refills at rps", "[rate_limiter]") {
  ManualClock clock(1'000'000);
  RateLimiter rl({.rps = 8, .burst = 8, .low_reserve = 2}, clock);
  CHECK(rl.available() == 8.0);
  for (int i = 0; i < 8; ++i) CHECK(rl.try_acquire(Priority::High));
  CHECK_FALSE(rl.try_acquire(Priority::High));
  clock.advance(124);
  CHECK_FALSE(rl.try_acquire(Priority::Normal));  // 0.992 tokens
  clock.advance(1);
  CHECK(rl.try_acquire(Priority::Normal));  // 1 token after 125 ms at 8 rps
  clock.advance(60'000);
  CHECK(rl.available() == 8.0);  // capped at burst
}

TEST_CASE("rate limiter: Low keeps a reserve for sends and inbound fetches", "[rate_limiter]") {
  ManualClock clock(5'000);
  RateLimiter rl({.rps = 8, .burst = 8, .low_reserve = 2}, clock);
  int low = 0;
  while (rl.try_acquire(Priority::Low)) ++low;
  CHECK(low == 7);  // 8 → 1 token left: Low needs at least 2
  CHECK(rl.try_acquire(Priority::High));
  CHECK_FALSE(rl.try_acquire(Priority::High));

  // A bucket smaller than the reserve must not starve Low forever.
  RateLimiter tiny({.rps = 1, .burst = 1, .low_reserve = 2}, clock);
  CHECK(tiny.try_acquire(Priority::Low));
}

TEST_CASE("rate limiter: pause_until blocks every priority", "[rate_limiter]") {
  ManualClock clock(10'000);
  RateLimiter rl({}, clock);
  CHECK(rl.paused_until() == 0);
  rl.pause_until(12'000);
  CHECK(rl.paused_until() == 12'000);
  CHECK_FALSE(rl.try_acquire(Priority::High));
  rl.pause_until(11'000);  // an earlier pause never shortens the current one
  CHECK(rl.paused_until() == 12'000);
  clock.set(12'000);
  CHECK(rl.paused_until() == 0);
  CHECK(rl.try_acquire(Priority::High));
  rl.pause_for(500ms);
  CHECK(rl.paused_until() == 12'500);
  CHECK_FALSE(rl.try_acquire(Priority::Low));
  rl.pause_for(-5s);  // negative durations are clamped
  CHECK(rl.paused_until() == 12'500);
}

TEST_CASE("rate limiter: options from Config", "[rate_limiter]") {
  Config cfg;
  auto o = rate_limiter_options_from(cfg);
  CHECK(o.rps == 8.0);
  CHECK(o.burst == 8.0);
  CHECK(o.low_reserve == 2.0);
  cfg.resend_rate_rps = 0.5;
  o = rate_limiter_options_from(cfg);
  CHECK(o.rps == 0.5);
  CHECK(o.burst == 1.0);
  cfg.resend_rate_rps = -3;
  CHECK(rate_limiter_options_from(cfg).rps == 8.0);
  CHECK_NOTHROW(RateLimiter(rate_limiter_options_from(cfg)));
}

TEST_CASE("rate limiter: invalid options are rejected", "[rate_limiter]") {
  CHECK_THROWS_AS(RateLimiter({.rps = 0}), std::invalid_argument);
  CHECK_THROWS_AS(RateLimiter({.rps = -1}), std::invalid_argument);
  CHECK_THROWS_AS(RateLimiter({.rps = 1, .burst = 0.5}), std::invalid_argument);
  CHECK_THROWS_AS(RateLimiter({.rps = 1, .burst = 1, .low_reserve = -1}), std::invalid_argument);
}

TEST_CASE("rate limiter: blocking acquire waits for a refill (real clock)", "[rate_limiter]") {
  RateLimiter rl({.rps = 20, .burst = 1, .low_reserve = 0});
  CHECK(rl.acquire(Priority::High));
  const auto t0 = std::chrono::steady_clock::now();
  CHECK(rl.acquire(Priority::High));
  const auto waited = std::chrono::steady_clock::now() - t0;
  CHECK(waited >= 35ms);  // ~50 ms at 20 rps
  CHECK(waited < 1s);
}

TEST_CASE("rate limiter: waiters are served High, then Normal, then Low", "[rate_limiter]") {
  // ManualClock: the pause only ends when the test moves the clock, after every waiter is queued
  // (blocked waits re-check the clock at least every 100 ms).
  ManualClock clock(1'000'000);
  RateLimiter rl({.rps = 10, .burst = 2, .low_reserve = 2}, clock);
  rl.pause_for(1s);
  std::mutex mu;
  std::vector<Priority> order;
  bool all_ok = true;
  auto waiter = [&](Priority p) {
    const bool ok = rl.acquire(p);
    std::lock_guard lk(mu);
    all_ok = all_ok && ok;
    order.push_back(p);
  };
  std::thread low(waiter, Priority::Low);
  std::thread normal(waiter, Priority::Normal);
  std::thread high(waiter, Priority::High);
  std::this_thread::sleep_for(300ms);  // all three are blocked by the pause
  clock.advance(1'000);                // pause over; 2 tokens: High, then Normal
  high.join();
  normal.join();
  clock.advance(1'000);  // refill for Low (needs 2 tokens)
  low.join();
  CHECK(all_ok);
  REQUIRE(order.size() == 3);
  CHECK(order[0] == Priority::High);
  CHECK(order[1] == Priority::Normal);
  CHECK(order[2] == Priority::Low);
}

TEST_CASE("rate limiter: acquire returns false when stopped", "[rate_limiter]") {
  RateLimiter rl({.rps = 1, .burst = 1});
  rl.pause_for(10s);
  std::stop_source already;
  already.request_stop();
  CHECK_FALSE(rl.acquire(Priority::High, already.get_token()));

  std::stop_source src;
  std::thread stopper([&] {
    std::this_thread::sleep_for(50ms);
    src.request_stop();
  });
  const auto t0 = std::chrono::steady_clock::now();
  CHECK_FALSE(rl.acquire(Priority::Normal, src.get_token()));
  CHECK(std::chrono::steady_clock::now() - t0 < 2s);
  stopper.join();
}
