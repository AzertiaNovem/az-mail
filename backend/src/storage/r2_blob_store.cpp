// Owner: WP-C
// R2BlobStore : BlobStore over the S3 API (DESIGN Addendum A / A.2) and the R2 probe.
#include "storage/r2_blob_store.hpp"

#include "core/crypto.hpp"
#include "core/log.hpp"
#include "core/strings.hpp"
#include "net/http_client.hpp"
#include "storage/file_cache.hpp"

#include <boost/url.hpp>

#include <algorithm>
#include <stdexcept>
#include <system_error>
#include <thread>

namespace azm::storage {
namespace {

namespace fs = std::filesystem;
namespace bhttp = boost::beast::http;

constexpr std::size_t kMaxObjectBytes = std::size_t{4} << 30;  // GET-to-file ceiling (we store ≤ 60 MB)
constexpr std::size_t kErrorBodyLimit = 64u << 10;
constexpr std::string_view kOctetStream = "application/octet-stream";

[[noreturn]] void throw_blob(const std::string& msg, bool retryable) {
  BlobError e(msg);
  e.retryable = retryable;
  throw e;
}

// <Code>…</Code> / <Message>…</Message> of an S3 XML error document ("" when absent).
std::string xml_field(std::string_view body, std::string_view tag) {
  const std::string open = "<" + std::string(tag) + ">";
  const std::string close = "</" + std::string(tag) + ">";
  const auto b = body.find(open);
  if (b == std::string_view::npos) return {};
  const auto e = body.find(close, b + open.size());
  if (e == std::string_view::npos) return {};
  return std::string(utf8_truncate(trim(body.substr(b + open.size(), e - b - open.size())), 200));
}

bool retryable_status(unsigned s) { return s == 429 || s == 408 || s >= 500; }

// Endpoint pieces, computed like net::HttpClient derives Host so signatures match.
struct Endpoint {
  std::string scheme;       // "https" | "http"
  std::string host_header;  // host[:port] (port only when non-default)
  std::string base_path;    // "" or "/prefix" (no trailing '/')
};

Endpoint parse_endpoint(const std::string& endpoint) {
  auto parsed = boost::urls::parse_uri(endpoint);
  if (!parsed) throw std::invalid_argument("R2 endpoint is not a valid URL");
  const auto& u = *parsed;
  Endpoint ep;
  ep.scheme = to_lower_ascii(u.scheme());
  if (ep.scheme != "https" && ep.scheme != "http") throw std::invalid_argument("R2 endpoint must be http(s)");
  if (u.encoded_host().empty()) throw std::invalid_argument("R2 endpoint has no host");
  ep.host_header = std::string(u.encoded_host());
  const std::uint16_t dflt = ep.scheme == "https" ? 443 : 80;
  if (u.has_port() && !u.port().empty() && u.port_number() != dflt) ep.host_header += ":" + std::string(u.port());
  std::string path = std::string(u.path());  // decoded
  while (!path.empty() && path.back() == '/') path.pop_back();
  if (!path.empty() && path.front() != '/') path.insert(path.begin(), '/');
  ep.base_path = std::move(path);
  return ep;
}

// Request body for S3Client::send: in-memory bytes or a file streamed from disk.
struct S3Body {
  const std::string* bytes = nullptr;
  const fs::path* file = nullptr;
};

// Low-level signed S3 requests by object key (used by the store and the probe).
class S3Client {
 public:
  S3Client(R2Options opts, net::HttpClient& http, const Clock& clock)
      : opts_(std::move(opts)), ep_(parse_endpoint(opts_.endpoint)), http_(http), clock_(clock) {
    if (opts_.bucket.empty()) throw std::invalid_argument("R2 bucket is required");
  }

  const R2Options& opts() const { return opts_; }
  const Endpoint& endpoint() const { return ep_; }

  std::string raw_path(std::string_view key) const {
    return ep_.base_path + "/" + opts_.bucket + "/" + std::string(key);
  }

  // Signed request. Transport failures → BlobError (retryable per NetError kind).
  net::HttpResponse send(bhttp::verb method, std::string_view key, std::string_view payload_sha, S3Body body = {},
                         const fs::path* sink = nullptr, std::size_t max_body = kErrorBodyLimit) {
    const std::string path = raw_path(key);
    sigv4::RequestToSign rs;
    rs.method = std::string(bhttp::to_string(method));
    rs.raw_path = path;
    rs.amz_date = sigv4::amz_date(clock_.now_ms());
    rs.payload_sha256 = std::string(payload_sha);
    rs.region = opts_.region;
    rs.service = "s3";
    rs.headers = {{"host", ep_.host_header},
                  {"x-amz-content-sha256", rs.payload_sha256},
                  {"x-amz-date", rs.amz_date}};
    const auto signed_req = sigv4::sign_request(opts_.creds, rs);

    net::HttpRequest req;
    req.method = method;
    req.url = ep_.scheme + "://" + ep_.host_header + sigv4::canonical_uri(path);
    req.timeout = opts_.request_timeout;
    req.headers = {{"Authorization", signed_req.authorization},
                   {"x-amz-date", rs.amz_date},
                   {"x-amz-content-sha256", rs.payload_sha256}};
    if (body.bytes != nullptr || body.file != nullptr) req.headers.emplace_back("Content-Type", std::string(kOctetStream));
    if (body.bytes != nullptr) req.body = *body.bytes;
    if (body.file != nullptr) req.body_file = *body.file;
    if (sink != nullptr) req.sink = *sink;
    req.max_body = max_body;
    try {
      return http_.send(req);
    } catch (const net::NetError& e) {
      if (e.kind == net::NetError::Kind::TooLarge) throw_blob("blob larger than allowed", false);
      throw_blob("R2 " + rs.method + " failed: " + e.what(), e.retryable());
    }
  }

  std::string presign(std::string_view method, std::string_view key, std::chrono::seconds ttl,
                      sigv4::KeyValues extra_query) const {
    sigv4::PresignInput in;
    in.method = std::string(method);
    in.scheme = ep_.scheme;
    in.host = ep_.host_header;
    in.raw_path = raw_path(key);
    in.extra_query = std::move(extra_query);
    in.amz_date = sigv4::amz_date(clock_.now_ms());
    in.expires_sec = std::clamp<int64_t>(ttl.count(), 1, 604800);
    in.region = opts_.region;
    return sigv4::presign(opts_.creds, in).url;
  }

  // Error for a failed response; logs non-retryable (configuration) problems.
  [[noreturn]] void fail(std::string_view op, const net::HttpResponse& resp) const {
    const std::string code = xml_field(resp.body, "Code");
    const bool retryable = retryable_status(resp.status);
    std::string msg = "R2 " + std::string(op) + " failed: HTTP " + std::to_string(resp.status);
    if (!code.empty()) msg += " " + code;
    if (!retryable)
      log::error("R2 request rejected", {{"op", op}, {"status", static_cast<int64_t>(resp.status)},
                                         {"code", code}, {"bucket", opts_.bucket}});
    throw_blob(msg, retryable);
  }

  // Runs `fn` with the retry policy; BlobNotFound and non-retryable errors pass through.
  template <class F>
  auto with_retries(F&& fn) -> decltype(fn()) {
    const int attempts = std::max(1, opts_.max_attempts);
    for (int i = 1;; ++i) {
      try {
        return fn();
      } catch (const BlobError& e) {
        if (!e.retryable || i >= attempts) throw;
        log::debug("R2 retry", {{"attempt", i}, {"error", e.what()}});
        std::this_thread::sleep_for(opts_.retry_base_delay * (1 << std::min(i - 1, 10)));
      }
    }
  }

 private:
  R2Options opts_;
  Endpoint ep_;
  net::HttpClient& http_;
  const Clock& clock_;
};

class R2BlobStore final : public BlobStore {
 public:
  R2BlobStore(R2Options opts, net::HttpClient& http, const Clock& clock) : s3_(std::move(opts), http, clock) {
    std::error_code ec;
    if (!s3_.opts().tmp_dir.empty()) {
      fs::create_directories(s3_.opts().tmp_dir, ec);
      if (ec) throw_blob("cannot create tmp dir " + s3_.opts().tmp_dir.string() + ": " + ec.message(), false);
    }
    if (s3_.opts().delivery == FilesDelivery::Proxy) {
      const fs::path dir = s3_.opts().cache_dir.empty() ? s3_.opts().tmp_dir / "cache" : s3_.opts().cache_dir;
      cache_ = std::make_unique<FileCache>(dir, s3_.opts().cache_max_bytes);
    }
  }

  std::string_view kind() const override { return "r2"; }
  fs::path tmp_dir() const override { return s3_.opts().tmp_dir; }

  BlobRef put_file(const fs::path& staged, std::optional<std::string> sha256_hex) override {
    if (sha256_hex && !is_sha256_hex(*sha256_hex)) throw_blob("invalid blob sha256", false);
    std::error_code ec;
    if (!fs::is_regular_file(staged, ec)) throw_blob("staged file missing", false);
    std::string actual;
    try {
      actual = crypto::sha256_file_hex(staged);
    } catch (const std::exception& e) {
      throw_blob(std::string("cannot hash staged file: ") + e.what(), true);
    }
    if (sha256_hex && *sha256_hex != actual) throw_blob("staged file sha256 mismatch", false);
    const auto size = fs::file_size(staged, ec);
    if (ec) throw_blob("cannot stat staged file: " + ec.message(), true);
    upload(actual, S3Body{nullptr, &staged});
    fs::remove(staged, ec);
    return {actual, static_cast<int64_t>(size), "r2"};
  }

  BlobRef put_bytes(std::string_view bytes) override {
    const std::string sha = crypto::sha256_hex(bytes);
    const std::string copy(bytes);
    upload(sha, S3Body{&copy, nullptr});
    return {sha, static_cast<int64_t>(bytes.size()), "r2"};
  }

  void get_to_file(std::string_view sha, const fs::path& dest) override {
    const std::string key = object_key(sha);
    s3_.with_retries([&] {
      const auto resp = s3_.send(bhttp::verb::get, key, sigv4::kEmptyPayloadSha256, {}, &dest, kMaxObjectBytes);
      if (resp.status == 404) throw BlobNotFound("blob not found: " + std::string(sha));
      if (resp.status != 200) s3_.fail("GET", resp);
      if (resp.sink_sha256.value_or("") != sha) {
        std::error_code ec;
        fs::remove(dest, ec);
        throw_blob("R2 GET returned corrupt content for blob " + std::string(sha), true);
      }
    });
  }

  std::string get_bytes(std::string_view sha, std::size_t max_bytes) override {
    const std::string key = object_key(sha);
    return s3_.with_retries([&] {
      // The limit also bounds error documents, so never below the error-body cap.
      auto resp = s3_.send(bhttp::verb::get, key, sigv4::kEmptyPayloadSha256, {}, nullptr,
                           std::max(max_bytes, kErrorBodyLimit));
      if (resp.status == 200 && resp.body.size() > max_bytes) throw_blob("blob larger than allowed", false);
      if (resp.status == 404) throw BlobNotFound("blob not found: " + std::string(sha));
      if (resp.status != 200) s3_.fail("GET", resp);
      if (crypto::sha256_hex(resp.body) != sha)
        throw_blob("R2 GET returned corrupt content for blob " + std::string(sha), true);
      return std::move(resp.body);
    });
  }

  bool exists(std::string_view sha) override {
    const std::string key = object_key(sha);
    return s3_.with_retries([&] { return head(key); });
  }

  void remove(std::string_view sha) override {
    const std::string key = object_key(sha);
    if (cache_) cache_->erase(sha);
    s3_.with_retries([&] {
      const auto resp = s3_.send(bhttp::verb::delete_, key, sigv4::kEmptyPayloadSha256);
      if (resp.status == 200 || resp.status == 204 || resp.status == 404) return;
      s3_.fail("DELETE", resp);
    });
  }

  ServePlan serve(std::string_view sha, const ServeOptions& so) override {
    const std::string key = object_key(sha);
    if (s3_.opts().delivery == FilesDelivery::Redirect) {
      sigv4::KeyValues q;
      if (!so.content_type.empty()) q.emplace_back("response-content-type", so.content_type);
      if (!so.disposition.empty()) q.emplace_back("response-content-disposition", so.disposition);
      const auto ttl = so.ttl.count() > 0 ? so.ttl : s3_.opts().presign_ttl;
      return RedirectUrl{s3_.presign("GET", key, ttl, std::move(q))};
    }
    const fs::path p = cache_->get_or_fill(sha, [&](const fs::path& tmp) { get_to_file(sha, tmp); });
    return LocalFile{p};
  }

  std::vector<std::string> public_origins() const override {
    if (s3_.opts().delivery != FilesDelivery::Redirect) return {};
    return {s3_.endpoint().scheme + "://" + s3_.endpoint().host_header};
  }

 private:
  std::string object_key(std::string_view sha) const { return r2_object_key(s3_.opts().prefix, sha); }

  bool head(const std::string& key) {
    const auto resp = s3_.send(bhttp::verb::head, key, sigv4::kEmptyPayloadSha256);
    if (resp.status == 200) return true;
    if (resp.status == 404) return false;
    s3_.fail("HEAD", resp);
  }

  // HEAD-before-PUT (idempotent, content-addressed). Every retry re-HEADs first: after a 429
  // (1 write/s per key) a concurrent writer of the same blob may already have stored it.
  void upload(const std::string& sha, S3Body body) {
    const std::string key = object_key(sha);
    s3_.with_retries([&] {
      if (head(key)) return;
      const auto resp = s3_.send(bhttp::verb::put, key, sha, body);
      if (resp.status == 200 || resp.status == 201 || resp.status == 204) return;
      s3_.fail("PUT", resp);
    });
  }

  S3Client s3_;
  std::unique_ptr<FileCache> cache_;
};

std::string normalized_prefix(std::string prefix) {
  while (!prefix.empty() && prefix.front() == '/') prefix.erase(prefix.begin());
  if (!prefix.empty() && prefix.back() != '/') prefix.push_back('/');
  return prefix;
}

}  // namespace

R2Options r2_options_from(const Config& cfg) {
  R2Options o;
  o.endpoint = cfg.effective_r2_endpoint();
  if (o.endpoint.empty()) throw std::invalid_argument("R2 needs R2_ENDPOINT or R2_ACCOUNT_ID");
  o.bucket = cfg.r2_bucket;
  if (o.bucket.empty()) throw std::invalid_argument("R2 needs R2_BUCKET");
  o.creds.access_key_id = cfg.r2_access_key_id;
  o.creds.secret_access_key = cfg.r2_secret_access_key;
  if (o.creds.access_key_id.empty() || o.creds.secret_access_key.empty())
    throw std::invalid_argument("R2 needs R2_ACCESS_KEY_ID and R2_SECRET_ACCESS_KEY");
  o.prefix = normalized_prefix(cfg.r2_prefix);
  o.delivery = cfg.files_delivery;
  o.presign_ttl = std::chrono::seconds(std::clamp(cfg.r2_presign_ttl_sec, 1, 604800));
  o.tmp_dir = fs::path(cfg.data_dir) / "tmp";
  o.cache_dir = fs::path(cfg.data_dir) / "cache";
  o.cache_max_bytes = static_cast<std::uint64_t>(cfg.file_cache_mb) << 20;
  return o;
}

std::unique_ptr<azm::BlobStore> make_r2_blob_store(const azm::Config& cfg, azm::net::HttpClient& http) {
  return make_r2_blob_store(r2_options_from(cfg), http, system_clock());
}

std::unique_ptr<azm::BlobStore> make_r2_blob_store(R2Options opts, azm::net::HttpClient& http, const Clock& clock) {
  opts.prefix = normalized_prefix(std::move(opts.prefix));
  return std::make_unique<R2BlobStore>(std::move(opts), http, clock);
}

std::string r2_object_key(std::string_view prefix, std::string_view sha256) {
  return std::string(prefix) + blob_relative_key(sha256);
}

R2ProbeReport probe_r2(const Config& cfg, net::HttpClient& http, bool write_test, bool check_presign_overrides) {
  R2ProbeReport rep;
  R2Options opts;
  try {
    opts = r2_options_from(cfg);
    opts.max_attempts = 1;
    opts.request_timeout = std::chrono::seconds(15);
  } catch (const std::exception& e) {
    rep.detail = e.what();
    return rep;
  }
  std::optional<S3Client> s3;
  try {
    s3.emplace(opts, http, system_clock());
  } catch (const std::exception& e) {
    rep.detail = e.what();
    return rep;
  }
  const std::string where = "bucket " + opts.bucket + " at " + s3->endpoint().host_header;

  // 1. Reachability + bucket: GET of a key that never exists.
  net::HttpResponse resp;
  try {
    resp = s3->send(bhttp::verb::get, opts.prefix + "probe/.azmail-missing", sigv4::kEmptyPayloadSha256);
  } catch (const std::exception& e) {
    rep.detail = "R2 unreachable: " + std::string(e.what());
    return rep;
  }
  rep.reachable = true;
  const std::string code = xml_field(resp.body, "Code");
  if (resp.status == 404 && code != "NoSuchBucket") {
    rep.bucket_ok = true;
  } else if (resp.status == 200) {
    rep.bucket_ok = true;
  } else if (resp.status == 404) {
    rep.detail = where + ": NoSuchBucket";
    return rep;
  } else if (resp.status == 403 || resp.status == 401) {
    rep.detail = where + ": access denied (" + (code.empty() ? "HTTP " + std::to_string(resp.status) : code) +
                 ") — check R2_ACCESS_KEY_ID / R2_SECRET_ACCESS_KEY";
    return rep;
  } else {
    rep.detail = where + ": unexpected HTTP " + std::to_string(resp.status) + (code.empty() ? "" : " " + code);
    return rep;
  }
  if (!write_test && !check_presign_overrides) {
    rep.detail = where + ": ok";
    return rep;
  }

  // 2. A tiny probe object: PUT → GET (→ presigned GET with overrides) → DELETE.
  const std::string key = opts.prefix + "probe/azmail-probe-" + crypto::hex_encode(crypto::random_bytes(8));
  const std::string content = "azmail r2 probe";
  const std::string sha = crypto::sha256_hex(content);
  std::vector<std::string> notes;
  bool stored = false;
  try {
    const auto put = s3->send(bhttp::verb::put, key, sha, S3Body{&content, nullptr});
    stored = put.status == 200 || put.status == 201 || put.status == 204;
    if (!stored) notes.push_back("PUT HTTP " + std::to_string(put.status) + " " + xml_field(put.body, "Code"));
    bool read_ok = false;
    if (stored) {
      const auto get = s3->send(bhttp::verb::get, key, sigv4::kEmptyPayloadSha256, {}, nullptr, 1 << 20);
      read_ok = get.status == 200 && get.body == content;
      if (!read_ok) notes.push_back("GET of the probe object did not return it");
    }
    if (check_presign_overrides) {
      bool ok = false;
      if (stored) {
        const std::string ct = "text/plain; charset=utf-8";
        const std::string cd = "attachment; filename=\"azmail-probe.txt\"";
        const std::string url = s3->presign("GET", key, std::chrono::seconds(60),
                                            {{"response-content-type", ct}, {"response-content-disposition", cd}});
        net::HttpRequest preq;
        preq.url = url;
        preq.timeout = std::chrono::seconds(15);
        preq.max_body = 1 << 20;
        const auto presp = http.send(preq);
        ok = presp.status == 200 && presp.body == content && presp.header("content-type") == ct &&
             presp.header("content-disposition") == cd;
        if (!ok) notes.push_back("presigned GET ignored response-content-* overrides");
      }
      rep.presign_overrides_ok = ok;
    }
    if (stored) {
      const auto del = s3->send(bhttp::verb::delete_, key, sigv4::kEmptyPayloadSha256);
      if (del.status != 200 && del.status != 204 && del.status != 404) {
        notes.push_back("DELETE HTTP " + std::to_string(del.status));
        read_ok = false;
      }
    }
    if (write_test) rep.write_ok = stored && read_ok;
  } catch (const std::exception& e) {
    notes.push_back(e.what());
    if (check_presign_overrides && !rep.presign_overrides_ok) rep.presign_overrides_ok = false;
  }
  rep.detail = where + ": " + (notes.empty() ? std::string("ok") : join(notes, "; "));
  return rep;
}

}  // namespace azm::storage
