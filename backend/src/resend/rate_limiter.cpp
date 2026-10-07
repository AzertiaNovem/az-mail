// Owner: WP-C
// Priority token bucket for Resend API calls (DESIGN B6).
#include "resend/rate_limiter.hpp"

#include "config.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <stdexcept>

namespace azm::resend {
namespace {

constexpr std::size_t idx(Priority p) {
  switch (p) {
    case Priority::High: return 0;
    case Priority::Normal: return 1;
    case Priority::Low: return 2;
  }
  return 2;
}

// Blocking waits re-check at least this often, so a ManualClock moved by a test, a pause that
// was lifted early or a stop request are noticed promptly.
constexpr int64_t kMaxWaitSliceMs = 100;

}  // namespace

struct RateLimiter::Impl {
  Options opts;
  const Clock* clock = nullptr;
  mutable std::mutex mu;
  std::condition_variable cv;
  // Guarded by mu (mutable: refill happens in const accessors too).
  mutable double tokens = 0;
  mutable int64_t last_ms = 0;
  int64_t paused_until = 0;
  std::array<int, 3> waiting{0, 0, 0};  // blocked acquire() calls per priority

  void refill(int64_t now) const {
    if (now > last_ms) {
      tokens = std::min(opts.burst, tokens + static_cast<double>(now - last_ms) * opts.rps / 1000.0);
      last_ms = now;
    } else if (now < last_ms) {
      last_ms = now;  // clock stepped back: restart accounting from here
    }
  }
  // Low keeps `low_reserve` tokens of headroom (clamped to the bucket size so Low never starves).
  double need(Priority p) const {
    return p == Priority::Low ? std::max(1.0, std::min(opts.low_reserve, opts.burst)) : 1.0;
  }
  // Waiters of a strictly higher priority (or, with `include_same`, the same one) go first.
  bool others_first(Priority p, bool include_same) const {
    const std::size_t me = idx(p);
    for (std::size_t i = 0; i < me; ++i)
      if (waiting[i] > 0) return true;
    return include_same && waiting[me] > 0;
  }
};

RateLimiter::RateLimiter(Options opts, const Clock& clock) : impl_(std::make_unique<Impl>()) {
  if (!(opts.rps > 0) || !std::isfinite(opts.rps)) throw std::invalid_argument("RateLimiter: rps must be > 0");
  if (!(opts.burst >= 1) || !std::isfinite(opts.burst))
    throw std::invalid_argument("RateLimiter: burst must be >= 1");
  if (!(opts.low_reserve >= 0)) throw std::invalid_argument("RateLimiter: low_reserve must be >= 0");
  impl_->opts = opts;
  impl_->clock = &clock;
  impl_->tokens = opts.burst;  // start full
  impl_->last_ms = clock.now_ms();
}

RateLimiter::~RateLimiter() = default;

bool RateLimiter::acquire(Priority p, std::stop_token st) {
  Impl& m = *impl_;
  // Registered before taking the lock: when stop is already requested the callback runs right
  // here (lock free); later it runs on the stopping thread, which must not hold `mu`.
  std::stop_callback on_stop(st, [&m] {
    std::lock_guard lk(m.mu);
    m.cv.notify_all();
  });
  std::unique_lock lk(m.mu);
  ++m.waiting[idx(p)];
  struct Unregister {
    Impl& m;
    Priority p;
    ~Unregister() {
      --m.waiting[idx(p)];
      m.cv.notify_all();  // lower priorities may proceed now / tokens may remain
    }
  } unregister{m, p};

  for (;;) {
    if (st.stop_requested()) return false;
    const int64_t now = m.clock->now_ms();
    m.refill(now);
    const double need = m.need(p);
    if (now >= m.paused_until && m.tokens >= need && !m.others_first(p, false)) {
      m.tokens -= 1.0;
      return true;
    }
    int64_t wait_ms = kMaxWaitSliceMs;
    if (now < m.paused_until) {
      wait_ms = m.paused_until - now;
    } else if (m.tokens < need) {
      wait_ms = static_cast<int64_t>(std::ceil((need - m.tokens) * 1000.0 / m.opts.rps));
    }
    wait_ms = std::clamp<int64_t>(wait_ms, 1, kMaxWaitSliceMs);
    m.cv.wait_for(lk, std::chrono::milliseconds(wait_ms));
  }
}

bool RateLimiter::try_acquire(Priority p) {
  Impl& m = *impl_;
  std::lock_guard lk(m.mu);
  const int64_t now = m.clock->now_ms();
  m.refill(now);
  if (now < m.paused_until || m.tokens < m.need(p) || m.others_first(p, true)) return false;
  m.tokens -= 1.0;
  return true;
}

void RateLimiter::pause_until(int64_t until_ms) {
  Impl& m = *impl_;
  {
    std::lock_guard lk(m.mu);
    m.paused_until = std::max(m.paused_until, until_ms);
  }
  m.cv.notify_all();
}

void RateLimiter::pause_for(std::chrono::milliseconds d) {
  pause_until(impl_->clock->now_ms() + std::max<int64_t>(0, d.count()));
}

int64_t RateLimiter::paused_until() const {
  std::lock_guard lk(impl_->mu);
  return impl_->paused_until > impl_->clock->now_ms() ? impl_->paused_until : 0;
}

RateLimiter::Options rate_limiter_options_from(const Config& cfg) {
  RateLimiter::Options o;
  if (cfg.resend_rate_rps > 0 && std::isfinite(cfg.resend_rate_rps)) o.rps = cfg.resend_rate_rps;
  o.burst = std::max(1.0, o.rps);
  return o;
}

double RateLimiter::available() const {
  std::lock_guard lk(impl_->mu);
  impl_->refill(impl_->clock->now_ms());
  return impl_->tokens;
}

}  // namespace azm::resend
