// Content-addressed blob storage (DESIGN Addendum A; frozen interface).
// All calls are blocking disk/network I/O: never call them inside a DB transaction. Register
// `blobs` rows (INSERT OR IGNORE) only after put_file/put_bytes succeeded.
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace azm {

struct BlobRef {
  std::string sha256;
  int64_t size = 0;
  std::string storage;  // storage = "local"|"r2"
};
struct LocalFile {
  std::filesystem::path path;
};
struct RedirectUrl {
  std::string url;  // presigned GET
};
using ServePlan = std::variant<LocalFile, RedirectUrl>;
struct ServeOptions {
  std::string content_type;
  std::string disposition;  // full Content-Disposition value
  std::chrono::seconds ttl{300};
};

class BlobStore {
 public:
  virtual ~BlobStore() = default;
  virtual std::string_view kind() const = 0;                  // "local" | "r2"
  virtual std::filesystem::path tmp_dir() const = 0;          // local scratch for staging
  // Upload/move a staged temp file (caller already streamed it). Computes sha if absent. Idempotent:
  // an existing blob is not re-uploaded (HEAD first for r2). Deletes the temp file on success.
  virtual BlobRef put_file(const std::filesystem::path& staged,
                           std::optional<std::string> sha256_hex = {}) = 0;
  virtual BlobRef put_bytes(std::string_view bytes) = 0;
  virtual void get_to_file(std::string_view sha256,
                           const std::filesystem::path& dest) = 0;  // throws BlobNotFound
  virtual std::string get_bytes(std::string_view sha256, std::size_t max_bytes) = 0;
  virtual bool exists(std::string_view sha256) = 0;
  virtual void remove(std::string_view sha256) = 0;  // missing = no-op
  // How to deliver a blob to a browser. Local → LocalFile. R2+redirect → presigned GET with
  // response-content-type / response-content-disposition overrides. R2+proxy → LocalFile from file_cache.
  virtual ServePlan serve(std::string_view sha256, const ServeOptions&) = 0;
  // Origins that serve file bytes to browsers (for iframe/img CSP), e.g. "https://<acct>.r2.cloudflarestorage.com".
  virtual std::vector<std::string> public_origins() const = 0;
};

struct BlobNotFound : std::runtime_error {
  using runtime_error::runtime_error;
};
struct BlobError : std::runtime_error {
  using runtime_error::runtime_error;
  bool retryable = true;
};

// LocalBlobStore: <data_dir>/blobs/aa/bb/<sha256>, staging in <data_dir>/tmp (both created).
// Thread-safe. Invalid sha strings (not 64 lowercase hex) throw BlobError{retryable=false}.
std::unique_ptr<BlobStore> make_local_blob_store(std::filesystem::path data_dir);

// ---- shared helpers (additive) ---------------------------------------------------------------
// True for exactly 64 lowercase hex characters (prevents path traversal via sha strings).
bool is_sha256_hex(std::string_view s);
// "blobs/aa/bb/<sha>" — relative object key / path used by every backend.
// Throws BlobError{retryable=false} for an invalid sha.
std::string blob_relative_key(std::string_view sha256);
// Unique "<tmp_dir>/<random>.part" path for staging (the file is not created).
std::filesystem::path make_staging_path(const std::filesystem::path& tmp_dir);

// ---- writer / GC serialization (in-process) ----------------------------------------------------
// put_file / put_bytes skip the upload when the object already exists, so without this guard
// gc.blobs could delete an object that a concurrent writer has just "stored" and is about to
// reference (repeated content: logos, signature images). Writers hold the shared guard from
// their first put_file / put_bytes until the transaction that registers AND references the
// blob has committed; gc.blobs holds the exclusive guard per blob across
// re-check → BlobStore::remove → row delete. Rules: take it outside any transaction, never
// nest or re-acquire it on the same thread (std::shared_mutex is not recursive), one guard per
// operation (e.g. one around all of an inbound email's put_file calls plus its deliver tx).
// Writers in this process: api attachments_upload, jobs run_inbound_fetch (+ blobs-migrate).
std::shared_lock<std::shared_mutex> blob_writer_guard();  // shared: writers run concurrently
std::unique_lock<std::shared_mutex> blob_gc_guard();      // exclusive: gc.blobs, one blob at a time

}  // namespace azm
