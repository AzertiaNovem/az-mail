// Owner: WP-A — http::dispatch / bearer_token / authenticate (Ctx-level, no sockets).
#include "core/log.hpp"
#include "db/sqlite.hpp"
#include "http/dispatch.hpp"
#include "http_harness.hpp"
#include "repo/accounts.hpp"
#include "test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <boost/json/parse.hpp>

using namespace azm;
using V = boost::beast::http::verb;

namespace {

http::Request req_with_auth(std::optional<std::string> authorization) {
  auto r = http::Request::make(V::get, "/api/x?secret=sig");
  r.request_id = "req-123";
  if (authorization) r.headers.set(boost::beast::http::field::authorization, *authorization);
  return r;
}

std::string code_of(const http::Response& r) {
  return std::string(boost::json::parse(std::get<std::string>(r.body)).at("error").at("code").as_string());
}

http::Route route(http::AuthReq auth, http::Handler h) {
  return {V::get, "/api/x", auth, http::BodyMode::None, 0, http::Exec::Db, std::move(h)};
}

}  // namespace

TEST_CASE("dispatch: bearer_token parsing", "[dispatch]") {
  CHECK(http::bearer_token(req_with_auth("Bearer abc")) == std::optional<std::string>("abc"));
  CHECK(http::bearer_token(req_with_auth("bearer  abc  ")) == std::optional<std::string>("abc"));
  CHECK(http::bearer_token(req_with_auth("BEARER\tabc")) == std::optional<std::string>("abc"));
  CHECK_FALSE(http::bearer_token(req_with_auth(std::nullopt)));
  CHECK_FALSE(http::bearer_token(req_with_auth("Bearer")));
  CHECK_FALSE(http::bearer_token(req_with_auth("Bearer   ")));
  CHECK_FALSE(http::bearer_token(req_with_auth("Bearerabc")));
  CHECK_FALSE(http::bearer_token(req_with_auth("Basic abc")));
  CHECK_FALSE(http::bearer_token(req_with_auth("")));
}

TEST_CASE("dispatch: auth modes, principal and error mapping", "[dispatch]") {
  test::TestServices ts;
  test::FakeResolver resolver;
  resolver.add("user-tok", 7, 70);
  resolver.add("admin-tok", 1, 10, true);
  ts.svc.session_resolver = &resolver;

  std::optional<http::Principal> seen;
  auto capture = [&](http::Ctx& ctx) {
    seen = ctx.principal;
    return http::Response::no_content();
  };

  // None / Webhook: no principal, the resolver is never called.
  for (auto mode : {http::AuthReq::None, http::AuthReq::Webhook}) {
    seen.reset();
    auto res = http::dispatch(route(mode, capture), req_with_auth("Bearer user-tok"), {}, ts.svc);
    CHECK(res.status == 204);
    CHECK_FALSE(seen);
  }
  CHECK(resolver.calls == 0);

  auto res = http::dispatch(route(http::AuthReq::User, capture), req_with_auth(std::nullopt), {}, ts.svc);
  CHECK(res.status == 401);
  CHECK(code_of(res) == "unauthorized");
  res = http::dispatch(route(http::AuthReq::User, capture), req_with_auth("Bearer nope"), {}, ts.svc);
  CHECK(res.status == 401);
  res = http::dispatch(route(http::AuthReq::User, capture), req_with_auth("Bearer user-tok"), {}, ts.svc);
  CHECK(res.status == 204);
  REQUIRE(seen);
  CHECK(seen->user_id == 7);
  CHECK(seen->session_id == 70);
  CHECK_FALSE(seen->is_admin);

  res = http::dispatch(route(http::AuthReq::Admin, capture), req_with_auth("Bearer user-tok"), {}, ts.svc);
  CHECK(res.status == 403);
  CHECK(code_of(res) == "forbidden");
  res = http::dispatch(route(http::AuthReq::Admin, capture), req_with_auth("Bearer admin-tok"), {}, ts.svc);
  CHECK(res.status == 204);
  CHECK(seen->is_admin);

  // Path params reach the handler.
  http::Params params;
  params["id"] = "42";
  res = http::dispatch(route(http::AuthReq::None,
                             [](http::Ctx& ctx) {
                               return http::Response::json(boost::json::object{{"id", ctx.id("id")}});
                             }),
                       req_with_auth(std::nullopt), params, ts.svc);
  CHECK(res.status == 200);
  CHECK(std::get<std::string>(res.body) == R"({"id":42})");
}

TEST_CASE("dispatch: signed routes are HMAC-checked before the handler", "[dispatch]") {
  test::TestServices ts;
  test::FakeResolver resolver;
  ts.svc.session_resolver = &resolver;
  int calls = 0;
  auto handler = [&](http::Ctx& ctx) {
    ++calls;
    CHECK_FALSE(ctx.principal);
    return http::Response::no_content();
  };
  http::Route file_route{V::get, "/api/files/:id", http::AuthReq::Signed, http::BodyMode::None, 0, http::Exec::Files,
                         handler};
  http::Route raw_route{V::get, "/api/files/raw/:messageId", http::AuthReq::Signed, http::BodyMode::None, 0,
                        http::Exec::Files, handler};
  const int64_t now = ts.svc.now_ms();
  auto target_of = [](const std::string& url) { return url.substr(url.find("/api/")); };
  auto run = [&](const http::Route& r, const std::string& target, http::Params params) {
    auto req = http::Request::make(V::get, target);
    req.request_id = "sig";
    return http::dispatch(r, req, std::move(params), ts.svc);
  };
  auto forbidden = [&](const http::Route& r, const std::string& target, http::Params p) {
    auto res = run(r, target, std::move(p));
    CHECK(res.status == 403);
    CHECK(code_of(res) == "invalid_signature");
  };

  const std::string good = target_of(ts.urls.file_url(12, 7, 'i', now + 60'000));
  CHECK(run(file_route, good, {{"id", "12"}}).status == 204);
  CHECK(calls == 1);
  // Another attachment id, user or disposition; tampered, missing or expired signatures.
  forbidden(file_route, good, {{"id", "13"}});
  std::string other_user = good;
  other_user.replace(other_user.find("u=7"), 3, "u=8");
  forbidden(file_route, other_user, {{"id", "12"}});
  std::string other_d = good;
  other_d.replace(other_d.find("d=i"), 3, "d=a");
  forbidden(file_route, other_d, {{"id", "12"}});
  forbidden(file_route, good.substr(0, good.size() - 2), {{"id", "12"}});
  forbidden(file_route, "/api/files/12?d=i&u=7&exp=1", {{"id", "12"}});
  forbidden(file_route, "/api/files/12", {{"id", "12"}});
  forbidden(file_route, target_of(ts.urls.file_url(12, 7, 'a', now - 1)), {{"id", "12"}});

  const std::string raw = target_of(ts.urls.raw_url(5, 7, now + 60'000));
  CHECK(run(raw_route, raw, {{"messageId", "5"}}).status == 204);
  forbidden(raw_route, raw, {{"messageId", "6"}});
  // A raw signature does not open a file URL.
  forbidden(file_route, raw, {{"id", "5"}});
  CHECK(calls == 2);
  CHECK(resolver.calls == 0);
}

TEST_CASE("dispatch: exceptions become structured errors, logged with the request id", "[dispatch]") {
  test::TestServices ts;
  std::vector<std::string> lines;
  std::mutex mu;
  log::set_sink([&](std::string_view l) {
    std::lock_guard lk(mu);
    lines.emplace_back(l);
  });
  struct Restore {
    ~Restore() { log::set_sink({}); }
  } restore;

  auto res = http::dispatch(route(http::AuthReq::None,
                                  [](http::Ctx&) -> http::Response {
                                    throw ApiError::unprocessable("unknown_local_recipient", "收件人不存在",
                                                                  {{"emails", boost::json::array{"a@x.cn"}}});
                                  }),
                            req_with_auth(std::nullopt), {}, ts.svc);
  CHECK(res.status == 422);
  auto body = boost::json::parse(std::get<std::string>(res.body)).as_object();
  CHECK(body["error"].as_object()["code"] == "unknown_local_recipient");
  CHECK(body["error"].as_object()["message"] == "收件人不存在");
  CHECK(body["error"].as_object()["details"].as_object()["emails"].as_array().size() == 1);

  res = http::dispatch(route(http::AuthReq::None, [](http::Ctx&) -> http::Response { throw db::BusyError("busy"); }),
                       req_with_auth(std::nullopt), {}, ts.svc);
  CHECK(res.status == 503);
  CHECK(code_of(res) == "service_unavailable");

  res = http::dispatch(route(http::AuthReq::None, [](http::Ctx&) -> http::Response { throw std::runtime_error("kaboom"); }),
                       req_with_auth(std::nullopt), {}, ts.svc);
  CHECK(res.status == 500);
  CHECK(code_of(res) == "internal_error");

  res = http::dispatch(route(http::AuthReq::None, [](http::Ctx&) -> http::Response { throw 42; }),
                       req_with_auth(std::nullopt), {}, ts.svc);
  CHECK(res.status == 500);

  res = http::dispatch(route(http::AuthReq::None, [](http::Ctx& ctx) { return http::Response::json(ctx.body_object()); }),
                       req_with_auth(std::nullopt), {}, ts.svc);
  CHECK(res.status == 400);
  CHECK(code_of(res) == "invalid_json");

  std::lock_guard lk(mu);
  bool found = false;
  for (const auto& l : lines) {
    if (l.find("kaboom") != std::string::npos) {
      found = true;
      CHECK(l.find("[req-123]") != std::string::npos);
      CHECK(l.find("secret=sig") == std::string::npos);  // never the query string
    }
  }
  CHECK(found);
  // The thread's request id is restored after dispatch.
  CHECK(log::request_id().empty());
}

TEST_CASE("dispatch: authenticate rejects empty/oversized tokens before any lookup", "[dispatch]") {
  test::TestServices ts;
  test::FakeResolver resolver;
  resolver.add(std::string(600, 'a'), 1, 1);
  ts.svc.session_resolver = &resolver;
  CHECK_FALSE(http::authenticate(ts.svc, ""));
  CHECK_FALSE(http::authenticate(ts.svc, std::string(600, 'a')));
  CHECK(resolver.calls == 0);
}

// Needs WP-D's repo::find_session / touch_session.
TEST_CASE("dispatch: authenticate against the sessions table (repo)", "[dispatch][.integration]") {
  test::TestServices ts;
  ts.cfg.session_touch_interval_sec = 3600;
  int64_t uid = 0;
  std::string fresh, stale, expired;
  const int64_t now = ts.svc.now_ms();
  ts.db.write([&](db::Tx& tx) {
    uid = test::seed_user(tx, "alice@x.cn", true);
    fresh = test::seed_session(tx, uid, 30LL * 24 * 3600 * 1000, now);
    stale = test::seed_session(tx, uid, 30LL * 24 * 3600 * 1000, now - 2 * 3600 * 1000);
    expired = test::seed_session(tx, uid, 1000, now - 10'000);
  });
  auto p = http::authenticate(ts.svc, fresh);
  REQUIRE(p);
  CHECK(p->user_id == uid);
  CHECK(p->is_admin);
  CHECK(p->email == "alice@x.cn");
  CHECK_FALSE(http::authenticate(ts.svc, expired));
  CHECK_FALSE(http::authenticate(ts.svc, "unknown-token"));
  // A stale session is touched (sliding expiry).
  REQUIRE(http::authenticate(ts.svc, stale));
  const auto last_seen = ts.db.read([&](db::Conn& c) {
    return c.scalar<int64_t>("SELECT MAX(last_seen_at) FROM sessions WHERE user_id=? AND last_seen_at<?", uid,
                             now + 1);
  });
  CHECK(last_seen.value_or(0) >= now);
  // Disabled users are rejected.
  ts.db.write([&](db::Tx& tx) { tx.run("UPDATE users SET disabled=1 WHERE id=?", uid); });
  CHECK_FALSE(http::authenticate(ts.svc, fresh));
}
