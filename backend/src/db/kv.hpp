// Owner: WP0 (frozen; new key constants may be added by any WP — additive only)
//
// Tiny helpers over the `kv(key, value, updated_at)` table (DESIGN §2): poll state, last webhook
// time, quota banner. Values are TEXT; integers are stored in decimal.
#pragma once

#include "db/sqlite.hpp"

#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace azm::db {

// Well-known keys. Writers: see docs/CONTRACTS.md (§ "kv keys").
namespace kv_keys {
inline constexpr std::string_view kLastWebhookAt = "last_webhook_at";      // ms; jobs::process_webhook
inline constexpr std::string_view kLastPollAt = "last_poll_at";            // ms; poll.receiving
inline constexpr std::string_view kPollHighWater = "poll.high_water";      // newest resend_id seen by the poller
inline constexpr std::string_view kPollGapWarning = "poll.gap_warning";    // text; admin warning (B7 retention gap)
inline constexpr std::string_view kQuotaBlocked = "resend.quota_blocked";  // "daily"|"monthly"|absent; admin banner
}  // namespace kv_keys

// Value for `key`, or nullopt when absent.
inline std::optional<std::string> kv_get(Conn& c, std::string_view key) {
  return c.scalar<std::string>("SELECT value FROM kv WHERE key=?", key);
}

// Integer value for `key`; nullopt when absent or not a decimal integer.
inline std::optional<int64_t> kv_get_i64(Conn& c, std::string_view key) {
  auto v = kv_get(c, key);
  if (!v) return std::nullopt;
  int64_t out = 0;
  const char* b = v->data();
  const char* e = b + v->size();
  auto [p, ec] = std::from_chars(b, e, out);
  if (ec != std::errc() || p != e) return std::nullopt;
  return out;
}

// Upsert (inside a write transaction).
inline void kv_set(Tx& tx, std::string_view key, std::string_view value, int64_t now_ms) {
  tx.run(
      "INSERT INTO kv(key,value,updated_at) VALUES(?,?,?) "
      "ON CONFLICT(key) DO UPDATE SET value=excluded.value, updated_at=excluded.updated_at",
      key, value, now_ms);
}
inline void kv_set_i64(Tx& tx, std::string_view key, int64_t value, int64_t now_ms) {
  kv_set(tx, key, std::to_string(value), now_ms);
}

// Removes `key`; true when a row was deleted.
inline bool kv_delete(Tx& tx, std::string_view key) {
  tx.run("DELETE FROM kv WHERE key=?", key);
  return tx.changes() > 0;
}

}  // namespace azm::db
