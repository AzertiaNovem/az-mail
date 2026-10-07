#include "core/blob_store.hpp"

#include "core/crypto.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <fstream>
#include <system_error>

namespace azm {
namespace fs = std::filesystem;

namespace {

[[noreturn]] void throw_blob_error(const std::string& msg, bool retryable) {
  BlobError e(msg);
  e.retryable = retryable;
  throw e;
}

void validate_sha(std::string_view sha) {
  if (!is_sha256_hex(sha)) throw_blob_error("invalid blob sha256", false);
}

// Best-effort durability: flush file data (or a directory entry) to stable storage.
void fsync_path(const fs::path& p) {
  const int fd = ::open(p.c_str(), O_RDONLY);
  if (fd < 0) return;
  (void)::fsync(fd);
  (void)::close(fd);
}

class LocalBlobStore final : public BlobStore {
 public:
  explicit LocalBlobStore(fs::path data_dir)
      : blobs_(data_dir / "blobs"), tmp_(data_dir / "tmp") {
    std::error_code ec;
    fs::create_directories(blobs_, ec);
    if (ec) throw_blob_error("cannot create blob dir " + blobs_.string() + ": " + ec.message(), false);
    fs::create_directories(tmp_, ec);
    if (ec) throw_blob_error("cannot create tmp dir " + tmp_.string() + ": " + ec.message(), false);
  }

  std::string_view kind() const override { return "local"; }
  fs::path tmp_dir() const override { return tmp_; }

  BlobRef put_file(const fs::path& staged, std::optional<std::string> sha256_hex) override {
    if (sha256_hex) validate_sha(*sha256_hex);
    std::error_code ec;
    if (!fs::is_regular_file(staged, ec)) throw_blob_error("staged file missing", false);
    std::string actual;
    try {
      actual = crypto::sha256_file_hex(staged);
    } catch (const std::exception& e) {
      throw_blob_error(std::string("cannot hash staged file: ") + e.what(), true);
    }
    if (sha256_hex && *sha256_hex != actual) throw_blob_error("staged file sha256 mismatch", false);
    const auto size = fs::file_size(staged, ec);
    if (ec) throw_blob_error("cannot stat staged file: " + ec.message(), true);
    return commit(staged, actual, static_cast<int64_t>(size));
  }

  BlobRef put_bytes(std::string_view bytes) override {
    const std::string sha = crypto::sha256_hex(bytes);
    const auto size = static_cast<int64_t>(bytes.size());
    if (have(sha, size)) return {sha, size, "local"};
    const fs::path staging = make_staging_path(tmp_);
    {
      std::ofstream out(staging, std::ios::binary | std::ios::trunc);
      out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
      out.flush();
      if (!out) {
        std::error_code ec;
        fs::remove(staging, ec);
        throw_blob_error("cannot write staging file", true);
      }
    }
    try {
      return commit(staging, sha, size);
    } catch (...) {
      std::error_code ec;
      fs::remove(staging, ec);
      throw;
    }
  }

  void get_to_file(std::string_view sha, const fs::path& dest) override {
    const fs::path p = path_for(sha);
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) throw BlobNotFound("blob not found: " + std::string(sha));
    fs::copy_file(p, dest, fs::copy_options::overwrite_existing, ec);
    if (ec) throw_blob_error("cannot copy blob: " + ec.message(), true);
  }

  std::string get_bytes(std::string_view sha, std::size_t max_bytes) override {
    const fs::path p = path_for(sha);
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) throw BlobNotFound("blob not found: " + std::string(sha));
    const auto size = fs::file_size(p, ec);
    if (ec) throw_blob_error("cannot stat blob: " + ec.message(), true);
    if (size > max_bytes) throw_blob_error("blob larger than allowed", false);
    std::string out(static_cast<std::size_t>(size), '\0');
    std::ifstream in(p, std::ios::binary);
    if (!in) throw_blob_error("cannot open blob", true);
    in.read(out.data(), static_cast<std::streamsize>(out.size()));
    if (static_cast<std::size_t>(in.gcount()) != out.size()) throw_blob_error("short read on blob", true);
    return out;
  }

  bool exists(std::string_view sha) override {
    std::error_code ec;
    return fs::is_regular_file(path_for(sha), ec);
  }

  void remove(std::string_view sha) override {
    const fs::path p = path_for(sha);
    std::error_code ec;
    fs::remove(p, ec);
    if (ec && ec != std::errc::no_such_file_or_directory)
      throw_blob_error("cannot remove blob: " + ec.message(), true);
    // Prune now-empty fan-out directories (fails harmlessly when not empty).
    fs::remove(p.parent_path(), ec);
    fs::remove(p.parent_path().parent_path(), ec);
  }

  ServePlan serve(std::string_view sha, const ServeOptions&) override {
    const fs::path p = path_for(sha);
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) throw BlobNotFound("blob not found: " + std::string(sha));
    return LocalFile{p};
  }

  std::vector<std::string> public_origins() const override { return {}; }

 private:
  fs::path path_for(std::string_view sha) const { return blobs_.parent_path() / blob_relative_key(sha); }

  bool have(const std::string& sha, int64_t size) const {
    std::error_code ec;
    const fs::path p = path_for(sha);
    if (!fs::is_regular_file(p, ec)) return false;
    const auto existing = fs::file_size(p, ec);
    return !ec && static_cast<int64_t>(existing) == size;
  }

  // Moves a verified staged file into place atomically (rename; copy+rename across devices).
  BlobRef commit(const fs::path& staged, const std::string& sha, int64_t size) {
    std::error_code ec;
    if (have(sha, size)) {
      fs::remove(staged, ec);
      return {sha, size, "local"};
    }
    const fs::path dest = path_for(sha);
    fs::create_directories(dest.parent_path(), ec);
    if (ec) throw_blob_error("cannot create blob dir: " + ec.message(), true);
    fsync_path(staged);
    fs::rename(staged, dest, ec);
    if (ec) {
      if (ec != std::errc::cross_device_link) throw_blob_error("cannot move blob: " + ec.message(), true);
      const fs::path part = dest.parent_path() / ("." + crypto::hex_encode(crypto::random_bytes(8)) + ".part");
      fs::copy_file(staged, part, fs::copy_options::overwrite_existing, ec);
      if (!ec) {
        fsync_path(part);
        fs::rename(part, dest, ec);
      }
      if (ec) {
        std::error_code ec2;
        fs::remove(part, ec2);
        throw_blob_error("cannot copy blob across devices: " + ec.message(), true);
      }
      fs::remove(staged, ec);
    }
    fsync_path(dest.parent_path());
    return {sha, size, "local"};
  }

  fs::path blobs_;
  fs::path tmp_;
};

}  // namespace

bool is_sha256_hex(std::string_view s) {
  if (s.size() != 64) return false;
  for (char c : s)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  return true;
}

std::string blob_relative_key(std::string_view sha) {
  validate_sha(sha);
  std::string key = "blobs/";
  key.append(sha.substr(0, 2));
  key.push_back('/');
  key.append(sha.substr(2, 2));
  key.push_back('/');
  key.append(sha);
  return key;
}

fs::path make_staging_path(const fs::path& tmp_dir) {
  return tmp_dir / (crypto::hex_encode(crypto::random_bytes(12)) + ".part");
}

std::unique_ptr<BlobStore> make_local_blob_store(fs::path data_dir) {
  return std::make_unique<LocalBlobStore>(std::move(data_dir));
}

namespace {
// One process-wide mutex: the race it prevents spans every BlobStore instance (mixed stores).
std::shared_mutex& blob_gc_mutex() {
  static std::shared_mutex m;
  return m;
}
}  // namespace

std::shared_lock<std::shared_mutex> blob_writer_guard() {
  return std::shared_lock<std::shared_mutex>(blob_gc_mutex());
}

std::unique_lock<std::shared_mutex> blob_gc_guard() {
  return std::unique_lock<std::shared_mutex>(blob_gc_mutex());
}

}  // namespace azm
