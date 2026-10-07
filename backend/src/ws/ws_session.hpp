// Owner: WP-A
//
// WebSocket session for GET /api/ws (DESIGN A4, docs/API.md "WebSocket protocol"):
//  1. Origin must be allowed by the CORS policy, else HTTP 403 and no upgrade.
//  2. accept with timeout::suggested(role_type::server) and read_message_max = cfg.ws_max_message_bytes.
//  3. Within cfg.ws_auth_timeout_ms the first message must be {"type":"auth","token":"…"};
//     http::authenticate runs on the db pool. Failure / timeout → close 4401.
//  4. Hub::attach, then send {"type":"ready","user_id","server_time"}.
//  5. Read loop: {"type":"ping"} → {"type":"pong"}; anything else ignored.
//  5b. Re-authentication: the raw token is kept in memory; every 5 min (a strand timer, or the
//     next ping once 5 min have passed) http::authenticate runs again on the db pool. nullopt
//     (session revoked by another process such as `azmail reset-password`, expired, purged or
//     the user disabled) → send {"type":"session.revoked"} and close 4401. Hub revocation
//     remains the fast path within this process.
//  6. Writes go through a strand-serialized queue capped at cfg.ws_queue_cap; overflow closes.
//  7. On exit: Hub::detach.
#pragma once

#include "http/cors.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/http/empty_body.hpp>
#include <boost/beast/http/message.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

namespace azm {
struct Config;
struct Services;
}  // namespace azm

namespace azm::ws {

class Hub;

struct WsDeps {
  const Config& cfg;
  Services& svc;
  Hub& hub;
  boost::asio::thread_pool& db_pool;  // session-token lookup runs here, never on the strand
  const http::CorsPolicy& cors;       // Origin allowlist
  // Additive (WP-A): period of the step-5b re-authentication (tests shorten it).
  std::chrono::milliseconds reauth_interval{std::chrono::minutes(5)};
};

// Close codes besides kCloseAuthFailed (events.hpp) used by the session.
inline constexpr std::uint16_t kCloseGoingAway = 1001;  // server shutdown
inline constexpr std::uint16_t kClosePolicy = 1008;     // send-queue overflow, too many sessions
inline constexpr std::uint16_t kCloseInternal = 1011;   // unexpected server error

// Takes over a connection whose upgrade request header was already read by http::run_session.
// Returns when the WebSocket (or the rejected handshake) is finished. Never throws.
boost::asio::awaitable<void> run_ws_session(
    boost::beast::tcp_stream stream,
    boost::beast::http::request<boost::beast::http::empty_body> upgrade, WsDeps& deps);

// ---- pure helpers -----------------------------------------------------------------------------
enum class ClientMessageType { Auth, Ping, Other, Invalid };
struct ClientMessage {
  ClientMessageType type = ClientMessageType::Invalid;  // Invalid = not a JSON object with "type"
  std::string token;                                    // Auth only
};
// Parses one client text frame.
ClientMessage parse_client_message(std::string_view text);

}  // namespace azm::ws
