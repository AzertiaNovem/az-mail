// Owner: WP-C — storage::FileCache (bounded LRU, atomic fill, single-flight, crash rebuild).
#include "core/crypto.hpp"
#include "storage/file_cache.hpp"
#include "test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <fstream>
#include <stdexcept>
#include <thread>

using namespace azm;
using namespace azm::storage;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

std::string key(int i) { return crypto::sha256_hex("blob-" + std::to_string(i)); }

void write_file(const fs::path& p, const std::string& data) {
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  out << data;
}
std::string read_file(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), {});
}
std::function<void(const fs::path&)> writer(std::string data, std::atomic<int>* calls = nullptr) {
  return [data = std::move(data), calls](const fs::path& tmp) {
    if (calls) ++*calls;
    write_file(tmp, data);
  };
}
bool has_part_files(const fs::path& dir) {
  for (const auto& de : fs::directory_iterator(dir))
    if (de.path().extension() == ".part") return true;
  return false;
}

}  // namespace

TEST_CASE("file cache: miss fills once, hits reuse the file", "[file_cache]") {
  test::TempDir td;
  FileCache cache(td / "cache", 1 << 20);
  std::atomic<int> calls{0};
  const auto p1 = cache.get_or_fill(key(1), writer("hello", &calls));
  CHECK(read_file(p1) == "hello");
  CHECK(p1.parent_path() == td / "cache");
  const auto p2 = cache.get_or_fill(key(1), writer("other", &calls));
  CHECK(p1 == p2);
  CHECK(calls == 1);
  CHECK(cache.lookup(key(1)) == std::optional<fs::path>(p1));
  CHECK_FALSE(cache.lookup(key(2)).has_value());
  CHECK(cache.size_bytes() == 5);
  CHECK(cache.max_bytes() == (1u << 20));
  CHECK(cache.dir() == td / "cache");
  CHECK_FALSE(has_part_files(td / "cache"));
}

TEST_CASE("file cache: failed fills leave nothing behind", "[file_cache]") {
  test::TempDir td;
  FileCache cache(td / "cache", 1 << 20);
  CHECK_THROWS_AS(cache.get_or_fill(key(1),
                                    [](const fs::path& tmp) {
                                      write_file(tmp, "partial");
                                      throw std::runtime_error("network down");
                                    }),
                  std::runtime_error);
  CHECK_FALSE(cache.lookup(key(1)).has_value());
  CHECK(cache.size_bytes() == 0);
  CHECK_FALSE(has_part_files(td / "cache"));
  // A fill that never creates the file is an error too.
  CHECK_THROWS(cache.get_or_fill(key(2), [](const fs::path&) {}));
  CHECK_FALSE(cache.lookup(key(2)).has_value());
  // Next attempt succeeds.
  CHECK(read_file(cache.get_or_fill(key(1), writer("ok"))) == "ok");
}

TEST_CASE("file cache: concurrent misses for one key share a single fill", "[file_cache]") {
  test::TempDir td;
  FileCache cache(td / "cache", 1 << 20);
  std::atomic<int> calls{0};
  std::vector<std::thread> threads;
  std::vector<fs::path> paths(8);
  for (int i = 0; i < 8; ++i) {
    threads.emplace_back([&, i] {
      paths[i] = cache.get_or_fill(key(7), [&](const fs::path& tmp) {
        ++calls;
        std::this_thread::sleep_for(100ms);
        write_file(tmp, "shared");
      });
    });
  }
  for (auto& t : threads) t.join();
  CHECK(calls == 1);
  for (const auto& p : paths) CHECK(p == paths[0]);
  CHECK(read_file(paths[0]) == "shared");
}

TEST_CASE("file cache: waiters see the filler's error", "[file_cache]") {
  test::TempDir td;
  FileCache cache(td / "cache", 1 << 20);
  std::atomic<int> calls{0}, failures{0};
  std::vector<std::thread> threads;
  for (int i = 0; i < 4; ++i) {
    threads.emplace_back([&] {
      try {
        (void)cache.get_or_fill(key(3), [&](const fs::path&) {
          ++calls;
          std::this_thread::sleep_for(100ms);
          throw std::runtime_error("r2 unavailable");
        });
      } catch (const std::runtime_error&) {
        ++failures;
      }
    });
  }
  for (auto& t : threads) t.join();
  CHECK(failures == 4);
  CHECK(calls >= 1);
  CHECK(calls < 4);  // at least some waiters shared the failed fill
}

TEST_CASE("file cache: LRU eviction above the cap, young entries spared", "[file_cache]") {
  test::TempDir td;
  SECTION("least recently used goes first") {
    FileCache cache(td / "c1", 250, 0s);
    const auto a = cache.get_or_fill(key(1), writer(std::string(100, 'a')));
    const auto b = cache.get_or_fill(key(2), writer(std::string(100, 'b')));
    std::this_thread::sleep_for(5ms);
    CHECK(cache.lookup(key(1)).has_value());  // touch a: b is now the LRU
    (void)cache.get_or_fill(key(3), writer(std::string(100, 'c')));
    CHECK(cache.lookup(key(1)).has_value());
    CHECK_FALSE(cache.lookup(key(2)).has_value());
    CHECK_FALSE(fs::exists(b));
    CHECK(fs::exists(a));
    CHECK(cache.size_bytes() == 200);
  }
  SECTION("entries used within min_age are kept even over the cap") {
    FileCache cache(td / "c2", 250, 60s);
    for (int i = 1; i <= 3; ++i) (void)cache.get_or_fill(key(i), writer(std::string(100, 'x')));
    CHECK(cache.size_bytes() == 300);
    for (int i = 1; i <= 3; ++i) CHECK(cache.lookup(key(i)).has_value());
  }
  SECTION("a single file larger than the cap is still served") {
    FileCache cache(td / "c3", 10, 0s);
    const auto p = cache.get_or_fill(key(9), writer(std::string(50, 'z')));
    CHECK(read_file(p).size() == 50);
  }
}

TEST_CASE("file cache: index rebuilt from disk; partial files removed", "[file_cache]") {
  test::TempDir td;
  const fs::path dir = td / "cache";
  fs::create_directories(dir);
  write_file(dir / key(1), "restored");
  write_file(dir / (key(2) + ".abc.part"), "partial");
  write_file(dir / "junk.txt", "x");
  FileCache cache(dir, 1 << 20);
  CHECK(cache.lookup(key(1)).has_value());
  CHECK(cache.size_bytes() == 8);
  CHECK_FALSE(fs::exists(dir / (key(2) + ".abc.part")));
  CHECK_FALSE(fs::exists(dir / "junk.txt"));

  // Restored entries over a smaller cap are evicted right away (they count as old).
  write_file(dir / key(3), std::string(100, 'q'));
  FileCache small(dir, 50, 60s);
  CHECK(small.size_bytes() <= 50);
}

TEST_CASE("file cache: erase, clear, external deletion, invalid keys", "[file_cache]") {
  test::TempDir td;
  FileCache cache(td / "cache", 1 << 20);
  const auto p = cache.get_or_fill(key(1), writer("one"));
  (void)cache.get_or_fill(key(2), writer("two"));
  cache.erase(key(1));
  CHECK_FALSE(fs::exists(p));
  CHECK_FALSE(cache.lookup(key(1)).has_value());
  cache.erase(key(1));  // no-op
  cache.erase("../../etc/passwd");  // invalid keys are ignored
  CHECK(cache.size_bytes() == 3);

  fs::remove(*cache.lookup(key(2)));  // deleted behind the cache's back
  CHECK_FALSE(cache.lookup(key(2)).has_value());
  CHECK(read_file(cache.get_or_fill(key(2), writer("again"))) == "again");

  cache.clear();
  CHECK(cache.size_bytes() == 0);
  CHECK_FALSE(cache.lookup(key(2)).has_value());

  CHECK_THROWS_AS(cache.get_or_fill("not-a-sha", writer("x")), std::invalid_argument);
  CHECK_THROWS_AS(cache.get_or_fill(std::string(64, 'A'), writer("x")), std::invalid_argument);
  CHECK_FALSE(cache.lookup("../x").has_value());
}
