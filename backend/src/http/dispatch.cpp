// Owner: WP-A
// Request dispatch on a blocking pool: authentication, admin check, handler call and error
// mapping (DESIGN "Request lifecycle" step 6, dispatch.hpp).
#include "http/dispatch.hpp"

#include "core/log.hpp"
#include "core/strings.hpp"
#include "db/sqlite.hpp"
#include "repo/accounts.hpp"
#include "services.hpp"

#include <charconv>

namespace azm::http {

namespace {
constexpr std::size_t kMaxTokenBytes = 512;

std::optional<int64_t> to_i64(std::optional<std::string_view> s) {
  if (!s || s->empty()) return std::nullopt;
  int64_t v = 0;
  auto [p, ec] = std::from_chars(s->data(), s->data() + s->size(), v);
  if (ec != std::errc() || p != s->data() + s->size()) return std::nullopt;
  return v;
}

std::optional<std::string_view> query_of(const Request& req, std::string_view key) {
  auto it = req.query.find(std::string(key));
  if (it == req.query.end()) return std::nullopt;
  return std::string_view(it->second);
}

// AuthReq::Signed (DESIGN D5): the HMAC is checked before any handler code runs, for the two
// signed URL shapes of route_table() — "/api/files/raw/:messageId" (raw) and "/api/files/:id"
// (file, d=i|a). The handler verifies again and checks that the user is active and owns the
// object (dispatch.hpp contract); unknown shapes are left entirely to the handler.
bool signed_request_valid(const Request& req, const Params& params, Services& svc) {
  const auto uid = to_i64(query_of(req, "u"));
  const auto exp = to_i64(query_of(req, "exp"));
  const auto sig = query_of(req, "sig");
  if (!uid || *uid <= 0 || !exp || !sig || sig->empty()) return false;
  const int64_t now = svc.now_ms();
  if (auto it = params.find("messageId"); it != params.end()) {
    const auto id = to_i64(std::string_view(it->second));
    return id && svc.signed_urls.verify_raw(*id, *uid, *exp, *sig, now);
  }
  if (auto it = params.find("id"); it != params.end()) {
    const auto id = to_i64(std::string_view(it->second));
    const auto d = query_of(req, "d");
    if (!id || !d || d->size() != 1) return false;
    return svc.signed_urls.verify_file(*id, *uid, (*d)[0], *exp, *sig, now);
  }
  return true;
}
}  // namespace

std::optional<std::string> bearer_token(const Request& req) {
  auto h = req.header("Authorization");
  if (!h) return std::nullopt;
  std::string_view v = trim(*h);
  constexpr std::string_view kScheme = "bearer";
  if (v.size() <= kScheme.size() || !istarts_with(v, kScheme)) return std::nullopt;
  // The scheme must be followed by whitespace ("Bearertoken" is not a Bearer credential).
  if (v[kScheme.size()] != ' ' && v[kScheme.size()] != '\t') return std::nullopt;
  std::string_view tok = trim(v.substr(kScheme.size()));
  if (tok.empty()) return std::nullopt;
  return std::string(tok);
}

std::optional<Principal> authenticate(Services& svc, std::string_view token) {
  if (token.empty() || token.size() > kMaxTokenBytes) return std::nullopt;
  if (svc.session_resolver != nullptr) return svc.session_resolver->resolve(svc, token);

  const int64_t now = svc.now_ms();
  const int64_t touch_ms = static_cast<int64_t>(svc.cfg.session_touch_interval_sec) * 1000;
  auto found = svc.db.read([&](db::Conn& c) { return repo::find_session(c, token, now, touch_ms); });
  if (!found) return std::nullopt;
  if (found->needs_touch) {
    // Sliding expiry, at most once per touch interval (DESIGN A5: GETs must not all be writes).
    const int64_t ttl_ms = static_cast<int64_t>(svc.cfg.session_ttl_days) * 24 * 3600 * 1000;
    try {
      const bool alive = svc.db.write(
          [&](db::Tx& tx) { return repo::touch_session(tx, found->session.id, now, ttl_ms); });
      if (!alive) return std::nullopt;  // revoked between the lookup and the touch
    } catch (const db::BusyError&) {
      // A missed touch only delays the sliding expiry; the session is still valid.
      log::warn("session touch skipped (database busy)", {{"session_id", found->session.id}});
    }
  }
  Principal p;
  p.user_id = found->user.id;
  p.session_id = found->session.id;
  p.is_admin = found->user.is_admin;
  p.email = found->user.email;
  return p;
}

Response dispatch(const Route& route, const Request& req, Params params, Services& svc) noexcept {
  try {
    log::ScopedRequestId rid(req.request_id);
    try {
      std::optional<Principal> principal;
      if (route.auth == AuthReq::User || route.auth == AuthReq::Admin) {
        auto token = bearer_token(req);
        if (!token) return Response::from_error(ApiError::unauthorized());
        principal = authenticate(svc, *token);
        if (!principal) return Response::from_error(ApiError::unauthorized());
        if (route.auth == AuthReq::Admin && !principal->is_admin)
          return Response::from_error(ApiError::forbidden());
      } else if (route.auth == AuthReq::Signed && !signed_request_valid(req, params, svc)) {
        return Response::from_error(ApiError::forbidden("invalid_signature", "链接无效或已过期"));
      }
      Ctx ctx{req, svc, std::move(params), std::move(principal)};
      return route.handler(ctx);
    } catch (const ApiError& e) {
      if (e.status >= 500)
        log::warn("request failed", {{"route", route.pattern}, {"status", e.status}, {"code", e.code}});
      return Response::from_error(e);
    } catch (const db::BusyError&) {
      log::warn("database busy", {{"route", route.pattern}});
      return Response::from_error(ApiError::unavailable());
    } catch (const std::exception& e) {
      // Never the body or the query string: only the route pattern and the exception text.
      log::error("unhandled exception in handler",
                 {{"route", route.pattern}, {"method", std::string(beast::http::to_string(req.method))},
                  {"error", e.what()}});
      return Response::from_error(ApiError::internal());
    } catch (...) {
      log::error("unhandled non-standard exception in handler", {{"route", route.pattern}});
      return Response::from_error(ApiError::internal());
    }
  } catch (...) {
    // Only reachable when building the error response itself failed (e.g. bad_alloc).
    Response r;
    r.status = 500;
    r.body = std::string(R"({"error":{"code":"internal_error","message":"服务器内部错误","details":{}}})");
    return r;
  }
}

}  // namespace azm::http
