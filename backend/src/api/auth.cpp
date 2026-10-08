// Owner: WP-D
// Auth endpoints (DESIGN D2; docs/CONTRACTS.md §B). Scrypt never runs inside a transaction and
// always under a ScryptSlot; WS revocations happen after COMMIT.
#include "api/common.hpp"
#include "api/dto.hpp"
#include "api/handlers.hpp"
#include "core/address.hpp"
#include "core/crypto.hpp"
#include "core/errors.hpp"
#include "core/strings.hpp"
#include "db/sqlite.hpp"
#include "http/throttle.hpp"
#include "repo/accounts.hpp"
#include "services.hpp"

namespace azm::api {

namespace json = boost::json;

namespace {

constexpr int64_t kDayMs = 24LL * 3600 * 1000;
constexpr std::size_t kMaxEmailBytes = 254;  // RFC 5321 path limit; longer cannot be an account

[[noreturn]] void too_many_attempts(int retry_after) {
  json::object d;
  d["retry_after"] = retry_after;
  throw ApiError::too_many("too_many_attempts", "尝试次数过多，请稍后再试", std::move(d));
}

// Me for `user_id` from one consistent snapshot; 401 when the user vanished meanwhile.
json::object load_me(Services& svc, int64_t user_id) {
  const ServerInfo server = server_info(svc);
  auto me = svc.db.read([&](db::Conn& c) -> std::optional<json::object> {
    auto u = repo::get_user(c, user_id);
    if (!u) return std::nullopt;
    const auto settings = repo::get_settings(c, user_id);
    const auto identities = repo::identities_for_user(c, user_id);
    return me_json(*u, settings, identities, server);
  });
  if (!me) throw ApiError::unauthorized();
  return std::move(*me);
}

std::string user_agent(const http::Request& req) {
  return std::string(req.header("User-Agent").value_or(""));
}

}  // namespace

// POST /api/auth/login
http::Response auth_login(http::Ctx& ctx) {
  Services& svc = ctx.svc;
  const LoginRequest in = parse_login(ctx.body_object());
  const std::string email = normalize_email(in.email);
  const std::string& ip = ctx.req.remote_ip;
  http::LoginThrottle* throttle = svc.login_throttle;
  // Something that cannot be an account address is never looked up nor tracked per email (an
  // attacker-sized string must not live in the throttle, SEC-2); it still costs a dummy scrypt
  // and counts against the client IP.
  const bool plausible = email.size() <= kMaxEmailBytes && is_valid_email(email);
  const std::string_view throttle_email = plausible ? std::string_view(email) : std::string_view();

  if (throttle != nullptr)
    if (auto retry_after = throttle->check(throttle_email, ip)) too_many_attempts(*retry_after);

  const auto user = plausible ? svc.db.read([&](db::Conn& c) { return repo::find_user_by_email(c, email); })
                              : std::optional<repo::User>();
  bool ok = false;
  {
    detail::ScryptSlot slot(svc);
    // Unknown emails verify against a dummy hash: same cost, no user enumeration by timing.
    ok = crypto::password_verify(in.password, user ? user->password_hash : detail::dummy_password_hash());
  }
  const int64_t now = svc.now_ms();
  // A disabled account answers the same whatever the password (SEC-7): the 403 must not confirm
  // a correct guess, and every attempt counts against the throttle like a wrong password.
  if (user && user->disabled) {
    if (throttle != nullptr) throttle->record_failure(throttle_email, ip);
    svc.db.write([&](db::Tx& tx) {
      repo::audit(tx, user->id, "login.failure", email, {{"reason", ok ? "disabled" : "disabled_bad_password"}},
                  ip, now);
    });
    throw ApiError::forbidden("account_disabled", "该账号已被停用，请联系管理员");
  }
  if (!user || !ok) {
    if (throttle != nullptr) throttle->record_failure(throttle_email, ip);
    svc.db.write([&](db::Tx& tx) {
      repo::audit(tx, user ? std::optional<int64_t>(user->id) : std::nullopt, "login.failure",
                  utf8_truncate(email, kMaxEmailBytes),
                  {{"reason", user ? "bad_password" : plausible ? "unknown_email" : "invalid_email"}}, ip, now);
    });
    throw ApiError::unauthorized("invalid_credentials", "邮箱或密码错误");
  }
  if (throttle != nullptr) throttle->record_success(email, ip);

  const int64_t ttl = static_cast<int64_t>(svc.cfg.session_ttl_days) * kDayMs;
  const auto created = svc.db.write([&](db::Tx& tx) {
    auto s = repo::create_session(tx, user->id, ttl, user_agent(ctx.req), ip, now);
    repo::record_login(tx, user->id, now);
    repo::audit(tx, user->id, "login.success", user->email, {{"session_id", s.session.id}}, ip, now);
    return s;
  });
  return http::Response::json(
      login_response(created.token, created.session.expires_at, load_me(svc, user->id)));
}

// POST /api/auth/logout
http::Response auth_logout(http::Ctx& ctx) {
  const auto& p = ctx.user();
  const int64_t now = ctx.svc.now_ms();
  ctx.svc.db.write([&](db::Tx& tx) {
    repo::revoke_session(tx, p.session_id);
    repo::audit(tx, p.user_id, "logout", p.email, {{"session_id", p.session_id}}, ctx.req.remote_ip,
                now);
  });
  ctx.svc.notifier.revoke_session(p.session_id);  // after COMMIT: closes this session's sockets
  return http::Response::no_content();
}

// GET  /api/auth/me
http::Response auth_me(http::Ctx& ctx) {
  return http::Response::json(load_me(ctx.svc, ctx.user().user_id));
}

// POST /api/auth/password
http::Response auth_change_password(http::Ctx& ctx) {
  Services& svc = ctx.svc;
  const auto& p = ctx.user();
  const PasswordChange in = parse_password_change(ctx.body_object());
  validate_new_password(in.new_password);
  const std::string& ip = ctx.req.remote_ip;
  http::LoginThrottle* throttle = svc.login_throttle;

  const auto user = svc.db.read([&](db::Conn& c) { return repo::get_user(c, p.user_id); });
  if (!user) throw ApiError::unauthorized();
  // A stolen session must not allow brute-forcing the current password: same throttle as login.
  if (throttle != nullptr)
    if (auto retry_after = throttle->check(user->email, ip)) too_many_attempts(*retry_after);

  std::string new_hash;
  {
    detail::ScryptSlot slot(svc);
    if (!crypto::password_verify(in.current_password, user->password_hash)) {
      if (throttle != nullptr) throttle->record_failure(user->email, ip);
      const int64_t now = svc.now_ms();
      svc.db.write([&](db::Tx& tx) {
        repo::audit(tx, user->id, "password.change_failed", user->email, {}, ip, now);
      });
      // 403, never 401: a 401 would end the client's (still valid) session (Addendum B).
      throw ApiError::forbidden("invalid_credentials", "当前密码不正确");
    }
    new_hash = crypto::password_hash(in.new_password);
  }

  const int64_t now = svc.now_ms();
  const auto revoked = svc.db.write([&](db::Tx& tx) {
    repo::UserPatch patch;
    patch.password_hash = new_hash;
    repo::update_user(tx, user->id, patch, now);
    auto ids = repo::revoke_all_sessions(tx, user->id, p.session_id);
    repo::audit(tx, user->id, "password.change", user->email,
                {{"revoked_sessions", static_cast<int64_t>(ids.size())}}, ip, now);
    return ids;
  });
  for (int64_t sid : revoked) svc.notifier.revoke_session(sid);
  return http::Response::no_content();
}

}  // namespace azm::api
