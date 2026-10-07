// Owner: WP-C
// WP0 stub: compiles and links; WP-C implements.
#include "storage/file_cache.hpp"

#include "core/errors.hpp"

namespace azm::storage {

struct FileCache::Impl {
  std::filesystem::path dir;
  std::uint64_t max_bytes = 0;
};

FileCache::FileCache(std::filesystem::path dir, std::uint64_t max_bytes, std::chrono::seconds)
    : impl_(std::make_unique<Impl>(Impl{std::move(dir), max_bytes})) {}
FileCache::~FileCache() = default;

std::filesystem::path FileCache::get_or_fill(
    std::string_view, const std::function<void(const std::filesystem::path&)>&) {
  throw NotImplemented("storage::FileCache::get_or_fill");
}
std::optional<std::filesystem::path> FileCache::lookup(std::string_view) {
  throw NotImplemented("storage::FileCache::lookup");
}
void FileCache::erase(std::string_view) { throw NotImplemented("storage::FileCache::erase"); }
void FileCache::clear() { throw NotImplemented("storage::FileCache::clear"); }
std::uint64_t FileCache::size_bytes() const {
  throw NotImplemented("storage::FileCache::size_bytes");
}
std::uint64_t FileCache::max_bytes() const { return impl_->max_bytes; }
const std::filesystem::path& FileCache::dir() const { return impl_->dir; }

}  // namespace azm::storage
