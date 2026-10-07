// Owner: WP-C — minimal in-memory S3/R2 for tests: path-style PUT/GET/HEAD/DELETE that VERIFIES
// SigV4 (header auth and presigned query auth) from the request exactly as received, re-derived
// from the canonical-request spec (DESIGN A.2), plus fault injection. Usable two ways:
//   * FakeS3Http — a net::HttpClient test double (no sockets), honouring body_file / sink /
//     max_body like the real client;
//   * FakeS3::handle() behind test::FakeHttpServer for true end-to-end tests.
#pragma once

#include "core/crypto.hpp"
#include "core/strings.hpp"
#include "core/time.hpp"
#include "net/http_client.hpp"
#include "storage/s3_sigv4.hpp"

#include <boost/url.hpp>

#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace azm::test {

class FakeS3 {
 public:
  struct Resp {
    unsigned status = 200;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
  };

  FakeS3(std::string bucket, storage::sigv4::Credentials creds, const Clock& clock, std::string region = "auto")
      : bucket_(std::move(bucket)), creds_(std::move(creds)), region_(std::move(region)), clock_(clock) {}

  // ---- fault injection / inspection (thread-safe) ----
  void fail_next_puts(std::vector<unsigned> statuses, bool store_anyway = false) {
    std::lock_guard lk(mu_);
    put_faults_.assign(statuses.begin(), statuses.end());
    put_fault_stores_ = store_anyway;
  }
  void fail_next_gets(std::vector<unsigned> statuses) {
    std::lock_guard lk(mu_);
    get_faults_.assign(statuses.begin(), statuses.end());
  }
  void set_corrupt_gets(bool v) {
    std::lock_guard lk(mu_);
    corrupt_gets_ = v;
  }
  void set_ignore_response_overrides(bool v) {
    std::lock_guard lk(mu_);
    ignore_overrides_ = v;
  }
  void put_object(const std::string& key, std::string bytes) {
    std::lock_guard lk(mu_);
    objects_[key] = std::move(bytes);
  }
  std::optional<std::string> object(const std::string& key) const {
    std::lock_guard lk(mu_);
    auto it = objects_.find(key);
    if (it == objects_.end()) return std::nullopt;
    return it->second;
  }
  std::size_t object_count() const {
    std::lock_guard lk(mu_);
    return objects_.size();
  }
  int count(std::string_view method) const {
    std::lock_guard lk(mu_);
    int n = 0;
    for (const auto& m : log_) n += m.rfind(std::string(method) + " ", 0) == 0;
    return n;
  }
  std::vector<std::string> log() const {
    std::lock_guard lk(mu_);
    return log_;
  }
  // Headers of the last authenticated PUT (lowercased names).
  std::vector<std::pair<std::string, std::string>> last_put_headers() const {
    std::lock_guard lk(mu_);
    return last_put_headers_;
  }

  // `path` and `query` as they appear on the wire (percent-encoded); header names lowercased.
  Resp handle(std::string_view method, std::string_view path, std::string_view query,
              const std::vector<std::pair<std::string, std::string>>& headers, const std::string& body) {
    std::lock_guard lk(mu_);
    log_.push_back(std::string(method) + " " + std::string(path));
    auto hdr = [&](std::string_view name) -> std::optional<std::string> {
      for (const auto& [k, v] : headers)
        if (k == name) return v;
      return std::nullopt;
    };
    const auto raw_path = url_decode(path);
    if (!raw_path) return error(400, "InvalidURI");
    const std::string bucket_prefix = "/" + bucket_ + "/";
    if (raw_path->rfind(bucket_prefix, 0) != 0) return error(404, "NoSuchBucket");
    const std::string key = raw_path->substr(bucket_prefix.size());
    auto params = parse_query(query);
    if (!params) return error(400, "InvalidArgument");

    bool presigned = false;
    for (const auto& [k, v] : *params) presigned = presigned || k == "X-Amz-Signature";
    if (presigned) {
      if (auto err = verify_presigned(method, path, *params, hdr("host").value_or(""))) return *err;
    } else {
      if (auto err = verify_header_auth(method, path, *params, headers, hdr, body)) return *err;
    }

    if (method == "PUT") {
      last_put_headers_ = headers;
      if (!put_faults_.empty()) {
        const unsigned s = put_faults_.front();
        put_faults_.pop_front();
        if (put_fault_stores_) objects_[key] = body;
        return error(s, s == 429 ? "TooManyRequests" : "ServiceUnavailable");
      }
      objects_[key] = body;
      return {200, {{"ETag", "\"" + crypto::sha256_hex(body).substr(0, 32) + "\""}}, ""};
    }
    if (method == "GET" || method == "HEAD") {
      if (method == "GET" && !get_faults_.empty()) {
        const unsigned s = get_faults_.front();
        get_faults_.pop_front();
        return error(s, s == 429 ? "TooManyRequests" : "InternalError");
      }
      auto it = objects_.find(key);
      if (it == objects_.end()) {
        if (method == "HEAD") return {404, {}, ""};
        return error(404, "NoSuchKey");
      }
      Resp r;
      r.status = 200;
      r.body = method == "HEAD" ? std::string() : it->second;
      if (corrupt_gets_ && !r.body.empty()) r.body[0] = static_cast<char>(r.body[0] ^ 0x5a);
      std::string ct = "application/octet-stream";
      std::string cd;
      if (!ignore_overrides_) {
        for (const auto& [k, v] : *params) {
          if (k == "response-content-type") ct = v;
          if (k == "response-content-disposition") cd = v;
        }
      }
      r.headers.emplace_back("Content-Type", ct);
      if (!cd.empty()) r.headers.emplace_back("Content-Disposition", cd);
      if (method == "HEAD") r.headers.emplace_back("X-Object-Size", std::to_string(it->second.size()));
      return r;
    }
    if (method == "DELETE") {
      objects_.erase(key);
      return {204, {}, ""};
    }
    return error(405, "MethodNotAllowed");
  }

 private:
  static Resp error(unsigned status, std::string code) {
    Resp r;
    r.status = status;
    r.headers.emplace_back("Content-Type", "application/xml");
    r.body = "<?xml version=\"1.0\" encoding=\"UTF-8\"?><Error><Code>" + code + "</Code><Message>" + code +
             "</Message></Error>";
    return r;
  }

  static std::optional<storage::sigv4::KeyValues> parse_query(std::string_view q) {
    storage::sigv4::KeyValues out;
    if (q.empty()) return out;
    for (const auto& part : split(q, '&', true)) {
      const auto eq = part.find('=');
      auto k = url_decode(part.substr(0, eq));
      auto v = url_decode(eq == std::string::npos ? std::string() : part.substr(eq + 1));
      if (!k || !v) return std::nullopt;
      out.emplace_back(std::move(*k), std::move(*v));
    }
    return out;
  }

  static std::optional<int64_t> parse_amz_date(std::string_view d) {
    if (d.size() != 16) return std::nullopt;
    auto num = [&](std::size_t pos, std::size_t len) { return std::stoi(std::string(d.substr(pos, len))); };
    try {
      return utc_ms(num(0, 4), num(4, 2), num(6, 2), num(9, 2), num(11, 2), num(13, 2));
    } catch (...) {
      return std::nullopt;
    }
  }

  std::optional<Resp> verify_presigned(std::string_view method, std::string_view path,
                                       const storage::sigv4::KeyValues& params, const std::string& host) {
    namespace sv = storage::sigv4;
    std::map<std::string, std::string> p;
    sv::KeyValues signed_params;
    for (const auto& [k, v] : params) {
      p[k] = v;
      if (k != "X-Amz-Signature") signed_params.emplace_back(k, v);
    }
    if (p["X-Amz-Algorithm"] != sv::kAlgorithm) return error(400, "AuthorizationQueryParametersError");
    const std::string date = p["X-Amz-Date"];
    const auto parts = split(p["X-Amz-Credential"], '/');
    if (parts.size() != 5 || parts[0] != creds_.access_key_id || parts[1] != date.substr(0, 8) ||
        parts[2] != region_ || parts[3] != "s3" || parts[4] != "aws4_request")
      return error(403, "InvalidAccessKeyId");
    if (p["X-Amz-SignedHeaders"] != "host") return error(400, "AuthorizationQueryParametersError");
    const auto t = parse_amz_date(date);
    if (!t) return error(400, "AuthorizationQueryParametersError");
    int64_t expires = 0;
    try {
      expires = std::stoll(p["X-Amz-Expires"]);
    } catch (...) {
      return error(400, "AuthorizationQueryParametersError");
    }
    if (clock_.now_ms() > *t + expires * 1000) return error(403, "ExpiredRequest");
    const sv::KeyValues h{{"host", host}};
    const auto cr = sv::canonical_request(method, path, sv::canonical_query(signed_params), sv::canonical_headers(h),
                                          sv::kUnsignedPayload);
    const auto scope = sv::credential_scope(date.substr(0, 8), region_, "s3");
    const auto sig = sv::signature(sv::signing_key(creds_.secret_access_key, date.substr(0, 8), region_, "s3"),
                                   sv::string_to_sign(date, scope, cr));
    if (!crypto::ct_equal(sig, p["X-Amz-Signature"])) return error(403, "SignatureDoesNotMatch");
    return std::nullopt;
  }

  template <class Hdr>
  std::optional<Resp> verify_header_auth(std::string_view method, std::string_view path,
                                         const storage::sigv4::KeyValues& params,
                                         const std::vector<std::pair<std::string, std::string>>& headers,
                                         Hdr hdr, const std::string& body) {
    namespace sv = storage::sigv4;
    const auto auth = hdr("authorization");
    const auto amz_date = hdr("x-amz-date");
    const auto payload = hdr("x-amz-content-sha256");
    if (!auth || !amz_date || !payload) return error(403, "AccessDenied");
    // "AWS4-HMAC-SHA256 Credential=AK/date/region/s3/aws4_request, SignedHeaders=a;b, Signature=hex"
    const std::string prefix = std::string(sv::kAlgorithm) + " ";
    if (auth->rfind(prefix, 0) != 0) return error(400, "InvalidArgument");
    std::map<std::string, std::string> fields;
    for (auto part : split(auth->substr(prefix.size()), ',')) {
      const std::string_view pv = trim(part);
      const auto eq = pv.find('=');
      if (eq == std::string_view::npos) return error(400, "InvalidArgument");
      fields[std::string(pv.substr(0, eq))] = std::string(pv.substr(eq + 1));
    }
    const auto cred = split(fields["Credential"], '/');
    if (cred.size() != 5 || cred[0] != creds_.access_key_id || cred[1] != amz_date->substr(0, 8) ||
        cred[2] != region_ || cred[3] != "s3" || cred[4] != "aws4_request")
      return error(403, "InvalidAccessKeyId");
    sv::KeyValues signed_headers;
    bool has_host = false, has_date = false, has_sha = false;
    for (const auto& name : split(fields["SignedHeaders"], ';')) {
      has_host = has_host || name == "host";
      has_date = has_date || name == "x-amz-date";
      has_sha = has_sha || name == "x-amz-content-sha256";
      auto v = hdr(name);
      if (!v) return error(403, "SignatureDoesNotMatch");
      signed_headers.emplace_back(name, *v);
    }
    if (!has_host || !has_date || !has_sha) return error(403, "AccessDenied");
    const auto cr = sv::canonical_request(method, path, sv::canonical_query(params), sv::canonical_headers(signed_headers),
                                          *payload);
    const std::string date8 = amz_date->substr(0, 8);
    const auto sig = sv::signature(sv::signing_key(creds_.secret_access_key, date8, region_, "s3"),
                                   sv::string_to_sign(*amz_date, sv::credential_scope(date8, region_, "s3"), cr));
    if (!crypto::ct_equal(sig, fields["Signature"])) return error(403, "SignatureDoesNotMatch");
    if (method == "PUT") {
      if (*payload != crypto::sha256_hex(body)) return error(400, "XAmzContentSHA256Mismatch");
      if (auto cl = hdr("content-length"); cl && *cl != std::to_string(body.size())) return error(400, "IncompleteBody");
      if (hdr("transfer-encoding")) return error(411, "MissingContentLength");
    } else if (*payload != sv::kEmptyPayloadSha256 && *payload != sv::kUnsignedPayload) {
      return error(400, "XAmzContentSHA256Mismatch");
    }
    (void)headers;
    return std::nullopt;
  }

  mutable std::mutex mu_;
  std::string bucket_;
  storage::sigv4::Credentials creds_;
  std::string region_;
  const Clock& clock_;
  std::map<std::string, std::string> objects_;
  std::deque<unsigned> put_faults_;
  bool put_fault_stores_ = false;
  std::deque<unsigned> get_faults_;
  bool corrupt_gets_ = false;
  bool ignore_overrides_ = false;
  std::vector<std::string> log_;
  std::vector<std::pair<std::string, std::string>> last_put_headers_;
};

// net::HttpClient double routing every request to a FakeS3 (body_file / sink / max_body honoured;
// Host derived from the URL like the real client). `unreachable` simulates a dead endpoint.
class FakeS3Http final : public net::HttpClient {
 public:
  explicit FakeS3Http(FakeS3& s3) : s3_(s3) {}
  bool unreachable = false;
  std::vector<net::HttpRequest> seen;

  net::HttpResponse send(const net::HttpRequest& req) override {
    seen.push_back(req);
    if (unreachable) throw net::NetError(net::NetError::Kind::Connect, "cannot connect (fake)");
    auto u = boost::urls::parse_uri(req.url);
    if (!u) throw net::NetError(net::NetError::Kind::Protocol, "invalid URL");
    std::string host = std::string(u->encoded_host());
    const std::uint16_t dflt = to_lower_ascii(u->scheme()) == "https" ? 443 : 80;
    if (u->has_port() && u->port_number() != dflt) host += ":" + std::string(u->port());
    std::vector<std::pair<std::string, std::string>> headers{{"host", host}};
    for (const auto& [k, v] : req.headers) headers.emplace_back(to_lower_ascii(k), v);
    std::string body = req.body;
    if (req.body_file) {
      std::ifstream in(*req.body_file, std::ios::binary);
      if (!in) throw net::NetError(net::NetError::Kind::Io, "cannot open body file");
      body.assign(std::istreambuf_iterator<char>(in), {});
      headers.emplace_back("content-length", std::to_string(body.size()));
    }
    const auto r = s3_.handle(boost::beast::http::to_string(req.method), u->encoded_path(),
                              u->has_query() ? std::string_view(u->encoded_query()) : std::string_view(),
                              headers, body);
    net::HttpResponse resp;
    resp.status = r.status;
    resp.final_url = req.url;
    for (const auto& [k, v] : r.headers) resp.headers.emplace_back(to_lower_ascii(k), v);
    const bool ok = r.status >= 200 && r.status < 300;
    if (req.sink && ok) {
      if (r.body.size() > req.max_body) throw net::NetError(net::NetError::Kind::TooLarge, "too large");
      std::ofstream out(*req.sink, std::ios::binary | std::ios::trunc);
      out << r.body;
      resp.sink_sha256 = crypto::sha256_hex(r.body);
    } else {
      if (!req.sink && r.body.size() > req.max_body) throw net::NetError(net::NetError::Kind::TooLarge, "too large");
      resp.body = r.body.substr(0, req.sink ? 65536 : r.body.size());
    }
    resp.body_size = r.body.size();
    return resp;
  }

 private:
  FakeS3& s3_;
};

}  // namespace azm::test
