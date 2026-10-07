// Owner: WP-C
// Svix webhook signatures (DESIGN §4 webhook route, E2E 19).
#include "resend/svix.hpp"

#include "core/crypto.hpp"
#include "core/strings.hpp"

#include <charconv>
#include <optional>
#include <stdexcept>

namespace azm::resend {
namespace {

constexpr std::string_view kSecretPrefix = "whsec_";

// The HMAC key: base64 after the optional "whsec_" prefix. nullopt when malformed/empty.
std::optional<std::string> secret_key(std::string_view whsec) {
  whsec = trim(whsec);
  if (whsec.substr(0, kSecretPrefix.size()) == kSecretPrefix) whsec.remove_prefix(kSecretPrefix.size());
  if (whsec.empty()) return std::nullopt;
  auto key = crypto::b64_decode(whsec);
  if (!key || key->empty()) return std::nullopt;
  return key;
}

std::optional<int64_t> parse_ts(std::string_view ts) {
  ts = trim(ts);
  if (ts.empty() || ts.size() > 18) return std::nullopt;
  int64_t v = 0;
  const char* b = ts.data();
  const char* e = b + ts.size();
  auto [p, ec] = std::from_chars(b, e, v);
  if (ec != std::errc() || p != e || v < 0) return std::nullopt;
  return v;
}

std::string expected_signature(std::string_view key, std::string_view id, std::string_view ts,
                               std::string_view body) {
  std::string signed_content;
  signed_content.reserve(id.size() + ts.size() + body.size() + 2);
  signed_content += id;
  signed_content.push_back('.');
  signed_content += ts;
  signed_content.push_back('.');
  signed_content += body;
  return crypto::b64_encode(crypto::hmac_sha256(key, signed_content));
}

}  // namespace

SvixResult verify_svix(std::string_view whsec, std::string_view id, std::string_view ts,
                       std::string_view sig_header, std::string_view body, int64_t now_s,
                       int tolerance_s) {
  if (trim(id).empty() || trim(ts).empty() || trim(sig_header).empty())
    return SvixResult::MissingHeaders;
  const auto t = parse_ts(ts);
  if (!t) return SvixResult::BadTimestamp;
  const int64_t skew = *t > now_s ? *t - now_s : now_s - *t;
  if (skew > tolerance_s) return SvixResult::BadTimestamp;
  const auto key = secret_key(whsec);
  if (!key) return SvixResult::BadSignature;

  const std::string expected = expected_signature(*key, id, trim(ts), body);
  // The header holds space-separated "<version>,<base64>" entries; only v1 is defined. Every
  // candidate is compared (constant time each) so timing does not reveal which one matched.
  bool ok = false;
  for (const auto& entry : split(sig_header, ' ', /*skip_empty=*/true)) {
    const auto comma = entry.find(',');
    if (comma == std::string::npos) continue;
    if (std::string_view(entry).substr(0, comma) != "v1") continue;
    const std::string_view sig = trim(std::string_view(entry).substr(comma + 1));
    if (crypto::ct_equal(sig, expected)) ok = true;
  }
  return ok ? SvixResult::Ok : SvixResult::BadSignature;
}

std::string sign_svix(std::string_view whsec, std::string_view id, std::string_view ts,
                      std::string_view body) {
  const auto key = secret_key(whsec);
  if (!key) throw std::invalid_argument("svix: malformed webhook secret");
  return "v1," + expected_signature(*key, id, ts, body);
}

std::string_view to_string(SvixResult r) {
  switch (r) {
    case SvixResult::Ok: return "ok";
    case SvixResult::MissingHeaders: return "missing_headers";
    case SvixResult::BadTimestamp: return "bad_timestamp";
    case SvixResult::BadSignature: return "bad_signature";
  }
  return "bad_signature";
}

}  // namespace azm::resend
