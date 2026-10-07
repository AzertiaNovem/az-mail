// Owner: WP-A
//
// Request dispatch on a blocking pool (DESIGN "Request lifecycle" step 6). Always called through
// http::run_blocking on the pool selected by Route::exec, never on the IO strand.
#pragma once

#include "http/types.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace azm {
struct Services;
}

namespace azm::http {

// Runs one request:
//  1. Sets the thread's log request id (log::ScopedRequestId(req.request_id)).
//  2. Auth per route.auth:
//       None / Signed / Webhook → Ctx::principal unset (the handler verifies signatures);
//       User / Admin → bearer_token() + authenticate(); missing or invalid → 401 "unauthorized";
//       Admin with !is_admin → 403 "forbidden".
//  3. Calls route.handler with Ctx{req, svc, params, principal}.
//  4. ApiError → Response::from_error. db::BusyError → 503 "service_unavailable". Any other
//     exception (incl. NotImplemented) → 500 "internal_error", logged with the request id and the
//     exception's what() (never the request body or query string).
// noexcept: always returns a Response.
Response dispatch(const Route& route, const Request& req, Params params, Services& svc) noexcept;

// Token from "Authorization: Bearer <token>" (scheme case-insensitive, surrounding spaces
// trimmed); nullopt when absent, empty or another scheme.
std::optional<std::string> bearer_token(const Request& req);

// Resolves a raw session token: repo::find_session (sha256 lookup, expiry, user active). When
// the session's last_seen_at is older than cfg.session_touch_interval_sec it is touched
// (sliding expiry, cfg.session_ttl_days) in a separate short Pool::write. nullopt for unknown,
// expired or disabled. Also used by ws::run_ws_session for first-message auth.
std::optional<Principal> authenticate(Services& svc, std::string_view token);

}  // namespace azm::http
