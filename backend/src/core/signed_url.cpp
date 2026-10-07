#include "core/signed_url.hpp"

#include "core/crypto.hpp"

#include <stdexcept>

namespace azm {
namespace {

constexpr int64_t kHourMs = 3600LL * 1000;

void check_disposition(char d) {
  if (d != 'i' && d != 'a') throw std::invalid_argument("signed url disposition must be 'i' or 'a'");
}

std::string file_payload(int64_t id, int64_t uid, int64_t exp, char d) {
  std::string p = "file|" + std::to_string(id) + "|" + std::to_string(uid) + "|" +
                  std::to_string(exp) + "|";
  p.push_back(d);
  return p;
}

std::string raw_payload(int64_t id, int64_t uid, int64_t exp) {
  return "raw|" + std::to_string(id) + "|" + std::to_string(uid) + "|" + std::to_string(exp);
}

}  // namespace

SignedUrls::SignedUrls(std::string secret, std::string api_base_url, int64_t ttl_ms)
    : ttl_ms_(ttl_ms) {
  if (secret.empty()) throw std::invalid_argument("SignedUrls: empty secret");
  if (ttl_ms <= 0) throw std::invalid_argument("SignedUrls: ttl must be positive");
  // Domain separation: the server secret may be reused for other MACs.
  key_ = crypto::hmac_sha256(secret, "azmail/signed-url/v1");
  while (!api_base_url.empty() && api_base_url.back() == '/') api_base_url.pop_back();
  base_ = std::move(api_base_url);
}

std::string SignedUrls::sign_file(int64_t id, int64_t uid, char d, int64_t exp) const {
  check_disposition(d);
  return crypto::b64url_encode(crypto::hmac_sha256(key_, file_payload(id, uid, exp, d)));
}

std::string SignedUrls::sign_raw(int64_t id, int64_t uid, int64_t exp) const {
  return crypto::b64url_encode(crypto::hmac_sha256(key_, raw_payload(id, uid, exp)));
}

std::string SignedUrls::file_url(int64_t id, int64_t uid, char d, int64_t exp) const {
  const std::string sig = sign_file(id, uid, d, exp);
  std::string url = base_ + "/api/files/" + std::to_string(id) + "?d=";
  url.push_back(d);
  url += "&u=" + std::to_string(uid) + "&exp=" + std::to_string(exp) + "&sig=" + sig;
  return url;
}

std::string SignedUrls::raw_url(int64_t id, int64_t uid, int64_t exp) const {
  return base_ + "/api/files/raw/" + std::to_string(id) + "?u=" + std::to_string(uid) +
         "&exp=" + std::to_string(exp) + "&sig=" + sign_raw(id, uid, exp);
}

bool SignedUrls::verify_file(int64_t id, int64_t uid, char d, int64_t exp, std::string_view sig,
                             int64_t now_ms) const {
  if (d != 'i' && d != 'a') return false;
  if (now_ms >= exp) return false;
  return crypto::ct_equal(sign_file(id, uid, d, exp), sig);
}

bool SignedUrls::verify_raw(int64_t id, int64_t uid, int64_t exp, std::string_view sig,
                            int64_t now_ms) const {
  if (now_ms >= exp) return false;
  return crypto::ct_equal(sign_raw(id, uid, exp), sig);
}

int64_t SignedUrls::expiry_from(int64_t now_ms, int64_t ttl_ms) {
  const int64_t t = now_ms + ttl_ms;
  const int64_t rem = t % kHourMs;
  return rem == 0 ? t : t + (kHourMs - rem);
}

int64_t SignedUrls::expiry(int64_t now_ms) const {
  // Hour rounding keeps URLs cache-stable; only meaningful when the TTL spans at least an hour.
  return ttl_ms_ >= kHourMs ? expiry_from(now_ms, ttl_ms_) : now_ms + ttl_ms_;
}

}  // namespace azm
