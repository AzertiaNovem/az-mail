// Owner: WP-A
//
// Per-connection coroutine (DESIGN A1/A2, "Request lifecycle"):
//  1. read the header with request_parser<empty_body> (cfg.max_header_bytes, cfg.header_timeout_sec);
//  2. GET /api/ws with Upgrade: websocket → ws::run_ws_session (ownership of the stream moves);
//  3. OPTIONS → http::preflight inline (no pool);
//  4. Router::match → 404 "not_found" / 405 "method_not_allowed" (+ Allow);
//  5. Content-Length over Route::body_limit → 413 "payload_too_large" before reading the body;
//     5a. BodyMode::File routes with AuthReq::User/Admin: BEFORE reading the body,
//         co_await run_blocking(db_pool, http::authenticate(bearer_token)) — missing/invalid
//         → 401 "unauthorized" (Admin without is_admin → 403 "forbidden") without reading the
//         body, sent with Connection: close (the unread body makes the stream unusable). So
//         anonymous clients can never stage bytes on disk;
//     then move-construct request_parser<string_body | file_body> and read in an
//     async_read_some loop re-arming cfg.body_idle_timeout_sec per chunk (BodyMode::File streams
//     to make_staging_path(BlobStore::tmp_dir()) with a running sha256);
//  6. in-flight cap (cfg.inflight_cap) → 503 "service_unavailable"; else
//     co_await run_blocking(pool for route.exec: Db → db_pool, Net → net_pool, Files →
//     files_pool (net_pool when null), dispatch);
//  7. after the handler returns or throws — and on every early exit once staging began (body
//     read error/timeout/limit, 503, dispatch 4xx/5xx) — run_session removes req.body_file if
//     it still exists (std::filesystem::remove with error_code; put_file normally consumed it);
//     then finalize_response (CORS + standard headers) and write string_body / file_body;
//     a FileRef with remove_after_send is removed after the write completes OR fails;
//  8. keep-alive loop (cfg.keepalive_timeout_sec idle).
// Crash leftovers in tmp_dir() are swept by gc.housekeeping (files older than 24 h) and once at
// App::start.
// Never logs query strings of /api/files/* (signed URLs) or bodies.
//
// Implementation notes (WP-A):
//  * Header errors: over cfg.max_header_bytes → 431 "header_too_large"; malformed → 400
//    "bad_request"; header timeout / EOF → the socket is closed without a response.
//  * The header parser has no body limit (Beast's 1 MiB default would reject a large
//    Content-Length while parsing the header); the route limit applies from step 5 on.
//  * Expect: 100-continue is answered right before the body is read (after 413 / pre-auth).
//  * Responses sent while request bytes are still unread carry Connection: close and are
//    followed by a lingering close (half-close, drain ≤ 4 MiB / 2 s) so the client sees them.
//  * FileRef bodies: Accept-Ranges: bytes; a single "bytes=" Range on a 200 GET → 206 with
//    Content-Range, unsatisfiable → 416 "range_not_satisfiable"; multi-range / If-Range → 200.
//    A file that cannot be opened → 500 "internal_error".
//  * 204/304 carry no Content-Length; Response::redirect → 302 with Content-Length: 0.
#pragma once

#include "http/cors.hpp"
#include "http/server.hpp"
#include "http/types.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

namespace azm::http {

namespace detail {
struct ConnHandle;  // session.cpp: executor + idle flag of one live connection
}

// State shared by all connections of one Server (owned by Server::Impl).
struct SessionShared {
  ServerDeps deps;
  CorsPolicy cors;                          // from cfg.cors_origins
  std::atomic<std::size_t> inflight{0};     // requests on blocking pools
  std::atomic<std::size_t> connections{0};  // open connections
  std::atomic<bool> stopping{false};        // set by Server::stop (disables keep-alive)

  // ---- additive (WP-A) ----------------------------------------------------------------------
  // Live connections, so Server::stop can close the ones idling between requests
  // (close_idle_connections). Registered/unregistered by run_session.
  std::mutex conns_mu;
  std::unordered_map<std::uint64_t, std::weak_ptr<detail::ConnHandle>> conns;  // guarded by conns_mu
  std::uint64_t next_conn_id = 0;                                              // guarded by conns_mu
};

// Serves one accepted connection until it closes. Never throws (errors close the socket).
// The socket's executor must be a strand (Server spawns each connection on its own strand).
boost::asio::awaitable<void> run_session(boost::asio::ip::tcp::socket socket, SessionShared& shared);

// Cancels every connection that is waiting for its next request (shutdown, A7). Connections
// in the middle of a request finish it and then close (keep-alive is off once `stopping`).
// Thread-safe.
void close_idle_connections(SessionShared& shared);

// ---- pure helpers (unit-tested by WP-A) -----------------------------------------------------

// Client IP: when `peer_ip` is in `trusted_proxies` (exact IPs or CIDRs) and X-Forwarded-For is
// present, the right-most XFF entry that is not itself trusted; otherwise `peer_ip`.
std::string resolve_client_ip(std::string_view peer_ip, std::optional<std::string_view> xff,
                              std::span<const std::string> trusted_proxies);

// 413 "payload_too_large" when the declared Content-Length exceeds `route.body_limit`
// (BodyMode::None bodies are read and discarded within that limit); nullopt when reading may
// proceed. Chunked bodies are limited while reading.
std::optional<Response> precheck_body(const Route& route, std::optional<std::uint64_t> content_length);

// Adds the headers every response carries: X-Request-Id, X-Content-Type-Options: nosniff,
// Cache-Control: no-store (unless the handler set Cache-Control), Referrer-Policy: no-referrer
// (unless set), then apply_cors().
void finalize_response(Response& res, std::string_view request_id, const CorsPolicy& cors,
                       std::optional<std::string_view> origin);

// New random request id (16 hex chars).
std::string make_request_id();

// ---- additive pure helpers (WP-A) -----------------------------------------------------------

// Single byte range of a `Range` header (RFC 9110 §14.1.2), inclusive bounds.
struct ByteRange {
  std::uint64_t first = 0;
  std::uint64_t last = 0;
};
enum class RangeResult {
  None,           // no usable single "bytes=" range (absent, malformed, multi-range): send 200
  Ok,             // `out` is set: send 206 with Content-Range
  Unsatisfiable,  // send 416 with "Content-Range: bytes */<size>"
};
RangeResult parse_range(std::string_view header, std::uint64_t size, ByteRange& out);

// True when `ip` equals one of the entries or lies in one of the CIDR blocks ("10.0.0.0/8",
// "fd00::/8"). IPv4-mapped IPv6 addresses compare as IPv4. Invalid entries never match.
bool ip_in_list(std::string_view ip, std::span<const std::string> entries);

}  // namespace azm::http
