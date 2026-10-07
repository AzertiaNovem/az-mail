// HMAC-signed attachment / raw-message URLs (DESIGN D5). Produced only at read time.
//   file: <base>/api/files/<id>?d=i&u=<uid>&exp=<ms>&sig=<b64url>   MAC over "file|id|uid|exp|d"
//   raw:  <base>/api/files/raw/<id>?u=<uid>&exp=<ms>&sig=<b64url>   MAC over "raw|id|uid|exp"
// The handler must still check that the user is active and owns the object. Never log the query.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace azm {

class SignedUrls {
 public:
  static constexpr int64_t kDefaultTtlMs = 12LL * 3600 * 1000;  // 12 h

  // `secret`: raw secret bytes (e.g. Config::server_secret); a purpose-specific key is derived
  // from it. `api_base_url`: e.g. "https://api.example.com" (a trailing '/' is stripped).
  // `ttl_ms`: lifetime of URLs produced via expiry() (App passes cfg.signed_url_ttl_sec * 1000);
  // must be > 0 (std::invalid_argument otherwise).
  SignedUrls(std::string secret, std::string api_base_url, int64_t ttl_ms = kDefaultTtlMs);

  // disposition: 'i' (inline) or 'a' (attachment); anything else throws std::invalid_argument.
  std::string file_url(int64_t attachment_id, int64_t user_id, char disposition,
                       int64_t exp_ms) const;
  std::string raw_url(int64_t message_id, int64_t user_id, int64_t exp_ms) const;

  bool verify_file(int64_t attachment_id, int64_t user_id, char disposition, int64_t exp_ms,
                   std::string_view sig, int64_t now_ms) const;
  bool verify_raw(int64_t message_id, int64_t user_id, int64_t exp_ms, std::string_view sig,
                  int64_t now_ms) const;

  // Signature only (b64url, no padding).
  std::string sign_file(int64_t attachment_id, int64_t user_id, char disposition,
                        int64_t exp_ms) const;
  std::string sign_raw(int64_t message_id, int64_t user_id, int64_t exp_ms) const;

  // now + ttl rounded UP to the next full hour, so URLs (and browser caches of inline images)
  // stay stable across reads within the hour.
  static int64_t expiry_from(int64_t now_ms, int64_t ttl_ms = kDefaultTtlMs);

  // The configured TTL and the expiry every read-time URL uses (mail render, raw_url):
  // expiry_from(now, ttl_ms()) when ttl_ms() >= 1 h, else exactly now + ttl_ms() (short TTLs,
  // e.g. E2E "expired → 403", must not be stretched to the next hour).
  int64_t ttl_ms() const { return ttl_ms_; }
  int64_t expiry(int64_t now_ms) const;

  const std::string& base_url() const { return base_; }

 private:
  std::string key_;
  std::string base_;
  int64_t ttl_ms_ = kDefaultTtlMs;
};

}  // namespace azm
