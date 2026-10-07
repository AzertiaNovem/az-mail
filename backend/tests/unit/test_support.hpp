// Shared helpers for hermetic unit tests (temp directories, temp databases, seeded accounts,
// a ready-made Services aggregate). Owner: WP0; every package may use them.
//
// The seed_* helpers write account tables with raw SQL on purpose: they are test fixtures, so
// packages can test without WP-D's repo::* (CONTRACTS §G rule 7 applies to production code).
// They follow the §2 schema exactly (FK / CHECK constraints stay enforced).
#pragma once

#include "config.hpp"
#include "core/address.hpp"
#include "core/blob_store.hpp"
#include "core/crypto.hpp"
#include "core/signed_url.hpp"
#include "core/strings.hpp"
#include "core/time.hpp"
#include "db/migrations.hpp"
#include "db/sqlite.hpp"
#include "notifier.hpp"
#include "repo/accounts.hpp"
#include "services.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace azm::test {

// Creates a unique empty directory under the system temp dir; removes it recursively on
// destruction.
class TempDir {
 public:
  TempDir() {
    path_ = std::filesystem::temp_directory_path() /
            ("azmail-test-" + crypto::hex_encode(crypto::random_bytes(8)));
    std::filesystem::create_directories(path_);
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  const std::filesystem::path& path() const { return path_; }
  std::filesystem::path operator/(const std::string& name) const { return path_ / name; }

 private:
  std::filesystem::path path_;
};

// HMAC secret used by TestServices (≥ 32 bytes).
inline constexpr std::string_view kTestSecret = "0123456789abcdef0123456789abcdef";

// Applies all migrations on a pool's database.
inline void migrate(db::Pool& pool) {
  auto lease = pool.acquire();
  db::migrate(*lease);
}

// ---- account seeding ---------------------------------------------------------------------------

// Get-or-create a local domain (case-insensitive). Returns domains.id.
inline int64_t seed_domain(db::Tx& tx, std::string_view name, int64_t now_ms = 1) {
  const std::string n = to_lower_ascii(name);
  if (auto id = tx.scalar<int64_t>("SELECT id FROM domains WHERE name=?", n)) return *id;
  tx.run("INSERT INTO domains(name, receiving_enabled, created_at) VALUES(?,1,?)", n, now_ms);
  return tx.last_insert_id();
}

// users + addresses(kind 'user') + user_settings (schema defaults); the email's domain is
// created when missing. `password_hash` empty → "!" (never verifies). Returns users.id.
inline int64_t seed_user(db::Tx& tx, std::string_view email, bool admin = false,
                         std::string_view display_name = {}, std::string_view password_hash = {},
                         int64_t now_ms = 1) {
  const std::string e = normalize_email(email);
  const int64_t domain_id = seed_domain(tx, domain_of(e), now_ms);
  tx.run(
      "INSERT INTO users(email, display_name, password_hash, is_admin, disabled, "
      "password_changed_at, created_at, updated_at) VALUES(?,?,?,?,0,?,?,?)",
      e, display_name, password_hash.empty() ? std::string_view("!") : password_hash, admin,
      now_ms, now_ms, now_ms);
  const int64_t uid = tx.last_insert_id();
  tx.run(
      "INSERT INTO addresses(email, domain_id, kind, user_id, display_name, created_at) "
      "VALUES(?,?,'user',?,?,?)",
      e, domain_id, uid, display_name, now_ms);
  tx.run("INSERT INTO user_settings(user_id, updated_at) VALUES(?,?)", uid, now_ms);
  return uid;
}

// addresses(kind 'alias') + alias_members {user_id, can_send_as}; the domain is created when
// missing. Returns the alias addresses.id.
inline int64_t seed_alias(db::Tx& tx, std::string_view email,
                          const std::vector<std::pair<int64_t, bool>>& members,
                          bool share_sent = true, std::string_view display_name = {},
                          int64_t now_ms = 1) {
  const std::string e = normalize_email(email);
  const int64_t domain_id = seed_domain(tx, domain_of(e), now_ms);
  tx.run(
      "INSERT INTO addresses(email, domain_id, kind, user_id, display_name, share_sent, "
      "created_at) VALUES(?,?,'alias',NULL,?,?,?)",
      e, domain_id, display_name, share_sent, now_ms);
  const int64_t alias_id = tx.last_insert_id();
  for (const auto& [user_id, can_send_as] : members)
    tx.run("INSERT INTO alias_members(alias_id, user_id, can_send_as) VALUES(?,?,?)", alias_id,
           user_id, can_send_as);
  return alias_id;
}

// A sessions row for `user_id` (token_hash = repo::session_token_hash as a BLOB, like
// repo::create_session). Returns the RAW bearer token. now_ms 0 → azm::now_ms().
inline std::string seed_session(db::Tx& tx, int64_t user_id,
                                int64_t ttl_ms = 30LL * 24 * 3600 * 1000, int64_t now_ms = 0) {
  if (now_ms == 0) now_ms = azm::now_ms();
  std::string token = crypto::random_token_b64url(32);
  const std::string h = repo::session_token_hash(token);
  const std::vector<uint8_t> hash(h.begin(), h.end());
  tx.run(
      "INSERT INTO sessions(user_id, token_hash, created_at, last_seen_at, expires_at) "
      "VALUES(?,?,?,?,?)",
      user_id, hash, now_ms, now_ms, now_ms + ttl_ms);
  return token;
}

// addresses.id for an email (exact, normalized); 0 when absent.
inline int64_t address_id(db::Conn& c, std::string_view email) {
  return c.scalar<int64_t>("SELECT id FROM addresses WHERE email=?", normalize_email(email))
      .value_or(0);
}

// ---- a ready-made Services -------------------------------------------------------------------
// TempDir + migrated temp-file Pool (TxHooks → RecordingNotifier, so tx.emit lands in
// `notifier`) + LocalBlobStore under <dir>/data + SignedUrls(kTestSecret,
// cfg.public_api_base_url, ttl) + ManualClock + Services. Optional Services pointers stay null.
// The clock starts at REAL time by default so functions that use azm::now_ms() internally
// (mail/types.hpp clock convention) agree with svc.now_ms(). Non-movable; mutate `cfg` freely
// after construction (Services holds references).
struct TestServices {
  explicit TestServices(int64_t start_ms = azm::now_ms(), std::size_t pool_size = 4,
                        int64_t signed_url_ttl_ms = SignedUrls::kDefaultTtlMs)
      : clock(start_ms),
        db(dir / "azmail.db", pool_size, db::TxHooks{&notifier, {}}),
        blobs(make_local_blob_store(dir / "data")),
        urls(std::string(kTestSecret), cfg.public_api_base_url, signed_url_ttl_ms),
        svc{cfg, db, *blobs, urls, notifier, clock} {
    cfg.data_dir = (dir / "data").string();
    cfg.db_path = (dir / "azmail.db").string();
    cfg.server_secret = std::string(kTestSecret);
    migrate(db);
  }
  TestServices(const TestServices&) = delete;
  TestServices& operator=(const TestServices&) = delete;

  TempDir dir;
  Config cfg;
  RecordingNotifier notifier;
  ManualClock clock;
  db::Pool db;
  std::unique_ptr<BlobStore> blobs;
  SignedUrls urls;
  Services svc;
};

}  // namespace azm::test
