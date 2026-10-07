// Owner: WP-C
// Pure AWS Signature Version 4 (DESIGN Addendum A.2). Verified byte-exact against the AWS S3
// documentation vectors in tests/unit/test_s3_sigv4.cpp.
#include "storage/s3_sigv4.hpp"

#include "core/crypto.hpp"
#include "core/strings.hpp"
#include "core/time.hpp"

#include <algorithm>
#include <cstdio>
#include <stdexcept>

namespace azm::storage::sigv4 {
namespace {

bool is_unreserved(unsigned char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
         c == '.' || c == '_' || c == '~';
}

bool is_ws(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v'; }

// Trim + collapse inner whitespace runs to one space (SigV4 canonical header values).
std::string canonical_value(std::string_view v) {
  std::string out;
  out.reserve(v.size());
  bool pending_space = false;
  for (char c : trim(v)) {
    if (is_ws(c)) {
      pending_space = true;
      continue;
    }
    if (pending_space && !out.empty()) out.push_back(' ');
    pending_space = false;
    out.push_back(c);
  }
  return out;
}

void require_amz_date(std::string_view amz_date) {
  // YYYYMMDDTHHMMSSZ
  if (amz_date.size() != 16 || amz_date[8] != 'T' || amz_date[15] != 'Z')
    throw std::invalid_argument("sigv4: amz_date must look like YYYYMMDDTHHMMSSZ");
}

}  // namespace

std::string uri_encode(std::string_view s, bool encode_slash) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(s.size() * 3 / 2);
  for (unsigned char c : s) {
    if (is_unreserved(c) || (c == '/' && !encode_slash)) {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(kHex[c >> 4]);
      out.push_back(kHex[c & 0x0f]);
    }
  }
  return out;
}

std::string canonical_uri(std::string_view raw_path) {
  if (raw_path.empty()) return "/";
  std::string out = uri_encode(raw_path, /*encode_slash=*/false);
  if (out.front() != '/') out.insert(out.begin(), '/');
  return out;
}

std::string canonical_query(std::span<const std::pair<std::string, std::string>> params) {
  std::vector<std::pair<std::string, std::string>> enc;
  enc.reserve(params.size());
  for (const auto& [k, v] : params) enc.emplace_back(uri_encode(k, true), uri_encode(v, true));
  std::sort(enc.begin(), enc.end());
  std::string out;
  for (const auto& [k, v] : enc) {
    if (!out.empty()) out.push_back('&');
    out += k;
    out.push_back('=');
    out += v;
  }
  return out;
}

CanonicalHeaders canonical_headers(std::span<const std::pair<std::string, std::string>> headers) {
  // name → values in input order (duplicates joined with ',').
  std::vector<std::pair<std::string, std::string>> merged;
  for (const auto& [k, v] : headers) {
    std::string name = to_lower_ascii(trim(k));
    std::string value = canonical_value(v);
    auto it = std::find_if(merged.begin(), merged.end(), [&](const auto& p) { return p.first == name; });
    if (it == merged.end()) {
      merged.emplace_back(std::move(name), std::move(value));
    } else {
      it->second.push_back(',');
      it->second += value;
    }
  }
  std::stable_sort(merged.begin(), merged.end(),
                   [](const auto& a, const auto& b) { return a.first < b.first; });
  CanonicalHeaders ch;
  for (const auto& [k, v] : merged) {
    ch.block += k;
    ch.block.push_back(':');
    ch.block += v;
    ch.block.push_back('\n');
    if (!ch.signed_headers.empty()) ch.signed_headers.push_back(';');
    ch.signed_headers += k;
  }
  return ch;
}

std::string canonical_request(std::string_view method, std::string_view canonical_uri_,
                              std::string_view canonical_query_, const CanonicalHeaders& headers,
                              std::string_view payload_sha256_hex) {
  std::string out;
  out.reserve(method.size() + canonical_uri_.size() + canonical_query_.size() +
              headers.block.size() + headers.signed_headers.size() + payload_sha256_hex.size() + 8);
  out += method;
  out.push_back('\n');
  out += canonical_uri_;
  out.push_back('\n');
  out += canonical_query_;
  out.push_back('\n');
  out += headers.block;  // each line ends with '\n' → the blank line after the block
  out.push_back('\n');
  out += headers.signed_headers;
  out.push_back('\n');
  out += payload_sha256_hex;
  return out;
}

std::string credential_scope(std::string_view date8, std::string_view region,
                             std::string_view service) {
  std::string out(date8);
  out.push_back('/');
  out += region;
  out.push_back('/');
  out += service;
  out += "/aws4_request";
  return out;
}

std::string string_to_sign(std::string_view amz_date_, std::string_view scope,
                           std::string_view canonical_request_) {
  std::string out(kAlgorithm);
  out.push_back('\n');
  out += amz_date_;
  out.push_back('\n');
  out += scope;
  out.push_back('\n');
  out += crypto::sha256_hex(canonical_request_);
  return out;
}

std::string signing_key(std::string_view secret_access_key, std::string_view date8,
                        std::string_view region, std::string_view service) {
  std::string k = "AWS4";
  k += secret_access_key;
  std::string k_date = crypto::hmac_sha256(k, date8);
  std::string k_region = crypto::hmac_sha256(k_date, region);
  std::string k_service = crypto::hmac_sha256(k_region, service);
  return crypto::hmac_sha256(k_service, "aws4_request");
}

std::string signature(std::string_view signing_key_raw, std::string_view string_to_sign_) {
  return crypto::hex_encode(crypto::hmac_sha256(signing_key_raw, string_to_sign_));
}

std::string authorization_header(std::string_view access_key_id, std::string_view scope,
                                 std::string_view signed_headers, std::string_view signature_hex) {
  std::string out(kAlgorithm);
  out += " Credential=";
  out += access_key_id;
  out.push_back('/');
  out += scope;
  out += ", SignedHeaders=";
  out += signed_headers;
  out += ", Signature=";
  out += signature_hex;
  return out;
}

std::string amz_date(int64_t ms) {
  const CivilTime t = civil_from_ms(ms);
  char buf[32];
  std::snprintf(buf, sizeof buf, "%04d%02d%02dT%02d%02d%02dZ", t.year, t.month, t.day, t.hour,
                t.minute, t.second);
  return buf;
}

SignedRequest sign_request(const Credentials& creds, const RequestToSign& req) {
  require_amz_date(req.amz_date);
  if (req.payload_sha256.empty()) throw std::invalid_argument("sigv4: payload hash required");
  const std::string date8 = req.amz_date.substr(0, 8);
  const CanonicalHeaders ch = canonical_headers(req.headers);
  SignedRequest out;
  out.canonical_request = canonical_request(req.method, canonical_uri(req.raw_path),
                                            canonical_query(req.query), ch, req.payload_sha256);
  const std::string scope = credential_scope(date8, req.region, req.service);
  out.string_to_sign = string_to_sign(req.amz_date, scope, out.canonical_request);
  out.signature =
      signature(signing_key(creds.secret_access_key, date8, req.region, req.service), out.string_to_sign);
  out.signed_headers = ch.signed_headers;
  out.authorization = authorization_header(creds.access_key_id, scope, ch.signed_headers, out.signature);
  return out;
}

PresignedUrl presign(const Credentials& creds, const PresignInput& in) {
  require_amz_date(in.amz_date);
  if (in.expires_sec < 1 || in.expires_sec > 604800)
    throw std::invalid_argument("sigv4: presign expiry must be within 1..604800 s");
  if (in.host.empty()) throw std::invalid_argument("sigv4: presign host required");
  const std::string date8 = in.amz_date.substr(0, 8);
  const std::string scope = credential_scope(date8, in.region, in.service);

  KeyValues query = in.extra_query;
  query.emplace_back("X-Amz-Algorithm", std::string(kAlgorithm));
  query.emplace_back("X-Amz-Credential", creds.access_key_id + "/" + scope);
  query.emplace_back("X-Amz-Date", in.amz_date);
  query.emplace_back("X-Amz-Expires", std::to_string(in.expires_sec));
  query.emplace_back("X-Amz-SignedHeaders", "host");
  const std::string cq = canonical_query(query);
  const KeyValues host_header{{"host", in.host}};
  const CanonicalHeaders ch = canonical_headers(host_header);
  const std::string curi = canonical_uri(in.raw_path);

  PresignedUrl out;
  out.canonical_request = canonical_request(in.method, curi, cq, ch, kUnsignedPayload);
  const std::string sts = string_to_sign(in.amz_date, scope, out.canonical_request);
  out.signature = signature(signing_key(creds.secret_access_key, date8, in.region, in.service), sts);
  out.url = in.scheme + "://" + in.host + curi + "?" + cq + "&X-Amz-Signature=" + out.signature;
  return out;
}

}  // namespace azm::storage::sigv4
