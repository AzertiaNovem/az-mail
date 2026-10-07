// Owner: WP-D (internal to src/api; not a contract header)
//
// Small helpers shared by the handler files: scrypt permits, error mapping for blob stores and
// Resend, audit shortcuts and query-string parsing.
#pragma once

#include "core/blob_store.hpp"
#include "core/errors.hpp"
#include "http/throttle.hpp"
#include "http/types.hpp"
#include "resend/types.hpp"
#include "services.hpp"

#include <boost/json/object.hpp>

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace azm::api::detail {

// Bounds concurrent scrypt operations (DESIGN D2). Uses the LoginThrottle's semaphore when one
// is wired (production), else a process-wide fallback with cfg.scrypt_concurrency slots (tests,
// tools), so a misconfiguration can never turn into an unbounded CPU sink.
class ScryptSlot {
 public:
  explicit ScryptSlot(const Services& svc);
  ~ScryptSlot();
  ScryptSlot(const ScryptSlot&) = delete;
  ScryptSlot& operator=(const ScryptSlot&) = delete;

 private:
  std::optional<http::LoginThrottle::ScryptPermit> permit_;
  bool fallback_ = false;
};

// Encoded hash of a fixed password, verified against when the login email is unknown so the
// response time does not reveal which emails exist. Computed once (lazily).
const std::string& dummy_password_hash();

// crypto::password_hash under a ScryptSlot.
std::string hash_password(const Services& svc, std::string_view password);

// BlobError → 503 storage_unavailable (retryable) / 502 storage_error. Logs the cause.
[[noreturn]] void throw_blob_error(const BlobError& e, std::string_view op);

// resend::Error → 502 resend_error (logged with kind/name; never with request data).
[[noreturn]] void throw_resend_error(const resend::Error& e, std::string_view op);

// 403 "forbidden" unless the caller is an admin (defence in depth: dispatch checks it too).
const http::Principal& require_admin(const http::Ctx& ctx);

// Non-empty query value, or nullopt for absent / empty parameters.
std::optional<std::string> query_nonempty(const http::Ctx& ctx, std::string_view key);

// Strict "0|1|true|false" flag; absent/empty → `def`; anything else → 400 invalid_field.
bool query_flag(const http::Ctx& ctx, std::string_view key, bool def);

// {"field": field} for invalid_field errors.
boost::json::object field_detail(std::string_view field);

}  // namespace azm::api::detail
