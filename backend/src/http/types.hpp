// Owner: WP-A (frozen contract from WP0; additive changes only)
//
// Transport-neutral request/response types shared by the HTTP session (WP-A) and every route
// handler (WP-D). Handlers never see Beast streams: they get a Ctx and return a Response; the
// session adds CORS, X-Request-Id, Cache-Control: no-store and nosniff, then writes a
// string_body or file_body (DESIGN A1).
#pragma once

#include "core/errors.hpp"

#include <boost/beast/http/fields.hpp>
#include <boost/beast/http/verb.hpp>
#include <boost/container/flat_map.hpp>
#include <boost/json/object.hpp>
#include <boost/json/value.hpp>
#include <boost/url/url.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace azm {
struct Services;
}

namespace azm::http {

namespace beast = boost::beast;

// Who may call a route.
enum class AuthReq {
  None,     // public (login, health)
  User,     // Bearer session of an active user → Ctx::principal set
  Admin,    // User + is_admin (403 "forbidden" otherwise)
  Signed,   // no Bearer; the handler verifies the HMAC signed URL (d, u, exp, sig query)
  Webhook,  // no Bearer; the handler verifies the Svix signature over the raw body
};

// How the session reads the request body before dispatch.
enum class BodyMode {
  None,  // body read and discarded, ≤ Route::body_limit (GET: 0; bodyless POST/DELETE: 4 KiB)
  Json,  // string body, ≤ Route::body_limit; handler calls Ctx::body_object()
  Raw,   // string body, ≤ Route::body_limit, not parsed (webhook: exact bytes for Svix)
  File,  // streamed to BlobStore::tmp_dir() via file_body with sha256 while streaming (uploads)
};

// Which blocking pool runs the handler (DESIGN A1: Resend/R2 network calls never starve DB work).
//   Db    — everything that only touches SQLite (cfg.db_threads);
//   Net   — the few handlers that call Resend synchronously: cancel-schedule, reschedule,
//           domain status (cfg.net_threads);
//   Files — file I/O that may be a synchronous R2 GET/PUT (proxy cache misses, uploads):
//           /api/files/:id, /api/files/raw/:id, /api/messages/:id/raw, POST /api/attachments
//           (cfg.files_threads), so a thread full of inline images cannot starve Net or Db.
enum class Exec { Db, Net, Files };

inline constexpr std::string_view to_string(AuthReq a) {
  switch (a) {
    case AuthReq::None: return "none";
    case AuthReq::User: return "user";
    case AuthReq::Admin: return "admin";
    case AuthReq::Signed: return "signed";
    case AuthReq::Webhook: return "webhook";
  }
  return "none";
}
inline constexpr std::string_view to_string(BodyMode b) {
  switch (b) {
    case BodyMode::None: return "none";
    case BodyMode::Json: return "json";
    case BodyMode::Raw: return "raw";
    case BodyMode::File: return "file";
  }
  return "none";
}
inline constexpr std::string_view to_string(Exec e) {
  switch (e) {
    case Exec::Db: return "db";
    case Exec::Net: return "net";
    case Exec::Files: return "files";
  }
  return "db";
}

// Authenticated caller (resolved by http::dispatch from the Bearer token).
struct Principal {
  int64_t user_id = 0;     // users.id
  int64_t session_id = 0;  // sessions.id (logout / password change keep-current)
  bool is_admin = false;   // users.is_admin at request time
  std::string email;       // users.email (login address)
};

// Decoded path / query parameters (first occurrence of a query key wins).
using Params = boost::container::flat_map<std::string, std::string>;

struct Request {
  beast::http::verb method = beast::http::verb::get;
  std::string target;    // raw request-target, e.g. "/api/threads?folder=inbox&limit=50"
  std::string path;      // percent-decoded path without the query, e.g. "/api/threads"
  boost::urls::url url;  // parsed target (relative-ref)
  Params query;          // percent-decoded query parameters ('+' is NOT a space)
  beast::http::fields headers;
  std::string body;  // BodyMode::Json / Raw
  // BodyMode::File: staged temp file in BlobStore::tmp_dir() (a handler may consume it via
  // BlobStore::put_file; the session deletes whatever is left after the handler returns/throws).
  std::optional<std::filesystem::path> body_file;
  std::optional<std::string> body_sha256;  // BodyMode::File: lowercase hex sha256 of body_file (Addendum A)
  std::size_t body_size = 0;               // bytes read (all modes)
  std::string remote_ip;   // client IP (X-Forwarded-For honoured only from cfg.trusted_proxies)
  std::string request_id;  // X-Request-Id (generated per request; echoed in the response)

  // Header value (case-insensitive name), first occurrence; nullopt when absent.
  std::optional<std::string_view> header(std::string_view name) const;

  // Test/session helper: builds a Request for `target`, filling target, url, path and query.
  // Throws std::invalid_argument when the target is not a valid origin-form request-target.
  static Request make(beast::http::verb method, std::string_view target);
};

// File body (served with beast file_body). `remove_after_send` deletes the file once the
// response has been written (temporary downloads); never set it for cache/blob store paths.
struct FileRef {
  std::filesystem::path path;
  bool remove_after_send = false;
};

struct Response {
  unsigned status = 200;
  std::string content_type = "application/json; charset=utf-8";
  std::vector<std::pair<std::string, std::string>> headers;  // extra headers (appended as-is)
  std::variant<std::monostate, std::string, FileRef> body;

  // serialize(v) with the default content type.
  static Response json(const boost::json::value&, unsigned status = 200);
  // 204, no body, no Content-Type.
  static Response no_content();
  // {"error":{"code","message","details"}} with `status`.
  static Response error(unsigned status, std::string_view code, std::string_view msg,
                        boost::json::object details = {});
  // 200 file body; adds "Content-Disposition: <disposition>" when non-empty.
  static Response file(std::filesystem::path, std::string content_type, std::string disposition);

  // ---- additive helpers ---------------------------------------------------------------------
  static Response from_error(const ApiError& e);  // error(e.status, e.code, e.message, e.details)
  static Response text(std::string body, std::string content_type = "text/plain; charset=utf-8",
                       unsigned status = 200);
  // `status` (302 default) with Location; no body.
  static Response redirect(std::string location, unsigned status = 302);

  // Appends a header; returns *this for chaining.
  Response& add_header(std::string name, std::string value);
  // First extra header with that (case-insensitive) name.
  std::optional<std::string_view> find_header(std::string_view name) const;
};

// Per-request handler context.
struct Ctx {
  const Request& req;
  Services& svc;
  Params params;                         // path parameters from the route pattern (":id" → "id")
  std::optional<Principal> principal;    // set for AuthReq::User / Admin

  // The authenticated caller. Throws ApiError(401, "unauthorized") when absent.
  const Principal& user() const;
  // Path parameter as a positive int64. Throws ApiError(400, "invalid_field", {field: param})
  // when missing, non-numeric, zero/negative or out of range.
  int64_t id(std::string_view param) const;
  // Decoded query parameter (first occurrence); nullopt when absent. Points into req.query.
  std::optional<std::string_view> query(std::string_view key) const;
  // Integer query parameter; nullopt when absent or empty. Throws ApiError(400,
  // "invalid_field", {field: key}) when present but not an integer.
  std::optional<int64_t> query_int(std::string_view key) const;
  // Parses req.body as a JSON object. Throws ApiError(400, "invalid_json") when the body is
  // empty, malformed or not an object.
  boost::json::object body_object() const;
};

using Handler = std::function<Response(Ctx&)>;

struct Route {
  beast::http::verb method;
  std::string pattern;     // "/api/threads/:id"; literal segments win over ":param" segments
  AuthReq auth;
  BodyMode body;
  std::size_t body_limit;  // max body bytes (0 for BodyMode::None); over → 413 payload_too_large
  Exec exec;
  Handler handler;
};

}  // namespace azm::http
