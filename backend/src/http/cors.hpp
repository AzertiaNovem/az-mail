// Owner: WP-A
//
// CORS for the API origin (DESIGN §3, E2E 30). Exact-match allowlist from cfg.cors_origins
// (AZMAIL_CORS_ORIGINS); no wildcard, no credentials (auth is a Bearer header). Every response
// carries "Vary: Origin". The same allowlist gates WebSocket upgrades (browsers don't apply
// CORS to WS, DESIGN A4).
#pragma once

#include "http/types.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace azm {
struct Config;
}

namespace azm::http {

struct CorsPolicy {
  std::vector<std::string> allowed_origins;  // e.g. "https://mail.example.com" (scheme://host[:port])
  std::string allow_methods = "GET, POST, PUT, PATCH, DELETE, OPTIONS";
  std::string allow_headers = "Authorization, Content-Type";
  std::string expose_headers = "X-Request-Id, Retry-After, Content-Disposition";
  int max_age_sec = 600;  // Access-Control-Max-Age for preflights
};

// Policy from cfg.cors_origins (entries trimmed, trailing '/' removed).
CorsPolicy cors_policy_from(const Config& cfg);

// True when `origin` exactly matches an allowed origin (scheme and host compared
// case-insensitively, port exactly). "null" and empty origins are never allowed.
bool origin_allowed(const CorsPolicy&, std::string_view origin);

// Adds "Vary: Origin" always and, when `origin` is allowed, Access-Control-Allow-Origin
// (echoing the origin) and Access-Control-Expose-Headers. Disallowed origins get no ACAO.
void apply_cors(const CorsPolicy&, std::optional<std::string_view> origin, Response& res);

// Response to an OPTIONS preflight: 204 with Allow-Origin/Methods/Headers/Max-Age when the
// origin is allowed; 403 error "forbidden" (still with Vary: Origin) otherwise. Requested
// headers outside allow_headers are not echoed (the browser then blocks the request).
Response preflight(const CorsPolicy&, std::optional<std::string_view> origin,
                   std::optional<std::string_view> request_method,
                   std::optional<std::string_view> request_headers);

}  // namespace azm::http
