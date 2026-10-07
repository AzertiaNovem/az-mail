#include "core/blob_store.hpp"
#include "core/crypto.hpp"
#include "test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <fstream>
#include <thread>

using namespace azm;
namespace fs = std::filesystem;

namespace {

const std::string kHelloSha = "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824";

fs::path write_file(const fs::path& p, std::string_view data) {
  std::ofstream out(p, std::ios::binary);
  out.write(data.data(), static_cast<std::streamsize>(data.size()));
  return p;
}

std::string read_file(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), {});
}

std::size_t count_files(const fs::path& dir) {
  std::size_t n = 0;
  for (const auto& e : fs::recursive_directory_iterator(dir))
    if (e.is_regular_file()) ++n;
  return n;
}

}  // namespace

TEST_CASE("LocalBlobStore basics", "[blob]") {
  test::TempDir td;
  auto store = make_local_blob_store(td.path());
  CHECK(store->kind() == "local");
  CHECK(store->public_origins().empty());
  CHECK(store->tmp_dir() == td.path() / "tmp");
  CHECK(fs::is_directory(store->tmp_dir()));

  const BlobRef ref = store->put_bytes("hello");
  CHECK(ref.sha256 == kHelloSha);
  CHECK(ref.size == 5);
  CHECK(ref.storage == "local");
  CHECK(store->exists(kHelloSha));
  CHECK(fs::is_regular_file(td.path() / "blobs" / "2c" / "f2" / kHelloSha));
  CHECK(store->get_bytes(kHelloSha, 1024) == "hello");

  const fs::path out = td.path() / "copy.bin";
  store->get_to_file(kHelloSha, out);
  CHECK(read_file(out) == "hello");

  const ServePlan plan = store->serve(kHelloSha, ServeOptions{"text/plain", "inline", std::chrono::seconds(60)});
  REQUIRE(std::holds_alternative<LocalFile>(plan));
  CHECK(read_file(std::get<LocalFile>(plan).path) == "hello");

  store->remove(kHelloSha);
  CHECK_FALSE(store->exists(kHelloSha));
  CHECK_FALSE(fs::exists(td.path() / "blobs" / "2c"));  // empty fan-out dirs pruned
  CHECK_NOTHROW(store->remove(kHelloSha));             // missing = no-op
}

TEST_CASE("LocalBlobStore put_file: staging, verification, dedupe", "[blob]") {
  test::TempDir td;
  auto store = make_local_blob_store(td.path());

  // Without sha: computed; staged file is consumed.
  const fs::path s1 = write_file(make_staging_path(store->tmp_dir()), "hello");
  const BlobRef r1 = store->put_file(s1);
  CHECK(r1.sha256 == kHelloSha);
  CHECK(r1.size == 5);
  CHECK_FALSE(fs::exists(s1));

  // Same content again with the correct sha: idempotent, staged deleted, one stored copy.
  const fs::path s2 = write_file(make_staging_path(store->tmp_dir()), "hello");
  const BlobRef r2 = store->put_file(s2, kHelloSha);
  CHECK(r2.sha256 == r1.sha256);
  CHECK_FALSE(fs::exists(s2));
  CHECK(store->put_bytes("hello").sha256 == kHelloSha);
  CHECK(count_files(td.path() / "blobs") == 1);
  CHECK(count_files(store->tmp_dir()) == 0);

  // Wrong (well-formed) sha: rejected, staged file left for the caller.
  const fs::path s3 = write_file(make_staging_path(store->tmp_dir()), "other");
  try {
    store->put_file(s3, kHelloSha);
    FAIL("expected BlobError");
  } catch (const BlobError& e) {
    CHECK_FALSE(e.retryable);
  }
  CHECK(fs::exists(s3));

  // Missing staged file.
  CHECK_THROWS_AS(store->put_file(td.path() / "nope.part"), BlobError);

  // Empty blob is fine.
  const BlobRef empty = store->put_bytes("");
  CHECK(empty.sha256 == crypto::sha256_hex(""));
  CHECK(empty.size == 0);
  CHECK(store->get_bytes(empty.sha256, 0).empty());
}

TEST_CASE("LocalBlobStore rejects malformed sha strings (path traversal)", "[blob]") {
  test::TempDir td;
  auto store = make_local_blob_store(td.path());
  const std::string bad[] = {"../../etc/passwd", "", kHelloSha.substr(0, 63),
                             "2CF24DBA5FB0A30E26E83B2AC5B9E29E1B161E5C1FA7425E73043362938B9824",
                             "../" + kHelloSha.substr(3), kHelloSha + "/"};
  for (const auto& sha : bad) {
    CHECK_THROWS_AS(store->exists(sha), BlobError);
    CHECK_THROWS_AS(store->get_bytes(sha, 10), BlobError);
    CHECK_THROWS_AS(store->get_to_file(sha, td.path() / "x"), BlobError);
    CHECK_THROWS_AS(store->remove(sha), BlobError);
    CHECK_THROWS_AS(store->serve(sha, {}), BlobError);
  }
  const fs::path s = write_file(make_staging_path(store->tmp_dir()), "hello");
  CHECK_THROWS_AS(store->put_file(s, std::string("../../evil")), BlobError);
  CHECK(fs::exists(s));
  CHECK(is_sha256_hex(kHelloSha));
  CHECK_FALSE(is_sha256_hex("g" + kHelloSha.substr(1)));
  CHECK(blob_relative_key(kHelloSha) == "blobs/2c/f2/" + kHelloSha);
  CHECK_THROWS_AS(blob_relative_key("../x"), BlobError);
}

TEST_CASE("LocalBlobStore missing blobs and size limit", "[blob]") {
  test::TempDir td;
  auto store = make_local_blob_store(td.path());
  const std::string missing = crypto::sha256_hex("never stored");
  CHECK_FALSE(store->exists(missing));
  CHECK_THROWS_AS(store->get_bytes(missing, 100), BlobNotFound);
  CHECK_THROWS_AS(store->get_to_file(missing, td.path() / "out"), BlobNotFound);
  CHECK_THROWS_AS(store->serve(missing, {}), BlobNotFound);

  const std::string data(1000, 'x');
  const BlobRef ref = store->put_bytes(data);
  CHECK(store->get_bytes(ref.sha256, 1000) == data);
  try {
    store->get_bytes(ref.sha256, 999);
    FAIL("expected BlobError");
  } catch (const BlobError& e) {
    CHECK_FALSE(e.retryable);
  }
}

TEST_CASE("LocalBlobStore concurrent puts of the same content", "[blob]") {
  test::TempDir td;
  auto store = make_local_blob_store(td.path());
  const std::string data = crypto::random_bytes(64 * 1024);
  const std::string sha = crypto::sha256_hex(data);
  std::vector<std::thread> threads;
  std::atomic<int> ok{0};
  for (int i = 0; i < 8; ++i) {
    threads.emplace_back([&, i] {
      try {
        if (i % 2 == 0) {
          if (store->put_bytes(data).sha256 == sha) ++ok;
        } else {
          const fs::path s = write_file(make_staging_path(store->tmp_dir()), data);
          if (store->put_file(s, sha).sha256 == sha) ++ok;
        }
      } catch (...) {
      }
    });
  }
  for (auto& t : threads) t.join();
  CHECK(ok == 8);
  CHECK(count_files(td.path() / "blobs") == 1);
  CHECK(count_files(store->tmp_dir()) == 0);
  CHECK(store->get_bytes(sha, data.size()) == data);
}

TEST_CASE("blob writer guards are shared; the GC guard excludes them", "[blob_store]") {
  using namespace std::chrono_literals;
  std::atomic<int> writers_inside{0};
  std::atomic<bool> both_writers_seen{false};
  std::atomic<bool> released{false};
  std::atomic<bool> gc_saw_release{false};

  // Two writers on different threads hold the shared guard at the same time.
  std::thread w1([&] {
    auto g = blob_writer_guard();
    ++writers_inside;
    while (writers_inside.load() < 2) std::this_thread::sleep_for(1ms);
    both_writers_seen = true;
    std::this_thread::sleep_for(50ms);
    released = true;  // set just before the guards are released
  });
  std::thread w2([&] {
    auto g = blob_writer_guard();
    ++writers_inside;
    while (!released.load()) std::this_thread::sleep_for(1ms);
  });
  while (writers_inside.load() < 2) std::this_thread::sleep_for(1ms);

  // GC blocks until every writer has released.
  std::thread gc([&] {
    auto g = blob_gc_guard();
    gc_saw_release = released.load();
  });
  w1.join();
  w2.join();
  gc.join();
  CHECK(both_writers_seen.load());
  CHECK(gc_saw_release.load());

  // And a writer waits for a running GC.
  std::atomic<bool> gc_done{false};
  std::atomic<bool> writer_saw_gc_done{false};
  auto gc_lock = blob_gc_guard();
  std::thread w3([&] {
    auto g = blob_writer_guard();
    writer_saw_gc_done = gc_done.load();
  });
  std::this_thread::sleep_for(30ms);
  gc_done = true;
  gc_lock.unlock();
  w3.join();
  CHECK(writer_saw_gc_done.load());
}
