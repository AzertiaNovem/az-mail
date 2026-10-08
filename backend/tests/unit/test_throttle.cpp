// Owner: WP-A — LoginThrottle sliding windows and scrypt semaphore (DESIGN D2).
#include "config.hpp"
#include "http/throttle.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace azm;
using namespace std::chrono_literals;

namespace {
http::LoginThrottle::Limits limits(int per_email = 5, int per_ip = 20, int window_s = 900, int scrypt = 4) {
  http::LoginThrottle::Limits l;
  l.max_per_email = per_email;
  l.max_per_ip = per_ip;
  l.window = std::chrono::seconds(window_s);
  l.scrypt_concurrency = scrypt;
  return l;
}
}  // namespace

TEST_CASE("throttle: 5 failures per email within 15 minutes", "[throttle]") {
  ManualClock clock(1'000'000);
  http::LoginThrottle t(limits(), clock);
  for (int i = 0; i < 4; ++i) {
    CHECK_FALSE(t.check("alice@x.cn", "10.0.0.1"));
    t.record_failure("alice@x.cn", "10.0.0.1");
    clock.advance(10'000);  // 10 s apart
  }
  CHECK_FALSE(t.check("alice@x.cn", "10.0.0.1"));
  t.record_failure("alice@x.cn", "10.0.0.1");  // 5th failure at t0 + 40 s
  // Blocked; also case-insensitively and from another IP (per-email window).
  auto r = t.check("ALICE@x.cn ", "10.0.0.2");
  REQUIRE(r);
  // The oldest failure (t0) leaves at t0 + 900 s; now = t0 + 40 s → 860 s.
  CHECK(*r == 860);
  // Another account from the same IP is not affected (IP limit is 20).
  CHECK_FALSE(t.check("bob@x.cn", "10.0.0.1"));

  // Sliding: once the oldest failure leaves the window, one more attempt is allowed.
  clock.set(1'000'000 + 900'000);
  CHECK_FALSE(t.check("alice@x.cn", "10.0.0.1"));
  t.record_failure("alice@x.cn", "10.0.0.1");
  r = t.check("alice@x.cn", "10.0.0.1");
  REQUIRE(r);
  CHECK(*r == 10);  // the second failure (t0 + 10 s) leaves at t0 + 910 s
}

TEST_CASE("throttle: retry_after is at least 1 second and rounds up", "[throttle]") {
  ManualClock clock(0);
  http::LoginThrottle t(limits(1, 100, 60), clock);
  t.record_failure("a@b.cn", "1.1.1.1");
  clock.set(59'500);
  auto r = t.check("a@b.cn", "1.1.1.1");
  REQUIRE(r);
  CHECK(*r == 1);
  clock.set(59'999);
  CHECK(t.check("a@b.cn", "1.1.1.1") == std::optional<int>(1));
  clock.set(60'000);
  CHECK_FALSE(t.check("a@b.cn", "1.1.1.1"));
  clock.set(0);
  t.record_failure("a@b.cn", "1.1.1.1");
  clock.set(1);
  CHECK(t.check("a@b.cn", "1.1.1.1") == std::optional<int>(60));
}

TEST_CASE("throttle: 20 failures per IP across accounts", "[throttle]") {
  ManualClock clock(5'000);
  http::LoginThrottle t(limits(), clock);
  for (int i = 0; i < 20; ++i) {
    const std::string email = "user" + std::to_string(i) + "@x.cn";
    REQUIRE_FALSE(t.check(email, "203.0.113.9"));
    t.record_failure(email, "203.0.113.9");
  }
  auto r = t.check("fresh@x.cn", "203.0.113.9");
  REQUIRE(r);
  CHECK(*r == 900);
  // Other IPs are fine; an empty IP is not throttled per IP.
  CHECK_FALSE(t.check("fresh@x.cn", "203.0.113.10"));
  CHECK_FALSE(t.check("fresh@x.cn", ""));
}

TEST_CASE("throttle: success clears the email but not the IP window", "[throttle]") {
  ManualClock clock(0);
  http::LoginThrottle t(limits(2, 3, 900), clock);
  t.record_failure("a@x.cn", "9.9.9.9");
  t.record_failure("a@x.cn", "9.9.9.9");
  CHECK(t.check("a@x.cn", "8.8.8.8"));  // email blocked
  t.record_success("A@x.cn", "9.9.9.9");
  CHECK_FALSE(t.check("a@x.cn", "8.8.8.8"));
  t.record_failure("b@x.cn", "9.9.9.9");  // third failure from this IP
  CHECK(t.check("c@x.cn", "9.9.9.9"));     // IP still counts the earlier two
}

TEST_CASE("throttle: worst key wins and prune drops expired entries", "[throttle]") {
  ManualClock clock(0);
  http::LoginThrottle t(limits(1, 1, 100), clock);
  t.record_failure("a@x.cn", "1.2.3.4");  // email window ends at 100 s
  clock.set(50'000);
  t.record_failure("b@x.cn", "5.6.7.8");
  clock.set(60'000);
  // a@x.cn blocked for 40 s, IP 5.6.7.8 for 90 s → 90.
  CHECK(t.check("a@x.cn", "5.6.7.8") == std::optional<int>(90));
  clock.set(1'000'000);
  t.prune();
  CHECK_FALSE(t.check("a@x.cn", "5.6.7.8"));
  CHECK_FALSE(t.check("b@x.cn", "1.2.3.4"));
}

TEST_CASE("throttle: built from Config", "[throttle]") {
  Config cfg;
  cfg.login_max_per_email = 2;
  cfg.login_window_sec = 30;
  ManualClock clock(0);
  http::LoginThrottle t(cfg, clock);
  t.record_failure("x@y.cn", "1.1.1.1");
  CHECK_FALSE(t.check("x@y.cn", "1.1.1.1"));
  t.record_failure("x@y.cn", "1.1.1.1");
  CHECK(t.check("x@y.cn", "1.1.1.1") == std::optional<int>(30));
}

TEST_CASE("throttle: scrypt semaphore bounds concurrency", "[throttle]") {
  http::LoginThrottle t(limits(5, 20, 900, 2));
  std::atomic<int> active{0};
  std::atomic<int> peak{0};
  std::vector<std::thread> threads;
  for (int i = 0; i < 8; ++i) {
    threads.emplace_back([&] {
      auto permit = t.acquire_scrypt();
      const int now = ++active;
      int p = peak.load();
      while (now > p && !peak.compare_exchange_weak(p, now)) {
      }
      std::this_thread::sleep_for(20ms);
      --active;
    });
  }
  for (auto& th : threads) th.join();
  CHECK(peak.load() <= 2);
  CHECK(peak.load() >= 1);

  // Permits are movable and released exactly once.
  {
    auto a = t.acquire_scrypt();
    auto b = std::move(a);
    auto c = t.acquire_scrypt();
  }
  auto d = t.acquire_scrypt();
  auto e = t.acquire_scrypt();  // would block forever if a permit leaked
  SUCCEED();
}

// ---- SEC-2: bounded memory whatever clients send -------------------------------------------

TEST_CASE("throttle: attacker-sized emails are keyed by a fixed-size digest", "[throttle][sec]") {
  ManualClock clock(0);
  http::LoginThrottle t(limits(), clock);
  const std::string huge(1u << 20, 'a');  // 1 MiB "email"
  for (int i = 0; i < 5; ++i) t.record_failure(huge + "@x.cn", "10.0.0.1");
  CHECK(t.email_keys() == 1);
  // Still the same key (case-insensitive, trimmed) and still throttled per email.
  CHECK(t.check(" " + huge + "@X.cn", "10.9.9.9"));
  t.record_success(huge + "@x.cn", "10.0.0.1");
  CHECK(t.email_keys() == 0);
}

TEST_CASE("throttle: an empty email counts against the IP only", "[throttle][sec]") {
  ManualClock clock(0);
  http::LoginThrottle t(limits(5, 3), clock);
  for (int i = 0; i < 3; ++i) t.record_failure("", "198.51.100.7");
  CHECK(t.email_keys() == 0);
  CHECK(t.ip_keys() == 1);
  CHECK(t.check("", "198.51.100.7") == std::optional<int>(900));
  CHECK(t.check("someone@x.cn", "198.51.100.7"));  // the IP is blocked for every account
  CHECK_FALSE(t.check("", "198.51.100.8"));
}

TEST_CASE("throttle: IPv6 clients are counted per /64", "[throttle][sec]") {
  using LT = http::LoginThrottle;
  CHECK(LT::ip_key("203.0.113.9") == "203.0.113.9");
  CHECK(LT::ip_key("::ffff:203.0.113.9") == "203.0.113.9");
  CHECK(LT::ip_key("2001:db8:1:2:aaaa:bbbb:cccc:dddd") == "2001:db8:1:2::/64");
  CHECK(LT::ip_key("[2001:db8:1:2::1]") == "2001:db8:1:2::/64");
  CHECK(LT::ip_key("not-an-ip") == "not-an-ip");
  CHECK(LT::ip_key(std::string(500, 'z')).size() == 64);

  ManualClock clock(0);
  LT t(limits(5, 20), clock);
  // A new address of the same /64 for every attempt: still one IP window.
  for (int i = 0; i < 20; ++i) {
    const std::string ip = "2001:db8:1:2::" + std::to_string(i + 1);
    REQUIRE_FALSE(t.check("u" + std::to_string(i) + "@x.cn", ip));
    t.record_failure("u" + std::to_string(i) + "@x.cn", ip);
  }
  CHECK(t.check("fresh@x.cn", "2001:db8:1:2:ffff::9"));
  CHECK_FALSE(t.check("fresh@x.cn", "2001:db8:1:3::1"));  // another /64
  CHECK(t.ip_keys() == 1);
}

TEST_CASE("throttle: each map keeps at most kMaxKeys entries", "[throttle][sec]") {
  ManualClock clock(0);
  http::LoginThrottle t(limits(), clock);
  const std::size_t n = http::LoginThrottle::kMaxKeys + 500;
  for (std::size_t i = 0; i < n; ++i) {
    t.record_failure("spray" + std::to_string(i) + "@x.cn", "10." + std::to_string((i >> 16) & 255) + "." +
                                                                std::to_string((i >> 8) & 255) + "." +
                                                                std::to_string(i & 255));
    clock.advance(1);  // all inside the window: pruning alone frees nothing
  }
  CHECK(t.email_keys() <= http::LoginThrottle::kMaxKeys);
  CHECK(t.ip_keys() <= http::LoginThrottle::kMaxKeys);
  // The newest entries survive the eviction (the stalest go first): the newest email keeps its
  // failure and reaches the limit with four more.
  const std::string newest = "spray" + std::to_string(n - 1) + "@x.cn";
  for (int i = 0; i < 4; ++i) t.record_failure(newest, "192.0.2.1");
  CHECK(t.check(newest, "192.0.2.2"));
}
