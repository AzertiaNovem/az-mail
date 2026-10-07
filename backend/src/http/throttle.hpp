// Owner: WP-A
//
// LoginThrottle (DESIGN D2): sliding-window failure counters per email and per client IP, plus
// a counting semaphore bounding concurrent scrypt operations (CPU DoS guard).
//   * check() before verifying a password; when it returns a value the handler answers
//     429 "too_many_attempts" with details {retry_after: <seconds>}.
//   * record_failure() after a failed login (unknown email counts too); record_success()
//     clears the email's counter (the IP counter keeps sliding).
// Thread-safe. In-memory only (resets on restart). Emails are compared normalized (lowercase).
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

 private:
  void release_scrypt() noexcept;
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace azm::http
