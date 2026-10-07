// Owner: WP-C
// Bounded on-disk LRU cache for R2 proxy delivery (DESIGN Addendum A).
#include "storage/file_cache.hpp"

#include "core/blob_store.hpp"
#include "core/crypto.hpp"
#include "core/log.hpp"

#include <algorithm>
#include <condition_variable>
#include <exception>
#include <list>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <unordered_map>

namespace azm::storage {
namespace fs = std::filesystem;
namespace {

using SteadyClock = std::chrono::steady_clock;

void require_key(std::string_view key) {
  if (!is_sha256_hex(key)) throw std::invalid_argument("FileCache: key must be a sha256 hex string");
}

}  // namespace

struct FileCache::Impl {
  fs::path dir;
  std::uint64_t max_bytes = 0;
  std::chrono::seconds min_age{60};

  struct Entry {
    std::uint64_t size = 0;
    SteadyClock::time_point last_use{};
    std::list<std::string>::iterator lru;  // position in `order` (front = least recently used)
  };
  // One fill in flight per key; waiters share its outcome.
  struct Fill {
    bool done = false;
    std::exception_ptr error;
  };

  mutable std::mutex mu;
  std::condition_variable cv;
  std::unordered_map<std::string, Entry> entries;
  std::list<std::string> order;
  std::unordered_map<std::string, std::shared_ptr<Fill>> filling;
  std::uint64_t total = 0;

  fs::path path_for(std::string_view key) const { return dir / std::string(key); }

  void touch(const std::string& key, Entry& e) {
    e.last_use = SteadyClock::now();
    order.splice(order.end(), order, e.lru);
  }

  void drop(std::unordered_map<std::string, Entry>::iterator it) {
    total -= std::min(total, it->second.size);
    order.erase(it->second.lru);
    std::error_code ec;
    fs::remove(path_for(it->first), ec);
    entries.erase(it);
  }

  // Evicts least-recently-used entries until the cache fits, sparing `keep` and entries used
  // within `min_age` (a path just handed out must stay valid until the caller opens it).
  void evict(const std::string& keep) {
    const auto young = SteadyClock::now() - min_age;
    for (auto it = order.begin(); it != order.end() && total > max_bytes;) {
      const std::string key = *it;
      ++it;
      if (key == keep) continue;
      auto e = entries.find(key);
      if (e == entries.end() || e->second.last_use > young) continue;
      drop(e);
    }
  }

  void insert(const std::string& key, std::uint64_t size, SteadyClock::time_point last_use) {
    order.push_back(key);
    Entry e;
    e.size = size;
    e.last_use = last_use;
    e.lru = std::prev(order.end());
    entries[key] = e;
    total += size;
  }
};

FileCache::FileCache(fs::path dir, std::uint64_t max_bytes, std::chrono::seconds min_age)
    : impl_(std::make_unique<Impl>()) {
  impl_->dir = std::move(dir);
  impl_->max_bytes = max_bytes;
  impl_->min_age = min_age;
  std::error_code ec;
  fs::create_directories(impl_->dir, ec);
  if (ec) throw std::runtime_error("FileCache: cannot create " + impl_->dir.string() + ": " + ec.message());

  // Rebuild the index: complete files named by their sha; ".part" leftovers of a crash go.
  struct Found {
    std::string key;
    std::uint64_t size;
    fs::file_time_type mtime;
  };
  std::vector<Found> found;
  for (const auto& de : fs::directory_iterator(impl_->dir, ec)) {
    std::error_code e2;
    if (!de.is_regular_file(e2)) continue;
    const std::string name = de.path().filename().string();
    if (!is_sha256_hex(name)) {
      fs::remove(de.path(), e2);  // partial fills and strays
      continue;
    }
    const auto size = de.file_size(e2);
    if (e2) continue;
    found.push_back({name, size, de.last_write_time(e2)});
  }
  // Oldest first; restored entries count as long unused (eligible for eviction at once).
  std::sort(found.begin(), found.end(), [](const Found& a, const Found& b) { return a.mtime < b.mtime; });
  for (const auto& f : found) impl_->insert(f.key, f.size, SteadyClock::time_point{});
  std::lock_guard lk(impl_->mu);
  impl_->evict("");
}

FileCache::~FileCache() = default;

fs::path FileCache::get_or_fill(std::string_view key_sv,
                                const std::function<void(const fs::path&)>& fill) {
  require_key(key_sv);
  Impl& m = *impl_;
  const std::string key(key_sv);
  std::unique_lock lk(m.mu);
  for (;;) {
    if (auto it = m.entries.find(key); it != m.entries.end()) {
      std::error_code ec;
      if (fs::is_regular_file(m.path_for(key), ec)) {
        m.touch(key, it->second);
        return m.path_for(key);
      }
      m.drop(it);  // deleted behind our back: refill
    }
    auto f = m.filling.find(key);
    if (f == m.filling.end()) break;  // we fill
    std::shared_ptr<Impl::Fill> fill_state = f->second;
    m.cv.wait(lk, [&] { return fill_state->done; });
    if (fill_state->error) std::rethrow_exception(fill_state->error);
    // Success: loop to pick up the new entry (or refill if it already vanished).
  }

  auto state = std::make_shared<Impl::Fill>();
  m.filling[key] = state;
  lk.unlock();

  const fs::path tmp = m.dir / (key + "." + crypto::hex_encode(crypto::random_bytes(6)) + ".part");
  const fs::path final_path = m.path_for(key);
  std::uint64_t size = 0;
  std::exception_ptr err;
  try {
    fill(tmp);
    size = fs::file_size(tmp);
    fs::rename(tmp, final_path);  // atomic: readers never see a partial file
  } catch (...) {
    err = std::current_exception();
    std::error_code ec;
    fs::remove(tmp, ec);
  }

  lk.lock();
  m.filling.erase(key);
  state->done = true;
  state->error = err;
  if (!err) {
    if (auto it = m.entries.find(key); it != m.entries.end()) {  // erase()+refill race: replace
      m.total -= std::min(m.total, it->second.size);
      m.order.erase(it->second.lru);
      m.entries.erase(it);
    }
    m.insert(key, size, SteadyClock::now());
    m.evict(key);
  }
  m.cv.notify_all();
  if (err) std::rethrow_exception(err);
  return final_path;
}

std::optional<fs::path> FileCache::lookup(std::string_view key_sv) {
  if (!is_sha256_hex(key_sv)) return std::nullopt;
  Impl& m = *impl_;
  const std::string key(key_sv);
  std::lock_guard lk(m.mu);
  auto it = m.entries.find(key);
  if (it == m.entries.end()) return std::nullopt;
  std::error_code ec;
  if (!fs::is_regular_file(m.path_for(key), ec)) {
    m.drop(it);
    return std::nullopt;
  }
  m.touch(key, it->second);
  return m.path_for(key);
}

void FileCache::erase(std::string_view key_sv) {
  if (!is_sha256_hex(key_sv)) return;
  Impl& m = *impl_;
  std::lock_guard lk(m.mu);
  if (auto it = m.entries.find(std::string(key_sv)); it != m.entries.end()) m.drop(it);
}

void FileCache::clear() {
  Impl& m = *impl_;
  std::lock_guard lk(m.mu);
  while (!m.entries.empty()) m.drop(m.entries.begin());
}

std::uint64_t FileCache::size_bytes() const {
  std::lock_guard lk(impl_->mu);
  return impl_->total;
}
std::uint64_t FileCache::max_bytes() const { return impl_->max_bytes; }
const fs::path& FileCache::dir() const { return impl_->dir; }

}  // namespace azm::storage
