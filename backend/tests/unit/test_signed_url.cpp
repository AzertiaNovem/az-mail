#include "core/signed_url.hpp"

#include <catch2/catch_test_macros.hpp>

#include <map>
#include <stdexcept>

using azm::SignedUrls;

namespace {

constexpr int64_t kNow = 1791374400000;  // 2026-10-07T12:00:00Z

// Splits "<path>?k=v&k=v" into path + params.
std::map<std::string, std::string> query_of(const std::string& url, std::string* path = nullptr) {
  std::map<std::string, std::string> q;
  const auto qm = url.find('?');
  if (path) *path = url.substr(0, qm);
  std::string rest = url.substr(qm + 1);
  std::size_t pos = 0;
  while (pos <= rest.size()) {
    auto amp = rest.find('&', pos);
    if (amp == std::string::npos) amp = rest.size();
    const std::string kv = rest.substr(pos, amp - pos);
    const auto eq = kv.find('=');
    q[kv.substr(0, eq)] = kv.substr(eq + 1);
    pos = amp + 1;
  }
  return q;
}

}  // namespace

TEST_CASE("file_url format and verification", "[signed_url]") {
  const SignedUrls su("server-secret-0123456789", "https://api.example.com/");
  CHECK(su.base_url() == "https://api.example.com");
  const int64_t exp = kNow + SignedUrls::kDefaultTtlMs;
  const std::string url = su.file_url(42, 7, 'i', exp);

  std::string path;
  auto q = query_of(url, &path);
  CHECK(path == "https://api.example.com/api/files/42");
  CHECK(url.starts_with("https://api.example.com/api/files/42?d=i&u=7&exp=" + std::to_string(exp) + "&sig="));
  CHECK(q["d"] == "i");
  CHECK(q["u"] == "7");
  CHECK(q["exp"] == std::to_string(exp));
  const std::string sig = q["sig"];
  CHECK(sig.size() == 43);  // 32 bytes b64url, no padding
  CHECK(sig.find_first_of("+/=") == std::string::npos);
  CHECK(sig == su.sign_file(42, 7, 'i', exp));

  CHECK(su.verify_file(42, 7, 'i', exp, sig, kNow));
  // Tampering with any field invalidates the signature.
  CHECK_FALSE(su.verify_file(43, 7, 'i', exp, sig, kNow));
  CHECK_FALSE(su.verify_file(42, 8, 'i', exp, sig, kNow));
  CHECK_FALSE(su.verify_file(42, 7, 'i', exp + 1, sig, kNow));
  CHECK_FALSE(su.verify_file(42, 7, 'a', exp, sig, kNow));
  CHECK_FALSE(su.verify_file(42, 7, 'x', exp, sig, kNow));
  std::string bad = sig;
  bad.back() = bad.back() == 'A' ? 'B' : 'A';
  CHECK_FALSE(su.verify_file(42, 7, 'i', exp, bad, kNow));
  CHECK_FALSE(su.verify_file(42, 7, 'i', exp, "", kNow));
  CHECK_FALSE(su.verify_file(42, 7, 'i', exp, sig.substr(1), kNow));
  // Expiry.
  CHECK(su.verify_file(42, 7, 'i', exp, sig, exp - 1));
  CHECK_FALSE(su.verify_file(42, 7, 'i', exp, sig, exp));
  CHECK_FALSE(su.verify_file(42, 7, 'i', exp, sig, exp + 1));

  // Attachment disposition has its own signature.
  const auto qa = query_of(su.file_url(42, 7, 'a', exp));
  CHECK(qa.at("d") == "a");
  CHECK(su.verify_file(42, 7, 'a', exp, qa.at("sig"), kNow));
  CHECK_FALSE(su.verify_file(42, 7, 'i', exp, qa.at("sig"), kNow));
  CHECK_THROWS_AS(su.file_url(42, 7, 'x', exp), std::invalid_argument);
}

TEST_CASE("raw_url format and verification", "[signed_url]") {
  const SignedUrls su("server-secret-0123456789", "http://127.0.0.1:8080");
  const int64_t exp = kNow + 1000;
  const std::string url = su.raw_url(99, 3, exp);
  std::string path;
  auto q = query_of(url, &path);
  CHECK(path == "http://127.0.0.1:8080/api/files/raw/99");
  CHECK(url.starts_with("http://127.0.0.1:8080/api/files/raw/99?u=3&exp=" + std::to_string(exp) + "&sig="));
  CHECK(q.count("d") == 0);
  CHECK(su.verify_raw(99, 3, exp, q["sig"], kNow));
  CHECK_FALSE(su.verify_raw(98, 3, exp, q["sig"], kNow));
  CHECK_FALSE(su.verify_raw(99, 4, exp, q["sig"], kNow));
  CHECK_FALSE(su.verify_raw(99, 3, exp - 1, q["sig"], kNow));
  CHECK_FALSE(su.verify_raw(99, 3, exp, q["sig"], exp));
  // A raw signature never validates as a file signature (and vice versa).
  CHECK_FALSE(su.verify_file(99, 3, 'a', exp, q["sig"], kNow));
  CHECK_FALSE(su.verify_file(99, 3, 'i', exp, q["sig"], kNow));
  CHECK_FALSE(su.verify_raw(99, 3, exp, su.sign_file(99, 3, 'a', exp), kNow));
}

TEST_CASE("signatures depend on the secret", "[signed_url]") {
  const SignedUrls a("secret-a", "https://api.example.com");
  const SignedUrls b("secret-b", "https://api.example.com");
  const int64_t exp = kNow + 1000;
  CHECK(a.sign_file(1, 1, 'i', exp) != b.sign_file(1, 1, 'i', exp));
  CHECK_FALSE(b.verify_file(1, 1, 'i', exp, a.sign_file(1, 1, 'i', exp), kNow));
  CHECK_THROWS_AS(SignedUrls("", "https://x"), std::invalid_argument);
}

TEST_CASE("expiry_from rounds up to the hour", "[signed_url]") {
  constexpr int64_t H = 3600LL * 1000;
  CHECK(SignedUrls::kDefaultTtlMs == 12 * H);
  CHECK(SignedUrls::expiry_from(0) == 12 * H);
  CHECK(SignedUrls::expiry_from(1) == 13 * H);
  CHECK(SignedUrls::expiry_from(H - 1) == 13 * H);
  CHECK(SignedUrls::expiry_from(kNow + 5, H) == kNow + 2 * H);  // kNow is on the hour
  // Stable within an hour.
  CHECK(SignedUrls::expiry_from(kNow + 10) == SignedUrls::expiry_from(kNow + H - 10));
}

TEST_CASE("configured TTL: expiry() and ttl_ms()", "[signed_url]") {
  constexpr int64_t H = 3600LL * 1000;
  const SignedUrls def("0123456789abcdef0123456789abcdef", "https://api.example.com");
  CHECK(def.ttl_ms() == SignedUrls::kDefaultTtlMs);
  CHECK(def.expiry(kNow + 5) == SignedUrls::expiry_from(kNow + 5));  // ≥ 1 h → hour-rounded

  const SignedUrls hour("0123456789abcdef0123456789abcdef", "https://api.example.com", H);
  CHECK(hour.expiry(kNow + 5) == kNow + 2 * H);

  // Short TTLs are exact (E2E "expired → 403" must not wait for the next hour).
  const SignedUrls short_ttl("0123456789abcdef0123456789abcdef", "https://api.example.com", 2000);
  CHECK(short_ttl.ttl_ms() == 2000);
  const int64_t exp = short_ttl.expiry(kNow + 5);
  CHECK(exp == kNow + 2005);
  const std::string sig = short_ttl.sign_file(9, 3, 'a', exp);
  CHECK(short_ttl.verify_file(9, 3, 'a', exp, sig, kNow + 2004));
  CHECK_FALSE(short_ttl.verify_file(9, 3, 'a', exp, sig, kNow + 2005));

  CHECK_THROWS_AS(SignedUrls("0123456789abcdef0123456789abcdef", "https://x", 0), std::invalid_argument);
  CHECK_THROWS_AS(SignedUrls("0123456789abcdef0123456789abcdef", "https://x", -1), std::invalid_argument);
}
