// Owner: WP-C
//
// Bounded on-disk LRU cache for R2 "proxy" delivery (DESIGN Addendum A, AZMAIL_FILES_DELIVERY=
// proxy): the backend downloads a blob once into <data_dir>/cache and streams it like a local
// file. Keys are blob sha256 hex strings (validated: is_sha256_hex), files are immutable.
//  * Total size is capped at `max_bytes` (cfg.file_cache_mb MiB); on insert, least-recently-used
//    entries are evicted — except entries used within the last `min_age` (60 s), so a path just
//    returned stays valid long enough for the session to open it (an opened file survives
//    unlinking on POSIX).
//  * Concurrent misses for the same key wait for a single fill.
//  * The index is rebuilt from the directory at construction (crash-safe: partial ".part"
//    files are deleted).
// Thread-safe.
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>

namespace azm::storage {

class FileCache {
 public:
  FileCache(std::filesystem::path dir, std::uint64_t max_bytes,
            std::chrono::seconds min_age = std::chrono::seconds(60));
  ~FileCache();
  FileCache(const FileCache&) = delete;
  FileCache& operator=(const FileCache&) = delete;

  // Path of the cached file for `key`. On a miss calls fill(tmp_path) (which must write the
  // complete content to tmp_path or throw), then atomically renames it into place. Exceptions
  // from fill propagate and leave no entry. Throws std::invalid_argument for a bad key.
  std::filesystem::path get_or_fill(std::string_view key,
                                    const std::function<void(const std::filesystem::path&)>& fill);

  // Cached path (and LRU touch) or nullopt.
  std::optional<std::filesystem::path> lookup(std::string_view key);
  void erase(std::string_view key);  // missing = no-op (blob GC calls this)
  void clear();

  std::uint64_t size_bytes() const;
  std::uint64_t max_bytes() const;
  const std::filesystem::path& dir() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace azm::storage
