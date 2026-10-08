// Owner: WP-C
//
// Typed Resend API client (DESIGN §3, B1–B10). Synchronous; call only from blocking pools /
// job threads and never inside a DB transaction.
//  * Base URL cfg.resend_api_base, "Authorization: Bearer <RESEND_API_KEY>" (never logged),
//    User-Agent cfg.resend_user_agent, JSON bodies.
//  * Every call first takes a RateLimiter token (priority as documented per method) with
//    acquire(p, current_stop_token()) — false (stop requested) → resend::Error{Kind::Network,
//    "stopped"}; a 429 rate_limit_exceeded calls RateLimiter::pause_until(now + retry_after)
//    before throwing.
//  * Failures throw resend::Error (HTTP errors via classify_error; net::NetError → Kind::Network).
//  * Methods are virtual so handler/job tests can substitute a fake (protected default ctor).
#pragma once

#include "resend/rate_limiter.hpp"
#include "resend/types.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace azm {
struct Config;
}
namespace azm::net {
class HttpClient;
}

namespace azm::resend {

// Additive (RT-4): overall deadline of POST /emails for a `body_bytes` JSON body — `base`
// (RESEND_TIMEOUT_SEC) plus the upload time at `bytes_per_sec` (RESEND_UPLOAD_KBPS), capped at
// max(base, 15 min). The per-chunk idle limit stays RESEND_TIMEOUT_SEC.
std::chrono::milliseconds send_timeout(std::chrono::milliseconds base, std::size_t body_bytes,
                                       std::uint64_t bytes_per_sec);

class Client {
 public:
  Client(const Config&, net::HttpClient&, RateLimiter&);
  virtual ~Client();
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  // POST /emails with Idempotency-Key = req.idempotency_key (Priority::High). Returns the id.
  virtual std::string send(const SendRequest&);
  // GET /emails/{id}.
  virtual SentEmail get(std::string_view id, Priority = Priority::Low);
  // PATCH /emails/{id} {"scheduled_at": iso} (Priority::High). Validation error when the email
  // is no longer scheduled.
  virtual void update_schedule(std::string_view id, std::string_view iso);
  // POST /emails/{id}/cancel (Priority::High). Validation error when already sent.
  virtual void cancel(std::string_view id);
  // GET /emails/receiving/{id}?html_format=cid (Priority::Normal). NotFound right after the
  // webhook is normal (B7; the job retries).
  virtual ReceivedEmail get_received(std::string_view id);
  // GET /emails/receiving?limit=&after=&before= (Priority::Low). limit 1–100.
  virtual ReceivedPage list_received(int limit, std::optional<std::string> after,
                                     std::optional<std::string> before);
  // GET /emails/receiving/{id}/attachments (Priority::Normal): fresh download URLs. Every page
  // (limit=100, after=<last id> while has_more, ≤ 20 pages) concatenated.
  virtual std::vector<RecvAttachment> list_received_attachments(std::string_view id);
  // Downloads a Resend-provided URL to `dest` WITHOUT the Authorization header (redirects
  // followed; not rate-limited). Throws Error(Validation, name "too_large") over max_bytes.
  // Returns the size (DESIGN §3).
  virtual int64_t download(std::string_view url, const std::filesystem::path& dest,
                           std::size_t max_bytes);
  // Same, also returning the sha256 computed while streaming (Addendum A data flow).
  virtual DownloadResult download_to(std::string_view url, const std::filesystem::path& dest,
                                     std::size_t max_bytes);
  // GET /domains (Priority::Low) — admin domain status.
  virtual std::vector<DomainInfo> list_domains();
  // GET /domains/{id} (Priority::Low) — includes DNS records.
  virtual DomainInfo get_domain(std::string_view resend_domain_id);

 protected:
  Client();  // for test doubles

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace azm::resend
