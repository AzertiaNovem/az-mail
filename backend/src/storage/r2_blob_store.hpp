// Owner: WP-C
//
// R2BlobStore : BlobStore over the S3 API (DESIGN Addendum A / A.2), via net::HttpClient and
// storage::sigv4. Object key = <prefix>blobs/<aa>/<bb>/<sha256>, path-style URL
// <endpoint>/<bucket>/<key>, region "auto", service "s3". Objects are immutable and PUT with
// Content-Type: application/octet-stream, Content-Length and x-amz-content-sha256 = the blob sha
// (no chunked signing). Behaviour per BlobStore method:
//   put_file / put_bytes: HEAD first; if present → no upload (idempotent). PUT streams the
//     staged file (HttpRequest::body_file); the temp file is deleted on success. 429
//     TooManyRequests (1 write/s per key) → re-HEAD; present = success.
//   get_to_file / get_bytes: GET (sink for files); 404 → BlobNotFound.
//   exists: HEAD (200 → true, 404 → false). remove: DELETE (404 = no-op); also evicts the cache.
//   serve: FilesDelivery::Redirect → RedirectUrl (presigned GET, ServeOptions::ttl, with
//     response-content-type / response-content-disposition overrides); FilesDelivery::Proxy →
//     LocalFile from the FileCache (filled with get_to_file on a miss).
//   public_origins: {"<scheme>://<endpoint host>"} in redirect mode, {} in proxy mode.
// Errors: 5xx / 429 / network → BlobError{retryable=true}; 400 / 403 → BlobError{retryable=false}
// (config problem: logged with the S3 error Code, never credentials or signed URLs).
// Blocking; never call inside a DB transaction. Thread-safe.
#pragma once

#include "config.hpp"
#include "core/blob_store.hpp"
#include "core/time.hpp"
#include "storage/s3_sigv4.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace azm::net {
class HttpClient;
}

namespace azm::storage {

struct R2Options {
  std::string endpoint;            // cfg.effective_r2_endpoint(), e.g. "https://<acct>.r2.cloudflarestorage.com"
  std::string bucket;              // cfg.r2_bucket
  std::string prefix = "azmail/";  // cfg.r2_prefix (normalized to end with '/', or empty)
  sigv4::Credentials creds;        // cfg.r2_access_key_id / cfg.r2_secret_access_key
  std::string region = "auto";
  FilesDelivery delivery = FilesDelivery::Proxy;  // cfg.files_delivery
  std::chrono::seconds presign_ttl{300};          // cfg.r2_presign_ttl_sec (default for serve())
  std::filesystem::path tmp_dir;                  // <cfg.data_dir>/tmp (staging; tmp_dir())
  std::filesystem::path cache_dir;                // <cfg.data_dir>/cache (proxy mode)
  std::uint64_t cache_max_bytes = 1024ull << 20;  // cfg.file_cache_mb MiB
  std::chrono::milliseconds request_timeout{120000};  // per object request (50 MB at ~0.5 MB/s)
};

// Options from Config. Throws std::invalid_argument when bucket, endpoint or credentials are
// missing (validate_config reports the same earlier).
R2Options r2_options_from(const Config& cfg);

// Addendum A factory (WP-A wiring for AZMAIL_BLOB_BACKEND=r2).
std::unique_ptr<azm::BlobStore> make_r2_blob_store(const azm::Config&, azm::net::HttpClient&);
// Explicit-options variant (tests with a fake HttpClient; `clock` drives amz dates / presign).
std::unique_ptr<azm::BlobStore> make_r2_blob_store(R2Options opts, azm::net::HttpClient& http,
                                                   const Clock& clock = system_clock());

// "<prefix>blobs/aa/bb/<sha256>" (throws BlobError{retryable=false} for an invalid sha).
std::string r2_object_key(std::string_view prefix, std::string_view sha256);

// Startup / `azmail doctor [--r2]` probe.
struct R2ProbeReport {
  bool reachable = false;   // the endpoint answered HTTP at all
  bool bucket_ok = false;   // GET of a missing probe key gave 404 NoSuchKey (not NoSuchBucket/403)
  bool write_ok = false;    // PUT + GET + DELETE of a tiny probe object (only when write_test)
  std::optional<bool> presign_overrides_ok;  // presigned GET honoured response-content-* (when checked)
  std::string detail;       // human-readable summary (no secrets, no signed query strings)
  bool ok() const { return reachable && bucket_ok; }
};
R2ProbeReport probe_r2(const Config& cfg, net::HttpClient& http, bool write_test,
                       bool check_presign_overrides);

}  // namespace azm::storage
