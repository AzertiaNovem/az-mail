// Owner: WP-A
// LoginThrottle: sliding-window failure counters + scrypt semaphore (DESIGN D2, throttle.hpp).
#include "http/throttle.hpp"

#include "config.hpp"
#include "core/crypto.hpp"
#include "core/strings.hpp"

#include <boost/asio/ip/address.hpp>

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

// Fixed-size key of a (normalized) email: memory per entry never depends on what was sent.
std::string email_key(std::string_view email) { return crypto::sha256(to_lower_ascii(trim(email))); }

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

  // Keeps `m` at most kMaxKeys entries: prune first, then evict the entries whose latest failure
  // is oldest (a tenth at a time, so a flood pays the scan rarely).
  void cap_locked(std::unordered_map<std::string, Window>& m, int64_t now) const {
    if (m.size() <= kMaxKeys) return;
    prune_locked(now);
    if (m.size() <= kMaxKeys) return;
    std::vector<std::pair<int64_t, std::string>> by_age;
    by_age.reserve(m.size());
    for (const auto& [k, w] : m) by_age.emplace_back(w.empty() ? 0 : w.back(), k);
    const std::size_t drop = m.size() - kMaxKeys + kMaxKeys / 10;
    std::nth_element(by_age.begin(), by_age.begin() + static_cast<std::ptrdiff_t>(drop - 1), by_age.end());
    for (std::size_t i = 0; i < drop; ++i) m.erase(by_age[i].second);
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
  if (!trim(email).empty()) consider(impl_->by_email, email_key(email), impl_->limits.max_per_email);
  if (!ip.empty()) consider(impl_->by_ip, ip_key(ip), impl_->limits.max_per_ip);
  return worst;
}

void LoginThrottle::record_failure(std::string_view email, std::string_view ip) {
  const int64_t now = impl_->clock.now_ms();
  const std::string ekey = trim(email).empty() ? std::string() : email_key(email);
  const std::string ikey = ip.empty() ? std::string() : ip_key(ip);
  std::lock_guard lk(impl_->mu);
  if (!ekey.empty()) {
    auto& ew = impl_->by_email[ekey];
    slide(ew, now, impl_->window_ms);
    Impl::push_bounded(ew, now, impl_->limits.max_per_email);
    impl_->cap_locked(impl_->by_email, now);
  }
  if (!ikey.empty()) {
    auto& iw = impl_->by_ip[ikey];
    slide(iw, now, impl_->window_ms);
    Impl::push_bounded(iw, now, impl_->limits.max_per_ip);
    impl_->cap_locked(impl_->by_ip, now);
  }
  // Opportunistic cleanup so a spray of distinct emails/IPs cannot grow the maps forever.
  if (++impl_->ops_since_prune >= 1024) {
    impl_->ops_since_prune = 0;
    impl_->prune_locked(now);
  }
}

void LoginThrottle::record_success(std::string_view email, std::string_view ip) {
  (void)ip;  // the IP window keeps sliding (one account's success says nothing about the IP)
  if (trim(email).empty()) return;
  const std::string ekey = email_key(email);
  std::lock_guard lk(impl_->mu);
  impl_->by_email.erase(ekey);
}

std::size_t LoginThrottle::email_keys() const {
  std::lock_guard lk(impl_->mu);
  return impl_->by_email.size();
}

std::size_t LoginThrottle::ip_keys() const {
  std::lock_guard lk(impl_->mu);
  return impl_->by_ip.size();
}

std::string LoginThrottle::ip_key(std::string_view ip) {
  std::string_view s = trim(ip);
  if (s.size() >= 2 && s.front() == '[' && s.back() == ']') s = s.substr(1, s.size() - 2);
  boost::system::error_code ec;
  const auto a = boost::asio::ip::make_address(std::string(s), ec);
  if (ec) return std::string(s.substr(0, 64));
  if (a.is_v4()) return a.to_string();
  const auto v6 = a.to_v6();
  if (v6.is_v4_mapped()) return boost::asio::ip::make_address_v4(boost::asio::ip::v4_mapped, v6).to_string();
  auto bytes = v6.to_bytes();
  for (std::size_t i = 8; i < bytes.size(); ++i) bytes[i] = 0;
  return boost::asio::ip::address_v6(bytes).to_string() + "/64";
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
