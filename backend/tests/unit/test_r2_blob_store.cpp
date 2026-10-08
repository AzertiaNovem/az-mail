// Owner: WP-C — R2BlobStore against a fake S3 that verifies SigV4 (header + presigned auth),
// via a transport double and end-to-end over HTTP; R2 probe; options.
#include "../fakes/fake_http_server.hpp"
#include "../fakes/fake_s3.hpp"
#include "config.hpp"
#include "core/crypto.hpp"
#include "core/strings.hpp"
#include "net/http_client.hpp"
#include "storage/r2_blob_store.hpp"
#include "test_support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <fstream>

using namespace azm;
using namespace azm::storage;
using Catch::Matchers::ContainsSubstring;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

const sigv4::Credentials kCreds{"r2-access-key", "r2-secret-key-never-logged"};

R2Options options(const test::TempDir& td, FilesDelivery delivery = FilesDelivery::Proxy) {
  R2Options o;
  o.endpoint = "https://acct123.r2.cloudflarestorage.com";
  o.bucket = "mail-bucket";
  o.prefix = "azmail/";
  o.creds = kCreds;
  o.delivery = delivery;
  o.tmp_dir = td / "tmp";
  o.cache_dir = td / "cache";
  o.cache_max_bytes = 1 << 20;
  o.retry_base_delay = 1ms;
  return o;
}

void write_file(const fs::path& p, const std::string& data) {
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  out << data;
}
std::string read_file(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), {});
}

struct Fixture {
  test::TempDir td;
  ManualClock clock{*utc_ms(2026, 10, 7, 12, 0, 0)};
  test::FakeS3 s3{"mail-bucket", kCreds, clock};
  test::FakeS3Http http{s3};
  std::unique_ptr<BlobStore> store;
  explicit Fixture(FilesDelivery d = FilesDelivery::Proxy) { store = make_r2_blob_store(options(td, d), http, clock); }
  std::string key(const std::string& sha) const { return r2_object_key("azmail/", sha); }
};

BlobError blob_error(const std::function<void()>& f) {
  try {
    f();
  } catch (const BlobError& e) {
    return e;
  }
  FAIL("expected BlobError");
  return BlobError("unreachable");
}

}  // namespace

TEST_CASE("r2: object keys and options", "[r2]") {
  const std::string sha = crypto::sha256_hex("x");
  CHECK(r2_object_key("azmail/", sha) == "azmail/blobs/" + sha.substr(0, 2) + "/" + sha.substr(2, 2) + "/" + sha);
  CHECK(r2_object_key("", sha) == "blobs/" + sha.substr(0, 2) + "/" + sha.substr(2, 2) + "/" + sha);
  CHECK_THROWS_AS(r2_object_key("azmail/", "../../etc"), BlobError);

  Config cfg;
  cfg.r2_account_id = "acct";
  cfg.r2_bucket = "b";
  cfg.r2_access_key_id = "ak";
  cfg.r2_secret_access_key = "sk";
  cfg.r2_prefix = "/team";
  cfg.data_dir = "/var/lib/azmail";
  cfg.file_cache_mb = 3;
  cfg.r2_presign_ttl_sec = 120;
  const auto o = r2_options_from(cfg);
  CHECK(o.endpoint == "https://acct.r2.cloudflarestorage.com");
  CHECK(o.prefix == "team/");
  CHECK(o.region == "auto");
  CHECK(o.delivery == FilesDelivery::Proxy);
  CHECK(o.presign_ttl == 120s);
  CHECK(o.tmp_dir == fs::path("/var/lib/azmail/tmp"));
  CHECK(o.cache_dir == fs::path("/var/lib/azmail/cache"));
  CHECK(o.cache_max_bytes == (3u << 20));
  cfg.r2_endpoint = "http://127.0.0.1:9000";
  CHECK(r2_options_from(cfg).endpoint == "http://127.0.0.1:9000");
  cfg.r2_prefix = "";
  CHECK(r2_options_from(cfg).prefix.empty());

  Config missing = cfg;
  missing.r2_bucket.clear();
  CHECK_THROWS_AS(r2_options_from(missing), std::invalid_argument);
  missing = cfg;
  missing.r2_secret_access_key.clear();
  CHECK_THROWS_AS(r2_options_from(missing), std::invalid_argument);
  missing = cfg;
  missing.r2_endpoint.clear();
  missing.r2_account_id.clear();
  CHECK_THROWS_AS(r2_options_from(missing), std::invalid_argument);

  test::TempDir td;
  test::FakeS3 s3{"b", kCreds, system_clock()};
  test::FakeS3Http http{s3};
  auto bad = options(td);
  bad.endpoint = "ftp://x";
  CHECK_THROWS_AS(make_r2_blob_store(bad, http), std::invalid_argument);
}

TEST_CASE("r2: put/get/exists/remove round trip with signed requests", "[r2]") {
  Fixture f;
  CHECK(f.store->kind() == "r2");
  CHECK(f.store->tmp_dir() == f.td / "tmp");
  CHECK(fs::is_directory(f.td / "tmp"));

  const std::string data = "hello r2 \xe4\xb8\xad";
  const auto ref = f.store->put_bytes(data);
  CHECK(ref.sha256 == crypto::sha256_hex(data));
  CHECK(ref.size == static_cast<int64_t>(data.size()));
  CHECK(ref.storage == "r2");
  CHECK(f.s3.object(f.key(ref.sha256)) == std::optional<std::string>(data));
  CHECK(f.s3.count("HEAD") == 1);
  CHECK(f.s3.count("PUT") == 1);

  // PUT headers: real sha256, octet-stream, signed date; path-style URL.
  bool ct = false, sha = false;
  for (const auto& [k, v] : f.s3.last_put_headers()) {
    ct = ct || (k == "content-type" && v == "application/octet-stream");
    sha = sha || (k == "x-amz-content-sha256" && v == ref.sha256);
  }
  CHECK(ct);
  CHECK(sha);
  CHECK(f.http.seen.back().url == "https://acct123.r2.cloudflarestorage.com/mail-bucket/" + f.key(ref.sha256));

  // Existing content: HEAD only, no second PUT (content-addressed dedupe).
  CHECK(f.store->put_bytes(data).sha256 == ref.sha256);
  CHECK(f.s3.count("PUT") == 1);

  CHECK(f.store->exists(ref.sha256));
  CHECK(f.store->get_bytes(ref.sha256, 1000) == data);
  f.store->get_to_file(ref.sha256, f.td / "out.bin");
  CHECK(read_file(f.td / "out.bin") == data);

  f.store->remove(ref.sha256);
  CHECK_FALSE(f.store->exists(ref.sha256));
  CHECK(f.s3.object_count() == 0);
  CHECK_NOTHROW(f.store->remove(ref.sha256));  // missing = no-op
  CHECK_THROWS_AS(f.store->get_bytes(ref.sha256, 1000), BlobNotFound);
  CHECK_THROWS_AS(f.store->get_to_file(ref.sha256, f.td / "out2.bin"), BlobNotFound);
  CHECK_FALSE(fs::exists(f.td / "out2.bin"));
}

TEST_CASE("r2: put_file streams the staged file and deletes it", "[r2]") {
  Fixture f;
  const auto staged = make_staging_path(f.store->tmp_dir());
  std::string content(70000, 'x');
  content[123] = 'y';
  write_file(staged, content);
  const std::string sha = crypto::sha256_hex(content);
  const auto ref = f.store->put_file(staged, sha);
  CHECK(ref.sha256 == sha);
  CHECK(ref.size == 70000);
  CHECK_FALSE(fs::exists(staged));
  CHECK(f.s3.object(f.key(sha)) == std::optional<std::string>(content));
  REQUIRE(f.http.seen.back().body_file.has_value());  // streamed, not buffered

  // Computes the sha when absent; an existing object is not re-uploaded.
  write_file(staged, content);
  CHECK(f.store->put_file(staged).sha256 == sha);
  CHECK(f.s3.count("PUT") == 1);
  CHECK_FALSE(fs::exists(staged));

  write_file(staged, "other");
  auto e = blob_error([&] { (void)f.store->put_file(staged, sha); });
  CHECK_FALSE(e.retryable);
  CHECK(fs::exists(staged));  // kept for the caller to clean up
  e = blob_error([&] { (void)f.store->put_file(f.td / "missing.part", std::nullopt); });
  CHECK_FALSE(e.retryable);
  e = blob_error([&] { (void)f.store->put_file(staged, std::string("NOT-A-SHA")); });
  CHECK_FALSE(e.retryable);
}

TEST_CASE("r2: 429 per-key write limit → re-HEAD; present counts as success", "[r2]") {
  Fixture f;
  // A concurrent writer stored the object while our PUT got 429.
  f.s3.fail_next_puts({429}, /*store_anyway=*/true);
  const auto ref = f.store->put_bytes("concurrent");
  CHECK(f.s3.object(f.key(ref.sha256)).has_value());
  CHECK(f.s3.count("PUT") == 1);
  CHECK(f.s3.count("HEAD") == 2);

  // Transient 429 / 503 without the object: retried until it succeeds.
  f.s3.fail_next_puts({429, 503});
  const auto r2 = f.store->put_bytes("retried");
  CHECK(f.s3.object(f.key(r2.sha256)) == std::optional<std::string>("retried"));
  CHECK(f.s3.count("PUT") == 4);

  // Persistent failure: retryable BlobError after max_attempts.
  f.s3.fail_next_puts({503, 503, 503, 503, 503});
  auto e = blob_error([&] { (void)f.store->put_bytes("never"); });
  CHECK(e.retryable);
  CHECK_THAT(std::string(e.what()), ContainsSubstring("503"));
}

TEST_CASE("r2: GET retries 5xx; corrupt content is detected", "[r2]") {
  Fixture f;
  const auto ref = f.store->put_bytes("payload");
  f.s3.fail_next_gets({500, 503});
  CHECK(f.store->get_bytes(ref.sha256, 100) == "payload");
  f.s3.set_corrupt_gets(true);
  auto e = blob_error([&] { (void)f.store->get_bytes(ref.sha256, 100); });
  CHECK(e.retryable);
  CHECK_THAT(std::string(e.what()), ContainsSubstring("corrupt"));
  e = blob_error([&] { f.store->get_to_file(ref.sha256, f.td / "c.bin"); });
  CHECK(e.retryable);
  CHECK_FALSE(fs::exists(f.td / "c.bin"));
  f.s3.set_corrupt_gets(false);
  e = blob_error([&] { (void)f.store->get_bytes(ref.sha256, 3); });  // over max_bytes
  CHECK_FALSE(e.retryable);
}

TEST_CASE("r2: wrong credentials are non-retryable and never leak the secret", "[r2]") {
  test::TempDir td;
  ManualClock clock{*utc_ms(2026, 10, 7, 12, 0, 0)};
  test::FakeS3 s3{"mail-bucket", {"r2-access-key", "a-different-secret"}, clock};
  test::FakeS3Http http{s3};
  auto store = make_r2_blob_store(options(td), http, clock);
  const auto e = blob_error([&] { (void)store->put_bytes("x"); });
  CHECK_FALSE(e.retryable);
  CHECK_THAT(std::string(e.what()), ContainsSubstring("SignatureDoesNotMatch"));
  CHECK_THAT(std::string(e.what()), !ContainsSubstring("r2-secret-key-never-logged"));
  CHECK(s3.count("HEAD") == 1);  // not retried

  // Network failures are retryable.
  http.unreachable = true;
  const auto n = blob_error([&] { (void)store->exists(crypto::sha256_hex("x")); });
  CHECK(n.retryable);
}

TEST_CASE("r2: redirect delivery presigns GET with response overrides", "[r2]") {
  Fixture f(FilesDelivery::Redirect);
  CHECK(f.store->public_origins() == std::vector<std::string>{"https://acct123.r2.cloudflarestorage.com"});
  const auto ref = f.store->put_bytes("%PDF-1.7 fake");
  ServeOptions so;
  so.content_type = "application/pdf";
  so.disposition = content_disposition("inline", "报告.pdf");
  so.ttl = 300s;
  const auto plan = f.store->serve(ref.sha256, so);
  REQUIRE(std::holds_alternative<RedirectUrl>(plan));
  const std::string url = std::get<RedirectUrl>(plan).url;
  CHECK(url.rfind("https://acct123.r2.cloudflarestorage.com/mail-bucket/azmail/blobs/", 0) == 0);
  CHECK_THAT(url, ContainsSubstring("X-Amz-Expires=300"));
  CHECK_THAT(url, ContainsSubstring("response-content-type=application%2Fpdf"));
  CHECK_THAT(url, ContainsSubstring("response-content-disposition="));
  CHECK_THAT(url, ContainsSubstring("%2Fauto%2Fs3%2Faws4_request"));

  // The fake verifies the query signature and applies the overrides (no Authorization header).
  net::HttpRequest get;
  get.url = url;
  auto resp = f.http.send(get);
  CHECK(resp.status == 200);
  CHECK(resp.body == "%PDF-1.7 fake");
  CHECK(resp.header("content-type") == std::optional<std::string>("application/pdf"));
  CHECK(resp.header("content-disposition") == std::optional<std::string>(so.disposition));

  // Tampering or expiry is rejected.
  std::string tampered = url;
  tampered.replace(tampered.find("application%2Fpdf"), 17, "text%2Fhtml%3B%20x");
  get.url = tampered;
  CHECK(f.http.send(get).status == 403);
  f.clock.advance(301'000);
  get.url = url;
  resp = f.http.send(get);
  CHECK(resp.status == 403);
  CHECK_THAT(resp.body, ContainsSubstring("ExpiredRequest"));

  // ttl 0 falls back to the configured presign TTL (300 s default).
  so.ttl = 0s;
  CHECK_THAT(std::get<RedirectUrl>(f.store->serve(ref.sha256, so)).url, ContainsSubstring("X-Amz-Expires=300"));
}

TEST_CASE("r2: proxy delivery serves through the file cache", "[r2]") {
  Fixture f(FilesDelivery::Proxy);
  CHECK(f.store->public_origins().empty());
  const auto ref = f.store->put_bytes("cached bytes");
  const int gets_before = f.s3.count("GET");
  auto plan = f.store->serve(ref.sha256, {});
  REQUIRE(std::holds_alternative<LocalFile>(plan));
  const auto path = std::get<LocalFile>(plan).path;
  CHECK(path.parent_path() == f.td / "cache");
  CHECK(read_file(path) == "cached bytes");
  plan = f.store->serve(ref.sha256, {});
  CHECK(std::get<LocalFile>(plan).path == path);
  CHECK(f.s3.count("GET") == gets_before + 1);  // second serve is a cache hit

  f.store->remove(ref.sha256);  // evicts the cache entry too
  CHECK_FALSE(fs::exists(path));
  CHECK_THROWS_AS(f.store->serve(ref.sha256, {}), BlobNotFound);
}

TEST_CASE("r2: end-to-end over HTTP with streamed upload/download and SigV4 checks",
          "[r2][http_client]") {
  test::TempDir td;
  const sigv4::Credentials creds{"e2e-key", "e2e-secret"};
  test::FakeS3 s3{"e2e-bucket", creds, system_clock()};
  test::FakeHttpServer srv([&](const test::FakeRequest& req) {
    const auto r = s3.handle(req.method, req.path, req.query, req.headers, req.body);
    test::FakeResponse out;
    out.status = r.status;
    out.headers = r.headers;
    out.body = r.body;
    return out;
  });
  net::ClientOptions co;
  co.allow_insecure_http = true;
  net::HttpClient http(co);
  R2Options o = options(td, FilesDelivery::Redirect);
  o.endpoint = srv.base_url();  // http://127.0.0.1:<port> (non-default port is part of Host)
  o.bucket = "e2e-bucket";
  o.creds = creds;
  o.prefix = "p";  // normalized to "p/"
  auto store = make_r2_blob_store(o, http);

  std::string content(256 * 1024 + 17, '\0');
  for (std::size_t i = 0; i < content.size(); ++i) content[i] = static_cast<char>((i * 7) & 0xff);
  const auto staged = make_staging_path(store->tmp_dir());
  write_file(staged, content);
  const auto ref = store->put_file(staged);
  CHECK(ref.sha256 == crypto::sha256_hex(content));
  CHECK(s3.object(r2_object_key("p/", ref.sha256)) == std::optional<std::string>(content));
  CHECK(store->exists(ref.sha256));
  store->get_to_file(ref.sha256, td / "back.bin");
  CHECK(read_file(td / "back.bin") == content);
  CHECK(store->get_bytes(ref.sha256, content.size()) == content);

  ServeOptions so;
  so.content_type = "image/png";
  so.disposition = "inline";
  const auto url = std::get<RedirectUrl>(store->serve(ref.sha256, so)).url;
  net::HttpRequest get;
  get.url = url;
  get.max_body = 1 << 20;
  const auto resp = http.send(get);
  CHECK(resp.status == 200);
  CHECK(resp.body == content);
  CHECK(resp.header("content-type") == std::optional<std::string>("image/png"));
  CHECK(store->public_origins() == std::vector<std::string>{srv.base_url()});
  store->remove(ref.sha256);
  CHECK_FALSE(store->exists(ref.sha256));
  for (const auto& line : s3.log()) CHECK(line.find("SignatureDoesNotMatch") == std::string::npos);
}

TEST_CASE("r2: probe reports reachability, bucket, write and presign checks", "[r2][probe]") {
  ManualClock clock{azm::now_ms()};
  test::FakeS3 s3{"probe-bucket", kCreds, system_clock()};
  test::FakeS3Http http{s3};
  Config cfg;
  cfg.r2_endpoint = "https://acct.r2.cloudflarestorage.com";
  cfg.r2_bucket = "probe-bucket";
  cfg.r2_access_key_id = kCreds.access_key_id;
  cfg.r2_secret_access_key = kCreds.secret_access_key;

  auto rep = probe_r2(cfg, http, false, false);
  CHECK(rep.ok());
  CHECK(rep.reachable);
  CHECK(rep.bucket_ok);
  CHECK_FALSE(rep.write_ok);
  CHECK_FALSE(rep.presign_overrides_ok.has_value());

  rep = probe_r2(cfg, http, true, true);
  CHECK(rep.ok());
  CHECK(rep.write_ok);
  CHECK(rep.presign_overrides_ok == std::optional<bool>(true));
  CHECK(s3.object_count() == 0);  // probe object deleted
  CHECK_THAT(rep.detail, !ContainsSubstring(kCreds.secret_access_key));

  s3.set_ignore_response_overrides(true);
  rep = probe_r2(cfg, http, false, true);
  CHECK(rep.ok());
  CHECK(rep.presign_overrides_ok == std::optional<bool>(false));
  CHECK_THAT(rep.detail, ContainsSubstring("overrides"));
  s3.set_ignore_response_overrides(false);

  Config wrong_bucket = cfg;
  wrong_bucket.r2_bucket = "other-bucket";
  rep = probe_r2(wrong_bucket, http, false, false);
  CHECK(rep.reachable);
  CHECK_FALSE(rep.bucket_ok);
  CHECK_THAT(rep.detail, ContainsSubstring("NoSuchBucket"));

  Config wrong_key = cfg;
  wrong_key.r2_secret_access_key = "nope";
  rep = probe_r2(wrong_key, http, false, false);
  CHECK(rep.reachable);
  CHECK_FALSE(rep.ok());
  CHECK_THAT(rep.detail, ContainsSubstring("access denied"));

  http.unreachable = true;
  rep = probe_r2(cfg, http, false, false);
  CHECK_FALSE(rep.reachable);
  CHECK_FALSE(rep.ok());

  Config incomplete;
  rep = probe_r2(incomplete, http, false, false);
  CHECK_FALSE(rep.ok());
  CHECK_FALSE(rep.detail.empty());
}

TEST_CASE("r2: objects over 64 KiB dedupe and exist over real HTTP (RT-1)", "[r2][http_client]") {
  test::TempDir td;
  const sigv4::Credentials creds{"e2e-key", "e2e-secret"};
  test::FakeS3 s3{"big-bucket", creds, system_clock()};
  // Like real S3/R2 (and the mock): HEAD of an object reports its size in Content-Length.
  test::FakeHttpServer srv([&](const test::FakeRequest& req) {
    const auto r = s3.handle(req.method, req.path, req.query, req.headers, req.body);
    test::FakeResponse out;
    out.status = r.status;
    out.headers = r.headers;
    out.body = r.body;
    if (req.method == "HEAD" && r.status == 200) {
      const std::string prefix = "/big-bucket/";
      if (auto obj = s3.object(req.path.substr(prefix.size()))) out.body = *obj;  // header only on the wire
    }
    return out;
  });
  net::ClientOptions co;
  co.allow_insecure_http = true;
  net::HttpClient http(co);
  R2Options o = options(td);
  o.endpoint = srv.base_url();
  o.bucket = "big-bucket";
  o.creds = creds;
  auto store = make_r2_blob_store(o, http);

  const std::string big(100 * 1024, 'b');
  const auto ref = store->put_bytes(big);
  CHECK(store->exists(ref.sha256));
  CHECK(store->put_bytes(big).sha256 == ref.sha256);  // HEAD-before-PUT dedupe of an existing object
  CHECK(s3.count("PUT") == 1);
  const auto staged = make_staging_path(store->tmp_dir());
  write_file(staged, big);
  CHECK(store->put_file(staged, ref.sha256).sha256 == ref.sha256);
  CHECK_FALSE(store->exists(std::string(64, 'e')));
}
