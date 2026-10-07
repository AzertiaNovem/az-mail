// Owner: WP-C
//
// Priority token bucket shared by all Resend calls (DESIGN B6):
//  * refills at `rps` tokens/s up to `burst` tokens (cfg.resend_rate_rps, default 8; the
//    10 rps limit is per team);
//  * Priority::Low waits while fewer than `low_reserve` tokens (2) are available, so sends and
//    inbound fetches keep headroom; High is served before Normal before Low when waiting;
//  * pause_until(ms) (after a 429 rate_limit_exceeded with retry-after) blocks every priority
//    until that time;
//  * S3/R2 and Resend download URLs are NOT rate-limited.
// Thread-safe. Time comes from the injected Clock; blocking waits use real sleeps, so tests
// with a ManualClock use try_acquire().
#pragma once

#include "core/time.hpp"
#include "resend/types.hpp"

#include <cstdint>
#include <memory>
#include <stop_token>
#include <utility>

namespace azm::resend {

// ---- thread-local stop token (inline, implemented in WP0) ----------------------------------
// resend::Client methods carry no stop_token, yet may block in RateLimiter::acquire (up to a
// 429 pause). jobs::Runner installs the job's token around each handler call; Client passes
// current_stop_token() to acquire() and, when acquire returns false (stop requested), throws
// resend::Error{Kind::Network, name "stopped"} (retryable) so the job ends promptly on shutdown.
// Request threads have no token installed (default std::stop_token: never stops).
namespace detail {
inline thread_local std::stop_token tl_stop_token;
}
class ScopedStopToken {
 public:
  explicit ScopedStopToken(std::stop_token st) noexcept
      : prev_(std::exchange(detail::tl_stop_token, std::move(st))) {}
  ~ScopedStopToken() { detail::tl_stop_token = std::move(prev_); }
  ScopedStopToken(const ScopedStopToken&) = delete;
  ScopedStopToken& operator=(const ScopedStopToken&) = delete;

 private:
  std::stop_token prev_;
};
// The token installed on this thread (default-constructed when none).
inline std::stop_token current_stop_token() noexcept { return detail::tl_stop_token; }

class RateLimiter {
 public:
  struct Options {
    double rps = 8.0;         // tokens per second (> 0)
    double burst = 8.0;       // bucket capacity (≥ 1)
    double low_reserve = 2.0;  // Low priority needs more than this many tokens
  };

  explicit RateLimiter(Options opts, const Clock& clock = system_clock());
  ~RateLimiter();
  RateLimiter(const RateLimiter&) = delete;
  RateLimiter& operator=(const RateLimiter&) = delete;

  // Blocks until a token for priority `p` is taken (true) or `st` is stopped (false).
  bool acquire(Priority p, std::stop_token st = {});
  // Takes a token without blocking; false when none is available for `p` or paused.
  bool try_acquire(Priority p);
  // Pause every priority until `until_ms` (ms epoch). A later pause extends, an earlier one is ignored.
  void pause_until(int64_t until_ms);
  int64_t paused_until() const;  // 0 when not paused
  double available() const;      // current token count (after refill), for tests/metrics

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace azm::resend
