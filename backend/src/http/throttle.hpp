// Owner: WP-A
//
// LoginThrottle (DESIGN D2): sliding-window failure counters per email and per client IP, plus
// a counting semaphore bounding concurrent scrypt operations (CPU DoS guard).
//   * check() before verifying a password; when it returns a value the handler answers
//     429 "too_many_attempts" with details {retry_after: <seconds>}.
//   * record_failure() after a failed login (unknown email counts too); record_success()
//     clears the email's counter (the IP counter keeps sliding).
// Thread-safe. In-memory only (resets on restart). Emails are compared normalized (lowercase).
// Memory is bounded whatever clients send (SEC-2): emails are keyed by their sha256 (32 bytes,
// never the attacker-sized string), an empty email counts against the IP only, IPv6 clients
// are counted per /64 (one subscriber's prefix), and each map keeps at most kMaxKeys entries
// (the stalest are evicted first).
#pragma once

#include "core/time.hpp"

#include <chrono>
#include <memory>
#include <optional>
#include <string_view>

namespace azm {
struct Config;
}

namespace azm::http {

class LoginThrottle {
 public:
  struct Limits {
    int max_per_email = 5;   // failures per window per email (cfg.login_max_per_email)
    int max_per_ip = 20;     // failures per window per IP (cfg.login_max_per_ip)
    std::chrono::seconds window{900};  // sliding window (cfg.login_window_sec)
    int scrypt_concurrency = 4;        // concurrent scrypt permits (cfg.scrypt_concurrency)
  };

  // Additive (SEC-2): entry cap per map (emails, IPs).
  static constexpr std::size_t kMaxKeys = 100'000;

  explicit LoginThrottle(Limits limits, const Clock& clock = system_clock());
  explicit LoginThrottle(const Config& cfg, const Clock& clock = system_clock());
  ~LoginThrottle();
  LoginThrottle(const LoginThrottle&) = delete;
  LoginThrottle& operator=(const LoginThrottle&) = delete;

  // nullopt = allowed. Otherwise the number of seconds (≥ 1) until the oldest counted failure
  // of the exceeded key leaves the window.
  std::optional<int> check(std::string_view email, std::string_view ip) const;
  void record_failure(std::string_view email, std::string_view ip);
  void record_success(std::string_view email, std::string_view ip);

  // RAII permit for one scrypt hash/verify. Movable; releases on destruction.
  class ScryptPermit {
   public:
    ScryptPermit(ScryptPermit&& o) noexcept : owner_(o.owner_) { o.owner_ = nullptr; }
    ScryptPermit& operator=(ScryptPermit&&) = delete;
    ScryptPermit(const ScryptPermit&) = delete;
    ScryptPermit& operator=(const ScryptPermit&) = delete;
    ~ScryptPermit();

   private:
    friend class LoginThrottle;
    explicit ScryptPermit(LoginThrottle* owner) : owner_(owner) {}
    LoginThrottle* owner_;
  };
  // Blocks while scrypt_concurrency permits are held.
  ScryptPermit acquire_scrypt();

  // Drops entries whose failures all left the window (also done opportunistically).
  void prune();

  // Additive (SEC-2, tests/metrics): current number of tracked emails and IP keys.
  std::size_t email_keys() const;
  std::size_t ip_keys() const;
  // Additive (SEC-2): the per-IP key — IPv4 (and IPv4-mapped IPv6) as is, other IPv6 addresses
  // as their /64 prefix ("2001:db8:1:2::/64"), anything unparsable as given (≤ 64 bytes).
  static std::string ip_key(std::string_view ip);

 private:
  void release_scrypt() noexcept;
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace azm::http
