// Owner: WP-A
// LoginThrottle: sliding-window failure counters + scrypt semaphore (DESIGN D2, throttle.hpp).
#include "http/throttle.hpp"

#include "config.hpp"
#include "core/strings.hpp"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>

namespace azm::http {

namespace {

using Window = std::deque<int64_t>;  // failure timestamps (ms), ascending

// Drops timestamps that left the window [now - window, now].
void slide(Window& w, int64_t now, int64_t window_ms) {
  while (!w.empty() && w.front() <= now - window_ms) w.pop_front();
}

// Seconds until the window holds fewer than `max` failures again (≥ 1), or nullopt when it
// already does.
std::optional<int> blocked_for(const Window& w, int max, int64_t now, int64_t window_ms) {
  if (max <= 0 || static_cast<int>(w.size()) < max) return std::nullopt;
  // The (size - max + 1)-th oldest failure must leave before one more attempt is allowed.
  const int64_t leaves_at = w[w.size() - static_cast<std::size_t>(max)] + window_ms;
  const int64_t ms = std::max<int64_t>(leaves_at - now, 1);
  return static_cast<int>((ms + 999) / 1000);
}

LoginThrottle::Limits limits_from(const Config& cfg) {
  LoginThrottle::Limits l;
  l.max_per_email = cfg.login_max_per_email;
  l.max_per_ip = cfg.login_max_per_ip;
  l.window = std::chrono::seconds(cfg.login_window_sec);
  l.scrypt_concurrency = cfg.scrypt_concurrency;
  return l;
}

}  // namespace

struct LoginThrottle::Impl {
  Limits limits;
  const Clock& clock;
  int64_t window_ms;

  mutable std::mutex mu;
  // Mutated by check() too (expired timestamps are dropped), hence mutable.
  mutable std::unordered_map<std::string, Window> by_email;
  mutable std::unordered_map<std::string, Window> by_ip;
  std::size_t ops_since_prune = 0;

  std::mutex scrypt_mu;
  std::condition_variable scrypt_cv;
  int scrypt_in_use = 0;

  Impl(Limits l, const Clock& c)
      : limits(l), clock(c), window_ms(std::max<int64_t>(l.window.count(), 1) * 1000) {
    if (limits.scrypt_concurrency < 1) limits.scrypt_concurrency = 1;
  }

  // Keeps per-key memory bounded: a key never needs more than `max` timestamps (older ones
  // cannot influence blocked_for once `max` newer ones exist).
  static void push_bounded(Window& w, int64_t now, int max) {
    w.push_back(now);
    const auto cap = static_cast<std::size_t>(std::max(max, 1));
    while (w.size() > cap) w.pop_front();
  }

  void prune_locked(int64_t now) const {
    for (auto* m : {&by_email, &by_ip}) {
      for (auto it = m->begin(); it != m->end();) {
        slide(it->second, now, window_ms);
        it = it->second.empty() ? m->erase(it) : std::next(it);
      }
    }
  }
};

LoginThrottle::LoginThrottle(Limits limits, const Clock& clock)
    : impl_(std::make_unique<Impl>(limits, clock)) {}
LoginThrottle::LoginThrottle(const Config& cfg, const Clock& clock)
    : impl_(std::make_unique<Impl>(limits_from(cfg), clock)) {}
LoginThrottle::~LoginThrottle() = default;

std::optional<int> LoginThrottle::check(std::string_view email, std::string_view ip) const {
  const int64_t now = impl_->clock.now_ms();
  std::lock_guard lk(impl_->mu);
  std::optional<int> worst;
  auto consider = [&](std::unordered_map<std::string, Window>& map, const std::string& key, int max) {
    auto it = map.find(key);
    if (it == map.end()) return;
    slide(it->second, now, impl_->window_ms);
    if (auto s = blocked_for(it->second, max, now, impl_->window_ms)) worst = std::max(worst.value_or(0), *s);
    if (it->second.empty()) map.erase(it);
  };
  consider(impl_->by_email, to_lower_ascii(trim(email)), impl_->limits.max_per_email);
  if (!ip.empty()) consider(impl_->by_ip, std::string(ip), impl_->limits.max_per_ip);
  return worst;
}

void LoginThrottle::record_failure(std::string_view email, std::string_view ip) {
  const int64_t now = impl_->clock.now_ms();
  std::lock_guard lk(impl_->mu);
  auto& ew = impl_->by_email[to_lower_ascii(trim(email))];
  slide(ew, now, impl_->window_ms);
  Impl::push_bounded(ew, now, impl_->limits.max_per_email);
  if (!ip.empty()) {
    auto& iw = impl_->by_ip[std::string(ip)];
    slide(iw, now, impl_->window_ms);
    Impl::push_bounded(iw, now, impl_->limits.max_per_ip);
  }
  // Opportunistic cleanup so a spray of distinct emails/IPs cannot grow the maps forever.
  if (++impl_->ops_since_prune >= 1024) {
    impl_->ops_since_prune = 0;
    impl_->prune_locked(now);
  }
}

void LoginThrottle::record_success(std::string_view email, std::string_view ip) {
  (void)ip;  // the IP window keeps sliding (one account's success says nothing about the IP)
  std::lock_guard lk(impl_->mu);
  impl_->by_email.erase(to_lower_ascii(trim(email)));
}

LoginThrottle::ScryptPermit::~ScryptPermit() {
  if (owner_) owner_->release_scrypt();
}

LoginThrottle::ScryptPermit LoginThrottle::acquire_scrypt() {
  std::unique_lock lk(impl_->scrypt_mu);
  impl_->scrypt_cv.wait(lk, [&] { return impl_->scrypt_in_use < impl_->limits.scrypt_concurrency; });
  ++impl_->scrypt_in_use;
  return ScryptPermit(this);
}

void LoginThrottle::prune() {
  const int64_t now = impl_->clock.now_ms();
  std::lock_guard lk(impl_->mu);
  impl_->prune_locked(now);
}

void LoginThrottle::release_scrypt() noexcept {
  {
    std::lock_guard lk(impl_->scrypt_mu);
    if (impl_->scrypt_in_use > 0) --impl_->scrypt_in_use;
  }
  impl_->scrypt_cv.notify_one();
}

}  // namespace azm::http
