// Owner: WP-D — api handlers called directly with an http::Ctx over TestServices (no server,
// no network). Tests that need another package's real implementation (WP-A LoginThrottle /
// Router, WP-B mail::*, WP-C svix / webhook processing / job retry) carry the hidden
// "[.integration]" tag so the default run stays hermetic and green.
#include "test_support.hpp"

#include "api/dto.hpp"
#include "api/handlers.hpp"
#include "api/routes.hpp"
#include "core/crypto.hpp"
#include "core/errors.hpp"
#include "core/strings.hpp"
#include "http/router.hpp"
#include "http/throttle.hpp"
#include "jobs/jobs.hpp"
#include "jobs/kinds.hpp"
#include "repo/accounts.hpp"
#include "resend/client.hpp"
#include "resend/svix.hpp"

#include <boost/json.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using namespace azm;
namespace json = boost::json;
using V = boost::beast::http::verb;

namespace {

constexpr int64_t kDayMs = 24LL * 3600 * 1000;
constexpr std::string_view kPassword = "correct horse battery";

// Cheap scrypt for seeded accounts (password_verify reads the parameters from the encoding).
std::string fast_hash(std::string_view pw) { return crypto::password_hash(pw, crypto::ScryptParams{.ln = 10}); }

class FakeResend final : public resend::Client {
 public:
  std::vector<resend::DomainInfo> domains;
  std::map<std::string, resend::DomainInfo> details;
  std::optional<resend::Error> list_error, get_error, cancel_error, update_error;
  std::vector<std::string> canceled;
  std::vector<std::pair<std::string, std::string>> rescheduled;

  std::vector<resend::DomainInfo> list_domains() override {
    if (list_error) throw *list_error;
    return domains;
  }
  resend::DomainInfo get_domain(std::string_view id) override {
    if (get_error) throw *get_error;
    auto it = details.find(std::string(id));
    if (it == details.end()) throw resend::Error(resend::Error::Kind::NotFound, 404, "not_found", "nope");
    return it->second;
  }
  void cancel(std::string_view id) override {
    if (cancel_error) throw *cancel_error;
    canceled.emplace_back(id);
  }
  void update_schedule(std::string_view id, std::string_view iso) override {
    if (update_error) throw *update_error;
    rescheduled.emplace_back(id, iso);
  }
};

struct Account {
  int64_t id = 0;
  std::string email;
  std::string token;
  http::Principal principal;
};

struct Api {
  test::TestServices ts;

  Api() {
    ts.db.write([&](db::Tx& tx) { repo::add_domain(tx, "team.example", ts.svc.now_ms()); });
  }

  Account account(std::string email, bool admin = false, std::string name = "") {
    Account a;
    a.email = email;
    ts.db.write([&](db::Tx& tx) {
      repo::NewUser nu;
      nu.email = email;
      nu.display_name = name;
      nu.password_hash = fast_hash(kPassword);
      nu.is_admin = admin;
      a.id = repo::create_user(tx, nu, ts.svc.now_ms()).id;
      auto s = repo::create_session(tx, a.id, 30 * kDayMs, "test", "127.0.0.1", ts.svc.now_ms());
      a.token = s.token;
      a.principal = http::Principal{a.id, s.session.id, admin, a.email};
    });
    return a;
  }

  // Calls `h` like http::dispatch would (ApiError → error Response).
  http::Response call(const http::Handler& h, V method, std::string_view target, std::string body = {},
                      std::optional<http::Principal> who = std::nullopt, http::Params params = {},
                      std::vector<std::pair<std::string, std::string>> headers = {}) {
    http::Request req = http::Request::make(method, target);
    req.body = std::move(body);
    req.body_size = req.body.size();
    req.remote_ip = "192.0.2.10";
    for (auto& [k, v] : headers) req.headers.set(k, v);
    http::Ctx ctx{req, ts.svc, std::move(params), std::move(who)};
    try {
      return h(ctx);
    } catch (const ApiError& e) {
      return http::Response::from_error(e);
    }
  }
  http::Response as(const Account& a, const http::Handler& h, V method, std::string_view target,
                    std::string body = {}, http::Params params = {}) {
    return call(h, method, target, std::move(body), a.principal, std::move(params));
  }
};

http::Params id_param(int64_t id, std::string name = "id") {
  http::Params p;
  p.emplace(std::move(name), std::to_string(id));
  return p;
}

json::value body_of(const http::Response& r) {
  const auto* s = std::get_if<std::string>(&r.body);
  REQUIRE(s != nullptr);
  return json::parse(*s);
}

std::string error_code(const http::Response& r) {
  const auto v = body_of(r);
  return std::string(v.at("error").at("code").as_string());
}

std::string error_field(const http::Response& r) {
  const auto v = body_of(r);
  const auto* f = v.at("error").at("details").as_object().if_contains("field");
  return f ? std::string(f->as_string()) : std::string();
}

#define CHECK_ERR(resp, st, cd)       \
  do {                                \
    const auto r_ = (resp);           \
    CHECK(r_.status == (st));         \
    CHECK(error_code(r_) == (cd));    \
  } while (0)

#define CHECK_FIELD(resp, fld)                \
  do {                                        \
    const auto r_ = (resp);                   \
    CHECK(r_.status == 400u);                 \
    CHECK(error_code(r_) == "invalid_field"); \
    CHECK(error_field(r_) == (fld));          \
  } while (0)

std::string login_body(std::string_view email, std::string_view pw) {
  json::object o;
  o["email"] = email;
  o["password"] = pw;
  return json::serialize(o);
}

}  // namespace

// =============================================================================================
// Auth
// =============================================================================================

TEST_CASE("login: success returns token, expiry and Me", "[api][auth]") {
  Api api;
  const auto alice = api.account("alice@team.example", false, "Alice");
  api.ts.db.write([&](db::Tx& tx) { test::seed_alias(tx, "support@team.example", {{alice.id, true}}, true, "客服"); });
  const int64_t now = api.ts.svc.now_ms();

  const auto r = api.call(api::auth_login, V::post, "/api/auth/login", login_body(" ALICE@team.example ", kPassword),
                          std::nullopt, {}, {{"User-Agent", "Mozilla/5.0 test"}});
  REQUIRE(r.status == 200u);
  const auto b = body_of(r).as_object();
  const std::string token(b.at("token").as_string());
  CHECK(token.size() == 43);
  CHECK(b.at("expires_at").as_int64() == now + 30 * kDayMs);
  const auto& me = b.at("user").as_object();
  CHECK(me.at("id").as_int64() == alice.id);
  CHECK(me.at("email") == "alice@team.example");
  CHECK(me.at("display_name") == "Alice");
  CHECK(me.at("is_admin") == false);
  CHECK(me.at("settings").at("undo_send_seconds") == 5);
  CHECK(me.at("settings").at("display_name") == "Alice");
  const auto& ids = me.at("identities").as_array();
  REQUIRE(ids.size() == 2);
  CHECK(ids[0].at("is_default") == true);
  CHECK(ids[1].at("email") == "support@team.example");
  CHECK(ids[1].at("kind") == "alias");
  CHECK(me.at("server").at("files_origins") == json::parse(R"(["http://127.0.0.1:8080"])"));
  CHECK(me.at("server").at("blob_backend") == "local");
  CHECK(me.at("server").at("version") == AZMAIL_VERSION);
  CHECK(json::serialize(b).find("scrypt") == std::string::npos);

  api.ts.db.read([&](db::Conn& c) {
    auto s = repo::find_session(c, token, now + 1);
    REQUIRE(s);
    CHECK(s->user.id == alice.id);
    CHECK(s->session.user_agent == "Mozilla/5.0 test");
    CHECK(s->session.ip == "192.0.2.10");
    CHECK(repo::get_user(c, alice.id)->last_login_at == now);
    CHECK(c.scalar<int64_t>("SELECT count(*) FROM audit_log WHERE action='login.success' AND actor_user_id=?",
                            alice.id) == 1);
  });
}

TEST_CASE("login: failures", "[api][auth]") {
  Api api;
  const auto alice = api.account("alice@team.example");
  auto login = [&](std::string body) { return api.call(api::auth_login, V::post, "/api/auth/login", std::move(body)); };

  CHECK_ERR(login(login_body("alice@team.example", "wrong password")), 401u, "invalid_credentials");
  CHECK_ERR(login(login_body("nobody@team.example", kPassword)), 401u, "invalid_credentials");
  CHECK_ERR(login(login_body("", "")), 401u, "invalid_credentials");
  CHECK_ERR(login("not json"), 400u, "invalid_json");
  CHECK_ERR(login(""), 400u, "invalid_json");
  CHECK_FIELD(login(R"({"email":"alice@team.example"})"), "password");
  api.ts.db.read([&](db::Conn& c) {
    CHECK(c.scalar<int64_t>("SELECT count(*) FROM audit_log WHERE action='login.failure'") == 3);
    // Unknown emails are audited without an actor.
    CHECK(c.scalar<int64_t>("SELECT count(*) FROM audit_log WHERE action='login.failure' AND actor_user_id IS NULL") == 2);
    CHECK(c.scalar<int64_t>("SELECT count(*) FROM sessions WHERE user_id=?", alice.id) == 1);  // only the seeded one
  });

  // Disabled: 403 only with the right password (a wrong one stays 401, revealing nothing).
  api.ts.db.write([&](db::Tx& tx) {
    repo::UserPatch p;
    p.disabled = true;
    repo::update_user(tx, alice.id, p, api.ts.svc.now_ms());
  });
  CHECK_ERR(login(login_body("alice@team.example", kPassword)), 403u, "account_disabled");
  CHECK_ERR(login(login_body("alice@team.example", "nope")), 401u, "invalid_credentials");
}

TEST_CASE("logout, me, unauthenticated calls", "[api][auth]") {
  Api api;
  const auto alice = api.account("alice@team.example", true, "Alice");

  const auto me = api.as(alice, api::auth_me, V::get, "/api/auth/me");
  REQUIRE(me.status == 200u);
  CHECK(body_of(me).at("is_admin") == true);
  CHECK(body_of(me).at("identities").as_array().size() == 1);

  CHECK_ERR(api.call(api::auth_me, V::get, "/api/auth/me"), 401u, "unauthorized");
  CHECK_ERR(api.call(api::settings_get, V::get, "/api/settings"), 401u, "unauthorized");
  CHECK_ERR(api.call(api::labels_list, V::get, "/api/labels"), 401u, "unauthorized");

  const auto out = api.as(alice, api::auth_logout, V::post, "/api/auth/logout");
  CHECK(out.status == 204u);
  CHECK(std::holds_alternative<std::monostate>(out.body));
  CHECK(api.ts.notifier.revoked_sessions() == std::vector<int64_t>{alice.principal.session_id});
  CHECK_FALSE(api.ts.db.read([&](db::Conn& c) { return repo::find_session(c, alice.token, api.ts.svc.now_ms()); }));

  // A principal whose user vanished gets 401 from /me.
  api.ts.db.write([&](db::Tx& tx) { tx.run("DELETE FROM users WHERE id=?", alice.id); });
  CHECK_ERR(api.as(alice, api::auth_me, V::get, "/api/auth/me"), 401u, "unauthorized");
}

TEST_CASE("password change", "[api][auth]") {
  Api api;
  const auto alice = api.account("alice@team.example");
  std::vector<int64_t> others;
  api.ts.db.write([&](db::Tx& tx) {
    for (int i = 0; i < 2; ++i)
      others.push_back(repo::create_session(tx, alice.id, kDayMs, "", "", api.ts.svc.now_ms()).session.id);
  });
  auto change = [&](std::string cur, std::string next) {
    json::object o;
    o["current_password"] = cur;
    o["new_password"] = next;
    return api.as(alice, api::auth_change_password, V::post, "/api/auth/password", json::serialize(o));
  };

  CHECK_ERR(change(std::string(kPassword), "short"), 422u, "weak_password");
  CHECK_ERR(change("not my password", "a brand new password"), 403u, "invalid_credentials");
  CHECK(api.ts.notifier.revoked_sessions().empty());

  const auto ok = change(std::string(kPassword), "a brand new password");
  CHECK(ok.status == 204u);
  CHECK(api.ts.notifier.revoked_sessions() == others);
  api.ts.db.read([&](db::Conn& c) {
    const auto u = repo::get_user(c, alice.id);
    CHECK(crypto::password_verify("a brand new password", u->password_hash));
    CHECK_FALSE(crypto::password_verify(kPassword, u->password_hash));
    CHECK(c.scalar<int64_t>("SELECT count(*) FROM sessions WHERE user_id=?", alice.id) == 1);
    CHECK(repo::find_session(c, alice.token, api.ts.svc.now_ms()));  // current session kept
    CHECK(c.scalar<int64_t>("SELECT count(*) FROM audit_log WHERE action='password.change'") == 1);
  });
  // Login with the new password works, the old one does not.
  CHECK(api.call(api::auth_login, V::post, "/", login_body(alice.email, "a brand new password")).status == 200u);
  CHECK_ERR(api.call(api::auth_login, V::post, "/", login_body(alice.email, kPassword)), 401u, "invalid_credentials");
}

TEST_CASE("login throttling → 429 too_many_attempts", "[api][auth][.integration]") {
  // Needs WP-A's LoginThrottle implementation.
  Api api;
  api.account("alice@team.example");
  http::LoginThrottle throttle(http::LoginThrottle::Limits{2, 100, std::chrono::seconds(900), 4}, api.ts.clock);
  api.ts.svc.login_throttle = &throttle;
  auto login = [&](std::string_view pw) {
    return api.call(api::auth_login, V::post, "/api/auth/login", login_body("alice@team.example", pw));
  };
  CHECK_ERR(login("bad1"), 401u, "invalid_credentials");
  CHECK_ERR(login("bad2"), 401u, "invalid_credentials");
  const auto r = login(kPassword);  // even the right password is refused while throttled
  CHECK_ERR(r, 429u, "too_many_attempts");
  CHECK(body_of(r).at("error").at("details").at("retry_after").as_int64() >= 1);
  api.ts.clock.advance(901 * 1000);
  CHECK(login(kPassword).status == 200u);
}

// =============================================================================================
// Settings / identities
// =============================================================================================

TEST_CASE("settings get / put", "[api][settings]") {
  Api api;
  const auto alice = api.account("alice@team.example", false, "Alice");
  const auto g = api.as(alice, api::settings_get, V::get, "/api/settings");
  REQUIRE(g.status == 200u);
  CHECK(body_of(g).at("page_size") == 50);
  CHECK(body_of(g).at("display_name") == "Alice");

  api.ts.notifier.clear();
  const auto p = api.as(alice, api::settings_update, V::put, "/api/settings",
                        R"({"page_size":20,"remote_images":"always","trusted_image_senders":["News@x.example"]})");
  REQUIRE(p.status == 200u);
  const auto s = body_of(p).as_object();
  CHECK(s.at("page_size") == 20);
  CHECK(s.at("remote_images") == "always");
  CHECK(s.at("trusted_image_senders") == json::parse(R"(["news@x.example"])"));
  CHECK(s.at("undo_send_seconds") == 5);  // full Settings back
  CHECK(s.size() == 8);
  const auto ev = api.ts.notifier.events_of("settings.changed");
  REQUIRE(ev.size() == 1);
  CHECK(ev[0].user_id == alice.id);

  CHECK_FIELD(api.as(alice, api::settings_update, V::put, "/api/settings", R"({"page_size":5})"), "page_size");
  CHECK_FIELD(api.as(alice, api::settings_update, V::put, "/api/settings", R"({"undo_send_seconds":3})"),
              "undo_send_seconds");
  CHECK_FIELD(api.as(alice, api::settings_update, V::put, "/api/settings", R"({"remote_images":1})"), "remote_images");
  CHECK_ERR(api.as(alice, api::settings_update, V::put, "/api/settings", "[]"), 400u, "invalid_json");
}

TEST_CASE("identities list is a bare array", "[api][settings]") {
  Api api;
  const auto alice = api.account("alice@team.example", false, "Alice");
  const auto bob = api.account("bob@team.example");
  api.ts.db.write([&](db::Tx& tx) { test::seed_alias(tx, "help@team.example", {{alice.id, true}, {bob.id, false}}); });
  const auto r = api.as(alice, api::identities_list, V::get, "/api/identities");
  REQUIRE(r.status == 200u);
  const auto arr = body_of(r).as_array();
  REQUIRE(arr.size() == 2);
  CHECK(arr[0].at("email") == "alice@team.example");
  CHECK(arr[1].at("email") == "help@team.example");
  CHECK(body_of(api.as(bob, api::identities_list, V::get, "/api/identities")).as_array().size() == 1);
}

// =============================================================================================
// Labels
// =============================================================================================

TEST_CASE("labels CRUD and per-owner isolation", "[api][labels]") {
  Api api;
  const auto alice = api.account("alice@team.example");
  const auto bob = api.account("bob@team.example");
  api.ts.notifier.clear();

  const auto c = api.as(alice, api::labels_create, V::post, "/api/labels", R"({"name":"Work","color":"#FF0000"})");
  REQUIRE(c.status == 201u);
  const int64_t id = body_of(c).at("id").as_int64();
  CHECK(body_of(c) == json::parse(R"({"id":)" + std::to_string(id) + R"(,"name":"Work","color":"#ff0000","sort_order":0})"));
  CHECK(api.ts.notifier.events_of("labels.changed").size() == 1);

  CHECK_ERR(api.as(alice, api::labels_create, V::post, "/api/labels", R"({"name":"work","color":"#000000"})"), 409u,
            "label_exists");
  CHECK_FIELD(api.as(alice, api::labels_create, V::post, "/api/labels", R"({"name":"X","color":"red"})"), "color");
  CHECK_FIELD(api.as(alice, api::labels_create, V::post, "/api/labels", R"({"color":"#000000"})"), "name");

  const auto list = api.as(alice, api::labels_list, V::get, "/api/labels");
  REQUIRE(list.status == 200u);
  CHECK(body_of(list).as_array().size() == 1);
  CHECK(body_of(api.as(bob, api::labels_list, V::get, "/api/labels")).as_array().empty());

  CHECK(api.as(alice, api::labels_get, V::get, "/", {}, id_param(id)).status == 200u);
  CHECK_ERR(api.as(bob, api::labels_get, V::get, "/", {}, id_param(id)), 404u, "not_found");
  CHECK_ERR(api.as(bob, api::labels_update, V::patch, "/", R"({"name":"Mine"})", id_param(id)), 404u, "not_found");
  CHECK_ERR(api.as(bob, api::labels_delete, V::delete_, "/", {}, id_param(id)), 404u, "not_found");
  CHECK_FIELD(api.as(alice, api::labels_get, V::get, "/", {}, id_param(0)), "id");
  CHECK_FIELD(api.as(alice, api::labels_get, V::get, "/"), "id");

  const auto u = api.as(alice, api::labels_update, V::patch, "/", R"({"name":"Job","sort_order":3})", id_param(id));
  REQUIRE(u.status == 200u);
  CHECK(body_of(u).at("name") == "Job");
  CHECK(body_of(u).at("sort_order") == 3);
  CHECK(body_of(u).at("color") == "#ff0000");

  const auto d = api.as(alice, api::labels_delete, V::delete_, "/", {}, id_param(id));
  CHECK(d.status == 204u);
  CHECK_ERR(api.as(alice, api::labels_get, V::get, "/", {}, id_param(id)), 404u, "not_found");
}

// =============================================================================================
// Admin
// =============================================================================================

TEST_CASE("admin routes refuse non-admins", "[api][admin]") {
  Api api;
  const auto bob = api.account("bob@team.example");
  const std::vector<std::pair<const char*, http::Handler>> handlers = {
      {"users_list", api::admin_users_list},       {"users_create", api::admin_users_create},
      {"users_update", api::admin_users_update},   {"users_delete", api::admin_users_delete},
      {"aliases_list", api::admin_aliases_list},   {"aliases_create", api::admin_aliases_create},
      {"aliases_update", api::admin_aliases_update}, {"aliases_delete", api::admin_aliases_delete},
      {"domains_list", api::admin_domains_list},   {"domains_create", api::admin_domains_create},
      {"domains_get", api::admin_domains_get},     {"domains_delete", api::admin_domains_delete},
      {"domains_status", api::admin_domains_status}, {"events", api::admin_events_list},
      {"inbound", api::admin_inbound_list},        {"outbox", api::admin_outbox_list},
      {"outbox_retry", api::admin_outbox_retry},   {"jobs", api::admin_jobs_list},
      {"jobs_retry", api::admin_jobs_retry},       {"sync", api::admin_sync},
      {"stats", api::admin_stats_get},
  };
  for (const auto& [name, h] : handlers) {
    INFO(name);
    CHECK_ERR(api.as(bob, h, V::post, "/", "{}", id_param(bob.id)), 403u, "forbidden");
    CHECK_ERR(api.call(h, V::post, "/", "{}", std::nullopt, id_param(bob.id)), 401u, "unauthorized");
  }
  // Nothing happened.
  api.ts.db.read([](db::Conn& c) {
    CHECK(c.scalar<int64_t>("SELECT count(*) FROM jobs") == 0);
    CHECK(c.scalar<int64_t>("SELECT count(*) FROM audit_log") == 0);
  });
}

TEST_CASE("admin users: create, list, errors", "[api][admin]") {
  Api api;
  const auto admin = api.account("admin@team.example", true, "Admin");
  api.ts.cfg.default_undo_send_seconds = 10;
  auto create = [&](std::string body) { return api.as(admin, api::admin_users_create, V::post, "/api/admin/users", body); };

  const auto r = create(R"({"email":"Carol@Team.example","display_name":"Carol","password":"carol-password-1"})");
  REQUIRE(r.status == 201u);
  const auto u = body_of(r).as_object();
  const int64_t carol = u.at("id").as_int64();
  CHECK(u.at("email") == "carol@team.example");
  CHECK(u.at("display_name") == "Carol");
  CHECK(u.at("is_admin") == false);
  CHECK(u.at("disabled") == false);
  CHECK(u.at("last_login_at").is_null());
  CHECK(u.at("message_count") == 0);
  CHECK(u.at("storage_bytes") == 0);
  CHECK(u.at("aliases").as_array().empty());
  CHECK(u.size() == 10);
  api.ts.db.read([&](db::Conn& c) {
    CHECK(repo::get_settings(c, carol).undo_send_seconds == 10);  // cfg.default_undo_send_seconds
    CHECK(c.scalar<int64_t>("SELECT count(*) FROM audit_log WHERE action='user.create' AND actor_user_id=?",
                            admin.id) == 1);
  });
  CHECK(api.call(api::auth_login, V::post, "/", login_body("carol@team.example", "carol-password-1")).status == 200u);

  CHECK_ERR(create(R"({"email":"carol@team.example","password":"carol-password-1"})"), 409u, "address_exists");
  CHECK_ERR(create(R"({"email":"dan@nowhere.example","password":"dan-password-1"})"), 422u, "unknown_domain");
  CHECK_ERR(create(R"({"email":"dan@team.example","password":"short"})"), 422u, "weak_password");
  CHECK_FIELD(create(R"({"email":"not an email","password":"dan-password-1"})"), "email");
  CHECK_FIELD(create(R"({"email":"dan+x@team.example","password":"dan-password-1"})"), "email");
  CHECK_FIELD(create(R"({"password":"dan-password-1"})"), "email");

  const auto list = api.as(admin, api::admin_users_list, V::get, "/api/admin/users");
  REQUIRE(list.status == 200u);
  const auto arr = body_of(list).as_array();
  REQUIRE(arr.size() == 2);
  CHECK(arr[0].at("email") == "admin@team.example");
  CHECK(arr[1].at("email") == "carol@team.example");
}

TEST_CASE("admin users: patch and delete", "[api][admin]") {
  Api api;
  const auto admin = api.account("admin@team.example", true, "Admin");
  const auto bob = api.account("bob@team.example", false, "Bob");
  int64_t bob_second = 0;
  api.ts.db.write([&](db::Tx& tx) {
    bob_second = repo::create_session(tx, bob.id, kDayMs, "", "", api.ts.svc.now_ms()).session.id;
  });
  auto patch = [&](int64_t id, std::string body) {
    return api.as(admin, api::admin_users_update, V::patch, "/", std::move(body), id_param(id));
  };

  SECTION("display name and admin flag") {
    const auto r = patch(bob.id, R"({"display_name":"Robert","is_admin":true})");
    REQUIRE(r.status == 200u);
    CHECK(body_of(r).at("display_name") == "Robert");
    CHECK(body_of(r).at("is_admin") == true);
    CHECK(api.ts.notifier.revoked_users().empty());
    CHECK(api.ts.notifier.revoked_sessions().empty());
    CHECK_FIELD(patch(bob.id, R"({"display_name":3})"), "display_name");
    CHECK_ERR(patch(9999, R"({"display_name":"x"})"), 404u, "not_found");
  }
  SECTION("the last admin cannot demote or disable themselves") {
    CHECK_ERR(patch(admin.id, R"({"is_admin":false})"), 409u, "last_admin");
    CHECK_ERR(patch(admin.id, R"({"disabled":true})"), 409u, "last_admin");
    CHECK(api.ts.notifier.revoked_users().empty());
    // Sessions untouched by the rolled-back transaction.
    CHECK(api.ts.db.read([&](db::Conn& c) { return repo::find_session(c, admin.token, api.ts.svc.now_ms()); }));
  }
  SECTION("disable revokes every session and the user's sockets") {
    const auto r = patch(bob.id, R"({"disabled":true})");
    REQUIRE(r.status == 200u);
    CHECK(body_of(r).at("disabled") == true);
    CHECK(api.ts.notifier.revoked_users() == std::vector<int64_t>{bob.id});
    api.ts.db.read([&](db::Conn& c) {
      CHECK(c.scalar<int64_t>("SELECT count(*) FROM sessions WHERE user_id=?", bob.id) == 0);
    });
    CHECK_ERR(api.call(api::auth_login, V::post, "/", login_body(bob.email, kPassword)), 403u, "account_disabled");
  }
  SECTION("password reset revokes the target's sessions") {
    CHECK_ERR(patch(bob.id, R"({"password":"short"})"), 422u, "weak_password");
    const auto r = patch(bob.id, R"({"password":"reset-password-1"})");
    REQUIRE(r.status == 200u);
    auto revoked = api.ts.notifier.revoked_sessions();
    std::sort(revoked.begin(), revoked.end());
    CHECK(revoked == std::vector<int64_t>{bob.principal.session_id, bob_second});
    CHECK(api.call(api::auth_login, V::post, "/", login_body(bob.email, "reset-password-1")).status == 200u);
    // Resetting your own password keeps the session you are using.
    api.ts.notifier.clear();
    CHECK(patch(admin.id, R"({"password":"admin-password-2"})").status == 200u);
    CHECK(api.ts.notifier.revoked_sessions().empty());
    CHECK(api.ts.db.read([&](db::Conn& c) { return repo::find_session(c, admin.token, api.ts.svc.now_ms()); }));
  }
  SECTION("delete") {
    CHECK_ERR(api.as(admin, api::admin_users_delete, V::delete_, "/", {}, id_param(admin.id)), 409u,
              "cannot_delete_self");
    CHECK_ERR(api.as(admin, api::admin_users_delete, V::delete_, "/", {}, id_param(4242)), 404u, "not_found");
    const auto r = api.as(admin, api::admin_users_delete, V::delete_, "/", {}, id_param(bob.id));
    CHECK(r.status == 204u);
    CHECK(api.ts.notifier.revoked_users() == std::vector<int64_t>{bob.id});
    CHECK_FALSE(api.ts.db.read([&](db::Conn& c) { return repo::get_user(c, bob.id); }));
    api.ts.db.read([&](db::Conn& c) {
      CHECK(c.scalar<int64_t>("SELECT count(*) FROM audit_log WHERE action='user.delete' AND target=?",
                              bob.email) == 1);
    });
  }
}

TEST_CASE("admin aliases CRUD", "[api][admin]") {
  Api api;
  const auto admin = api.account("admin@team.example", true);
  const auto bob = api.account("bob@team.example", false, "Bob");

  const auto c = api.as(admin, api::admin_aliases_create, V::post, "/api/admin/aliases",
                        R"({"email":"support@team.example","display_name":"客服","share_sent":true,
                            "members":[{"user_id":)" + std::to_string(bob.id) + R"(,"can_send_as":true}]})");
  REQUIRE(c.status == 201u);
  const auto a = body_of(c).as_object();
  const int64_t id = a.at("id").as_int64();
  CHECK(a.at("email") == "support@team.example");
  CHECK(a.at("members").as_array().size() == 1);
  CHECK(a.at("members").at(0).at("display_name") == "Bob");
  CHECK(a.at("members").at(0).at("can_send_as") == true);

  CHECK_ERR(api.as(admin, api::admin_aliases_create, V::post, "/", R"({"email":"bob@team.example"})"), 409u,
            "address_exists");
  CHECK_FIELD(api.as(admin, api::admin_aliases_create, V::post, "/", R"({"email":"x@team.example","members":[{"user_id":999}]})"),
              "members");

  const auto list = api.as(admin, api::admin_aliases_list, V::get, "/api/admin/aliases");
  CHECK(body_of(list).as_array().size() == 1);
  // Bob may now send as support@.
  CHECK(body_of(api.as(bob, api::identities_list, V::get, "/")).as_array().size() == 2);

  const auto p = api.as(admin, api::admin_aliases_update, V::patch, "/", R"({"members":[],"share_sent":false})", id_param(id));
  REQUIRE(p.status == 200u);
  CHECK(body_of(p).at("members").as_array().empty());
  CHECK(body_of(p).at("share_sent") == false);
  CHECK(body_of(api.as(bob, api::identities_list, V::get, "/")).as_array().size() == 1);
  CHECK_ERR(api.as(admin, api::admin_aliases_update, V::patch, "/", "{}", id_param(777)), 404u, "not_found");

  // Referenced by an outbound row → alias_in_use; otherwise 204.
  api.ts.db.write([&](db::Tx& tx) {
    tx.run("INSERT INTO outbound(uuid, sender_user_id, from_address_id, status, send_after, payload_json, created_at, "
           "updated_at) VALUES('u1', ?, ?, 'sent', 1, '{}', 1, 1)",
           bob.id, id);
  });
  CHECK_ERR(api.as(admin, api::admin_aliases_delete, V::delete_, "/", {}, id_param(id)), 409u, "alias_in_use");
  const int64_t other = body_of(api.as(admin, api::admin_aliases_create, V::post, "/", R"({"email":"tmp@team.example"})"))
                            .at("id")
                            .as_int64();
  CHECK(api.as(admin, api::admin_aliases_delete, V::delete_, "/", {}, id_param(other)).status == 204u);
  CHECK_ERR(api.as(admin, api::admin_aliases_delete, V::delete_, "/", {}, id_param(other)), 404u, "not_found");
}

TEST_CASE("admin domains CRUD", "[api][admin]") {
  Api api;
  const auto admin = api.account("admin@team.example", true);
  const auto c = api.as(admin, api::admin_domains_create, V::post, "/api/admin/domains", R"({"name":"New.Example"})");
  REQUIRE(c.status == 201u);
  const int64_t id = body_of(c).at("id").as_int64();
  CHECK(body_of(c).at("name") == "new.example");
  CHECK(body_of(c).at("receiving_enabled") == true);
  CHECK_ERR(api.as(admin, api::admin_domains_create, V::post, "/", R"({"name":"new.example"})"), 409u, "domain_exists");
  CHECK_FIELD(api.as(admin, api::admin_domains_create, V::post, "/", R"({"name":"nodot"})"), "name");
  CHECK_FIELD(api.as(admin, api::admin_domains_create, V::post, "/", "{}"), "name");

  const auto list = api.as(admin, api::admin_domains_list, V::get, "/");
  CHECK(body_of(list).as_array().size() == 2);
  CHECK(body_of(api.as(admin, api::admin_domains_get, V::get, "/", {}, id_param(id))).at("name") == "new.example");
  CHECK_ERR(api.as(admin, api::admin_domains_get, V::get, "/", {}, id_param(999)), 404u, "not_found");

  const int64_t team = api.ts.db.read([](db::Conn& c) { return repo::find_domain(c, "team.example")->id; });
  CHECK_ERR(api.as(admin, api::admin_domains_delete, V::delete_, "/", {}, id_param(team)), 409u, "domain_in_use");
  CHECK(api.as(admin, api::admin_domains_delete, V::delete_, "/", {}, id_param(id)).status == 204u);
  CHECK_ERR(api.as(admin, api::admin_domains_delete, V::delete_, "/", {}, id_param(id)), 404u, "not_found");
}

TEST_CASE("admin domain status proxies Resend", "[api][admin]") {
  Api api;
  const auto admin = api.account("admin@team.example", true);
  const int64_t team = api.ts.db.read([](db::Conn& c) { return repo::find_domain(c, "team.example")->id; });
  auto status = [&] { return api.as(admin, api::admin_domains_status, V::get, "/", {}, id_param(team)); };

  CHECK_ERR(status(), 503u, "service_unavailable");  // no Resend client configured

  FakeResend fake;
  api.ts.svc.resend = &fake;
  CHECK(body_of(status()) == json::parse(R"({"id":)" + std::to_string(team) + R"(,"name":"team.example","resend":null})"));

  resend::DomainInfo listed;
  listed.id = "dom_1";
  listed.name = "TEAM.example";
  listed.status = "pending";
  fake.domains = {resend::DomainInfo{"dom_0", "other.example", "verified", {}, {}, {}}, listed};
  CHECK(body_of(status()).at("resend").is_null());  // listed but gone on GET /domains/{id}
  resend::DomainInfo full = listed;
  full.status = "verified";
  full.region = "us-east-1";
  full.created_at_ms = 5;
  full.records = {{"MX", "team.example", "MX", "Auto", "verified", "inbound.example", 10}};
  fake.details["dom_1"] = full;
  const auto ok = status();
  REQUIRE(ok.status == 200u);
  const auto rs = body_of(ok).at("resend").as_object();
  CHECK(rs.at("id") == "dom_1");
  CHECK(rs.at("status") == "verified");
  CHECK(rs.at("region") == "us-east-1");
  CHECK(rs.at("records").at(0).at("priority") == 10);

  fake.list_error = resend::Error(resend::Error::Kind::Server, 500, "internal", "boom");
  CHECK_ERR(status(), 502u, "resend_error");
  fake.list_error.reset();
  fake.get_error = resend::Error(resend::Error::Kind::Auth, 401, "invalid_api_key", "bad key");
  CHECK_ERR(status(), 502u, "resend_error");
  CHECK_ERR(api.as(admin, api::admin_domains_status, V::get, "/", {}, id_param(999)), 404u, "not_found");
}

TEST_CASE("admin read-only lists: events, inbound, outbox, jobs", "[api][admin]") {
  Api api;
  const auto admin = api.account("admin@team.example", true);
  api.ts.db.write([&](db::Tx& tx) {
    for (int i = 1; i <= 60; ++i)
      tx.run("INSERT INTO webhook_events(svix_id, type, payload, received_at) VALUES(?,?,'{\"html\":\"secret body\"}',?)",
             "msg_" + std::to_string(i), i % 2 ? "email.delivered" : "email.received", i);
    tx.run("INSERT INTO inbound_emails(resend_id, state, source, recipients_json, created_at, updated_at) "
           "VALUES('re_1','unroutable','poll','[\"ghost@team.example\"]',1,1)");
    tx.run("INSERT INTO inbound_emails(resend_id, state, source, created_at, updated_at) VALUES('re_2','delivered','webhook',2,2)");
    tx.run("INSERT INTO outbound(uuid, sender_user_id, from_address_id, status, send_after, payload_json, created_at, "
           "updated_at) VALUES('u1', ?, ?, 'failed', 1, '{}', 1, 1)",
           admin.id, test::address_id(tx.conn(), admin.email));
    const int64_t j = jobs::enqueue(tx, jobs::kinds::kGcBlobs, {});
    tx.run("UPDATE jobs SET state='dead' WHERE id=?", j);
    jobs::enqueue(tx, jobs::kinds::kPurgeTrash, {});
  });

  const auto e1 = api.as(admin, api::admin_events_list, V::get, "/api/admin/events");
  REQUIRE(e1.status == 200u);
  const auto p1 = body_of(e1).as_object();
  CHECK(p1.at("items").as_array().size() == 50);
  CHECK(p1.at("items").at(0).at("svix_id") == "msg_60");
  CHECK_FALSE(p1.at("items").at(0).as_object().contains("payload"));  // D7: no bodies
  CHECK(json::serialize(p1).find("secret body") == std::string::npos);
  const std::string cursor(p1.at("next_cursor").as_string());
  const auto e2 = api.as(admin, api::admin_events_list, V::get, "/api/admin/events?cursor=" + cursor);
  CHECK(body_of(e2).at("items").as_array().size() == 10);
  CHECK(body_of(e2).at("next_cursor").is_null());
  const auto f = api.as(admin, api::admin_events_list, V::get, "/api/admin/events?type=email.received");
  CHECK(body_of(f).at("items").as_array().size() == 30);
  CHECK_FIELD(api.as(admin, api::admin_events_list, V::get, "/api/admin/events?cursor=zzz"), "cursor");

  const auto in = api.as(admin, api::admin_inbound_list, V::get, "/api/admin/inbound?state=unroutable");
  REQUIRE(in.status == 200u);
  CHECK(body_of(in).as_array().size() == 1);
  CHECK(body_of(in).at(0).at("recipients") == json::parse(R"(["ghost@team.example"])"));
  CHECK(body_of(api.as(admin, api::admin_inbound_list, V::get, "/api/admin/inbound")).as_array().size() == 2);
  CHECK_FIELD(api.as(admin, api::admin_inbound_list, V::get, "/api/admin/inbound?state=nope"), "state");

  const auto out = api.as(admin, api::admin_outbox_list, V::get, "/api/admin/outbox?status=failed");
  REQUIRE(out.status == 200u);
  CHECK(body_of(out).as_array().size() == 1);
  CHECK(body_of(out).at(0).at("sender_email") == "admin@team.example");
  CHECK(body_of(api.as(admin, api::admin_outbox_list, V::get, "/api/admin/outbox?status=queued")).as_array().empty());
  CHECK_FIELD(api.as(admin, api::admin_outbox_list, V::get, "/api/admin/outbox?status=lost"), "status");

  const auto jr = api.as(admin, api::admin_jobs_list, V::get, "/api/admin/jobs?state=dead");
  REQUIRE(jr.status == 200u);
  CHECK(body_of(jr).as_array().size() == 1);
  CHECK(body_of(jr).at(0).at("kind") == "gc.blobs");
  CHECK(body_of(jr).at(0).at("payload").is_object());
  CHECK(body_of(api.as(admin, api::admin_jobs_list, V::get, "/api/admin/jobs")).as_array().size() == 2);
  CHECK_FIELD(api.as(admin, api::admin_jobs_list, V::get, "/api/admin/jobs?state=zombie"), "state");
  CHECK_ERR(api.as(admin, api::admin_jobs_retry, V::post, "/", {}, id_param(999)), 404u, "not_found");
}

TEST_CASE("admin sync enqueues one manual poll", "[api][admin]") {
  Api api;
  const auto admin = api.account("admin@team.example", true);
  const auto r = api.as(admin, api::admin_sync, V::post, "/api/admin/sync");
  REQUIRE(r.status == 202u);
  const int64_t job = body_of(r).at("job_id").as_int64();
  CHECK(body_of(api.as(admin, api::admin_sync, V::post, "/api/admin/sync")).at("job_id").as_int64() == job);  // dedupe
  api.ts.db.read([&](db::Conn& c) {
    const auto j = repo::get_job(c, job);
    REQUIRE(j);
    CHECK(j->kind == "poll.receiving");
    CHECK(j->lane == "sync");
    CHECK(j->dedupe_key == "poll:manual");
    CHECK(json::value(j->payload) == json::parse(R"({"manual":true})"));
    CHECK(j->run_at == api.ts.svc.now_ms());
  });
}

TEST_CASE("admin stats", "[api][admin]") {
  Api api;
  const auto admin = api.account("admin@team.example", true);
  api.ts.cfg.files_delivery = FilesDelivery::Redirect;
  const auto r = api.as(admin, api::admin_stats_get, V::get, "/api/admin/stats");
  REQUIRE(r.status == 200u);
  const auto s = body_of(r).as_object();
  CHECK(s.at("users") == 1);
  CHECK(s.at("queue") == json::parse(R"({"pending":0,"dead":0})"));
  CHECK(s.at("storage") == json::parse(R"({"backend":"local","delivery":"redirect","blob_count":0,"blob_bytes":0})"));
  CHECK(s.at("quota_blocked") == false);
  CHECK(s.at("last_webhook_at").is_null());
}

// =============================================================================================
// Health, webhooks, files, uploads (hermetic parts)
// =============================================================================================

TEST_CASE("health", "[api][health]") {
  Api api;
  const auto r = api.call(api::health_get, V::get, "/api/health");
  REQUIRE(r.status == 200u);
  const auto b = body_of(r).as_object();
  CHECK(b.at("status") == "ok");
  CHECK(b.at("db") == "ok");
  CHECK(b.at("version") == AZMAIL_VERSION);
  CHECK(b.at("time").as_int64() == api.ts.svc.now_ms());

  // A database that cannot be opened → 503 with db:"error" (never an exception).
  db::Pool broken(api.ts.dir / "missing-dir" / "x.db", 1);
  Services svc{api.ts.cfg, broken, *api.ts.blobs, api.ts.urls, api.ts.notifier, api.ts.clock};
  http::Request req = http::Request::make(V::get, "/api/health");
  http::Ctx ctx{req, svc, {}, std::nullopt};
  const auto bad = api::health_get(ctx);
  CHECK(bad.status == 503u);
  CHECK(body_of(bad).at("db") == "error");
  CHECK(body_of(bad).at("status") == "error");
}

TEST_CASE("webhook: rejected without secret or Svix headers", "[api][webhooks]") {
  Api api;
  const std::string body = R"({"type":"email.delivered","created_at":"2026-10-07T00:00:00Z","data":{"email_id":"e1"}})";
  const std::vector<std::pair<std::string, std::string>> hdrs = {
      {"svix-id", "msg_1"}, {"svix-timestamp", "1"}, {"svix-signature", "v1,AAAA"}};
  // No secret configured: nothing can be verified.
  CHECK_ERR(api.call(api::webhooks_resend, V::post, "/api/webhooks/resend", body, std::nullopt, {}, hdrs), 401u,
            "invalid_signature");
  api.ts.cfg.resend_webhook_secret = "whsec_MfKQ9r8GKYqrTwjUPD8ILPZIo2LaLaSw";
  for (std::size_t skip = 0; skip < hdrs.size(); ++skip) {
    std::vector<std::pair<std::string, std::string>> h;
    for (std::size_t i = 0; i < hdrs.size(); ++i)
      if (i != skip) h.push_back(hdrs[i]);
    CHECK_ERR(api.call(api::webhooks_resend, V::post, "/api/webhooks/resend", body, std::nullopt, {}, h), 401u,
              "invalid_signature");
  }
  api.ts.db.read([](db::Conn& c) { CHECK(c.scalar<int64_t>("SELECT count(*) FROM webhook_events") == 0); });
}

TEST_CASE("files: signed URL checks", "[api][files]") {
  Api api;
  const auto alice = api.account("alice@team.example");
  const int64_t now = api.ts.svc.now_ms();
  const int64_t exp = now + 3600 * 1000;
  auto get = [&](std::string target) {
    return api.call(api::files_get, V::get, target, {}, std::nullopt, id_param(5));
  };
  const std::string sig = api.ts.urls.sign_file(5, alice.id, 'a', exp);
  const std::string base = "/api/files/5?u=" + std::to_string(alice.id) + "&exp=" + std::to_string(exp);

  CHECK_ERR(get("/api/files/5"), 403u, "invalid_signature");
  CHECK_ERR(get(base + "&sig=" + sig), 403u, "invalid_signature");                      // no d
  CHECK_ERR(get(base + "&d=x&sig=" + sig), 403u, "invalid_signature");                  // bad d
  CHECK_ERR(get(base + "&d=i&sig=" + sig), 403u, "invalid_signature");                  // d is signed
  CHECK_ERR(get(base + "&d=a&sig=AAAA"), 403u, "invalid_signature");                    // forged
  CHECK_ERR(get("/api/files/5?d=a&u=abc&exp=1&sig=" + sig), 403u, "invalid_signature");  // malformed u
  const std::string expired_sig = api.ts.urls.sign_file(5, alice.id, 'a', now - 1);
  CHECK_ERR(get("/api/files/5?d=a&u=" + std::to_string(alice.id) + "&exp=" + std::to_string(now - 1) +
                "&sig=" + expired_sig),
            403u, "invalid_signature");
  // Another user's id with alice's signature.
  CHECK_ERR(get("/api/files/5?d=a&u=" + std::to_string(alice.id + 1) + "&exp=" + std::to_string(exp) + "&sig=" + sig),
            403u, "invalid_signature");
  // Valid signature, disabled user.
  api.ts.db.write([&](db::Tx& tx) { tx.run("UPDATE users SET disabled=1 WHERE id=?", alice.id); });
  CHECK_ERR(get(base + "&d=a&sig=" + sig), 403u, "invalid_signature");

  // Raw .eml URLs.
  const std::string raw_sig = api.ts.urls.sign_raw(9, alice.id, exp);
  auto raw = [&](std::string target) {
    return api.call(api::files_raw, V::get, target, {}, std::nullopt, id_param(9, "messageId"));
  };
  CHECK_ERR(raw("/api/files/raw/9"), 403u, "invalid_signature");
  CHECK_ERR(raw("/api/files/raw/9?u=" + std::to_string(alice.id) + "&exp=" + std::to_string(exp) + "&sig=AAAA"), 403u,
            "invalid_signature");
  CHECK_ERR(raw("/api/files/raw/9?u=" + std::to_string(alice.id) + "&exp=" + std::to_string(exp) + "&sig=" + raw_sig),
            403u, "invalid_signature");  // still disabled
}

TEST_CASE("attachments upload: parameter validation", "[api][files]") {
  Api api;
  const auto alice = api.account("alice@team.example");
  CHECK_FIELD(api.as(alice, api::attachments_upload, V::post, "/api/attachments"), "filename");
  CHECK_FIELD(api.as(alice, api::attachments_upload, V::post, "/api/attachments?filename=%20%20"), "filename");
  CHECK_FIELD(api.as(alice, api::attachments_upload, V::post, "/api/attachments?filename=a.txt&inline=maybe"), "inline");
  CHECK_ERR(api.as(alice, api::attachments_upload, V::post, "/api/attachments?filename=a.txt"), 400u, "bad_request");
  CHECK_ERR(api.call(api::attachments_upload, V::post, "/api/attachments?filename=a.txt"), 401u, "unauthorized");
}

TEST_CASE("mail routes validate query parameters before touching the mailbox", "[api][mail]") {
  Api api;
  const auto alice = api.account("alice@team.example");
  auto list = [&](std::string q) { return api.as(alice, api::threads_list, V::get, "/api/threads" + q); };
  CHECK_FIELD(list("?folder=bogus"), "folder");
  CHECK_FIELD(list("?folder=INBOX"), "folder");  // case-sensitive wire names
  CHECK_FIELD(list("?label_id=0"), "label_id");
  CHECK_FIELD(list("?label_id=abc"), "label_id");
  CHECK_FIELD(list("?limit=0"), "limit");
  CHECK_FIELD(list("?limit=ten"), "limit");
  CHECK_FIELD(list("?tzoff=900"), "tzoff");
  CHECK_FIELD(list("?q=" + std::string(1100, 'a')), "q");
  CHECK_FIELD(api.as(alice, api::threads_get, V::get, "/"), "id");
  CHECK_FIELD(api.as(alice, api::messages_get, V::get, "/", {}, id_param(-3)), "id");
  CHECK_ERR(api.as(alice, api::threads_actions, V::post, "/", "nope"), 400u, "invalid_json");
  CHECK_ERR(api.as(alice, api::messages_reschedule, V::post, "/", "", id_param(1)), 400u, "invalid_json");
  CHECK_FIELD(api.as(alice, api::messages_cancel_schedule, V::post, "/"), "id");
  CHECK_FIELD(api.as(alice, api::messages_undo_send, V::post, "/"), "id");
  CHECK_FIELD(api.as(alice, api::messages_retry, V::post, "/"), "id");
  CHECK_FIELD(api.as(alice, api::messages_raw, V::get, "/"), "id");
  CHECK_FIELD(api.as(alice, api::drafts_get, V::get, "/"), "id");
  CHECK_FIELD(api.as(alice, api::drafts_send, V::post, "/", "{}"), "id");
  CHECK_ERR(api.as(alice, api::drafts_update, V::put, "/", "{oops", id_param(1)), 400u, "invalid_json");
  CHECK_ERR(api.as(alice, api::messages_patch, V::patch, "/", "", id_param(1)), 400u, "invalid_json");
  CHECK_ERR(api.call(api::threads_list, V::get, "/api/threads"), 401u, "unauthorized");
  CHECK_ERR(api.call(api::drafts_create, V::post, "/api/drafts", "{}"), 401u, "unauthorized");
}

// =============================================================================================
// Integration (need the real WP-A / WP-B / WP-C implementations)
// =============================================================================================

TEST_CASE("register_routes binds every route_table entry", "[api][routes][.integration]") {
  http::Router router;
  api::register_routes(router);
  CHECK(router.size() == api::route_table().size());
  CHECK(router.size() == 56);
  const auto m = router.match(V::get, "/api/files/raw/7");
  REQUIRE(m.route != nullptr);
  CHECK(m.route->pattern == "/api/files/raw/:messageId");
  CHECK(m.params.at("messageId") == "7");
  CHECK(router.match(V::post, "/api/threads/actions").route->pattern == "/api/threads/actions");
  Config cfg;
  cfg.upload_body_limit = 3u << 20;
  http::Router r2;
  api::register_routes(r2, cfg);
  CHECK(r2.match(V::post, "/api/attachments").route->body_limit == (3u << 20));
}

TEST_CASE("webhook: Svix signature verification end to end", "[api][webhooks][.integration]") {
  Api api;
  const std::string secret = "whsec_MfKQ9r8GKYqrTwjUPD8ILPZIo2LaLaSw";
  api.ts.cfg.resend_webhook_secret = secret;
  const std::string body = R"({"type":"email.delivered","created_at":"2026-10-07T00:00:00.000Z","data":{"email_id":"unknown-1","to":["x@ext.example"]}})";
  const std::string ts = std::to_string(api.ts.svc.now_ms() / 1000);
  const std::string sig = resend::sign_svix(secret, "msg_ok", ts, body);
  auto post = [&](std::string id, std::string t, std::string s, std::string b) {
    return api.call(api::webhooks_resend, V::post, "/api/webhooks/resend", std::move(b), std::nullopt, {},
                    {{"svix-id", id}, {"svix-timestamp", t}, {"svix-signature", s}});
  };
  CHECK_ERR(post("msg_ok", ts, "v1,AAAA", body), 401u, "invalid_signature");
  CHECK_ERR(post("msg_ok", ts, sig, body + " "), 401u, "invalid_signature");  // body is signed byte for byte
  const std::string old_ts = std::to_string(api.ts.svc.now_ms() / 1000 - 400);
  CHECK_ERR(post("msg_ok", old_ts, resend::sign_svix(secret, "msg_ok", old_ts, body), body), 401u, "invalid_signature");
  const auto ok = post("msg_ok", ts, sig, body);
  REQUIRE(ok.status == 200u);
  CHECK(body_of(ok) == json::parse(R"({"ok":true})"));
  // Duplicate delivery is still 200 (svix retries) and recorded once.
  CHECK(post("msg_ok", ts, sig, body).status == 200u);
  api.ts.db.read([](db::Conn& c) {
    CHECK(c.scalar<int64_t>("SELECT count(*) FROM webhook_events WHERE svix_id='msg_ok'") == 1);
    CHECK(c.scalar<std::string>("SELECT result FROM webhook_events WHERE svix_id='msg_ok'") == "ignored_unknown");
  });
  const std::string bad = "not json at all";
  CHECK_ERR(post("msg_bad", ts, resend::sign_svix(secret, "msg_bad", ts, bad), bad), 400u, "invalid_json");
}

TEST_CASE("drafts and mail routes are owner-scoped (IDOR)", "[api][mail][.integration]") {
  Api api;
  const auto alice = api.account("alice@team.example", false, "Alice");
  const auto bob = api.account("bob@team.example", false, "Bob");
  const auto c = api.as(alice, api::drafts_create, V::post, "/api/drafts",
                        R"({"to":[{"name":"Bob","email":"bob@team.example"}],"subject":"Hi","html":"<p>hello</p>"})");
  REQUIRE(c.status == 201u);
  const auto d = body_of(c).as_object();
  const int64_t draft = d.at("id").as_int64();
  const int64_t thread = d.at("thread_id").as_int64();
  CHECK(d.at("mode") == "new");

  CHECK(api.as(alice, api::drafts_get, V::get, "/", {}, id_param(draft)).status == 200u);
  CHECK(api.as(alice, api::threads_get, V::get, "/", {}, id_param(thread)).status == 200u);
  CHECK(api.as(alice, api::messages_get, V::get, "/", {}, id_param(draft)).status == 200u);
  CHECK_ERR(api.as(bob, api::drafts_get, V::get, "/", {}, id_param(draft)), 404u, "not_found");
  CHECK_ERR(api.as(bob, api::drafts_update, V::put, "/", R"({"version":1,"subject":"pwned"})", id_param(draft)), 404u,
            "not_found");
  CHECK_ERR(api.as(bob, api::drafts_send, V::post, "/", R"({"version":1})", id_param(draft)), 404u, "not_found");
  CHECK_ERR(api.as(bob, api::drafts_delete, V::delete_, "/", {}, id_param(draft)), 404u, "not_found");
  CHECK_ERR(api.as(bob, api::threads_get, V::get, "/", {}, id_param(thread)), 404u, "not_found");
  CHECK_ERR(api.as(bob, api::messages_get, V::get, "/", {}, id_param(draft)), 404u, "not_found");
  CHECK_ERR(api.as(bob, api::messages_patch, V::patch, "/", R"({"is_read":true})", id_param(draft)), 404u, "not_found");
  CHECK_ERR(api.as(bob, api::messages_events, V::get, "/", {}, id_param(draft)), 404u, "not_found");
  CHECK_ERR(api.as(bob, api::messages_raw, V::get, "/", {}, id_param(draft)), 404u, "not_found");
  CHECK_ERR(api.as(bob, api::messages_undo_send, V::post, "/", {}, id_param(draft)), 404u, "not_found");
  const auto acts = api.as(bob, api::threads_actions, V::post, "/",
                           R"({"thread_ids":[)" + std::to_string(thread) + R"(],"action":"trash"})");
  REQUIRE(acts.status == 200u);
  CHECK(body_of(acts).at("thread_ids").as_array().empty());  // foreign ids are skipped
  CHECK(api.as(alice, api::drafts_get, V::get, "/", {}, id_param(draft)).status == 200u);

  // Version conflict carries the current draft.
  const auto upd = api.as(alice, api::drafts_update, V::put, "/", R"({"version":1,"subject":"v2"})", id_param(draft));
  REQUIRE(upd.status == 200u);
  const auto conflict = api.as(alice, api::drafts_update, V::put, "/", R"({"version":1,"subject":"v3"})", id_param(draft));
  CHECK_ERR(conflict, 409u, "version_conflict");
  CHECK(body_of(conflict).at("error").at("details").at("current").at("subject") == "v2");

  // Send → 202 SendResult, then undo within the window.
  const auto sent = api.as(alice, api::drafts_send, V::post, "/", R"({"version":2})", id_param(draft));
  REQUIRE(sent.status == 202u);
  CHECK(body_of(sent).at("status") == "queued");
  CHECK(body_of(sent).at("undo_ms") == 5000);
  const auto undo = api.as(alice, api::messages_undo_send, V::post, "/", {}, id_param(draft));
  REQUIRE(undo.status == 200u);
  CHECK(body_of(undo).at("draft").at("id").as_int64() == draft);

  CHECK(api.as(alice, api::threads_list, V::get, "/api/threads?folder=drafts").status == 200u);
  CHECK(api.as(alice, api::counts_get, V::get, "/api/counts").status == 200u);
  const auto contacts = api.as(alice, api::contacts_search, V::get, "/api/contacts?q=bob");
  REQUIRE(contacts.status == 200u);
  CHECK(body_of(contacts).at("items").at(0).at("email") == "bob@team.example");
  CHECK(api.as(alice, api::drafts_delete, V::delete_, "/", {}, id_param(draft)).status == 204u);
}

TEST_CASE("upload and signed download", "[api][files][.integration]") {
  Api api;
  const auto alice = api.account("alice@team.example");
  const auto bob = api.account("bob@team.example");
  const auto staged = make_staging_path(api.ts.blobs->tmp_dir());
  {
    std::ofstream f(staged, std::ios::binary);
    f << "<svg onload=alert(1)>";
  }
  http::Request req = http::Request::make(V::post, "/api/attachments?filename=%E6%8A%A5%E5%91%8A.svg&inline=1");
  req.body_file = staged;
  req.body_sha256 = crypto::sha256_file_hex(staged);
  req.body_size = 21;
  req.headers.set("Content-Type", "image/svg+xml");
  http::Ctx ctx{req, api.ts.svc, {}, alice.principal};
  const auto up = api::attachments_upload(ctx);
  REQUIRE(up.status == 201u);
  const auto att = body_of(up).as_object();
  CHECK(att.at("filename") == "报告.svg");
  CHECK(att.at("inline") == true);
  CHECK(att.at("view_url").is_null());  // SVG is never inline-safe
  const std::string url(att.at("download_url").as_string());
  const std::string target = url.substr(url.find("/api/files/"));
  const int64_t id = att.at("id").as_int64();

  const auto r = api.call(api::files_get, V::get, target, {}, std::nullopt, id_param(id));
  REQUIRE(r.status == 200u);
  CHECK(std::holds_alternative<http::FileRef>(r.body));
  CHECK(r.find_header("Content-Security-Policy") == "sandbox");
  CHECK(r.find_header("X-Content-Type-Options") == "nosniff");
  CHECK(std::string(*r.find_header("Content-Disposition")).find("attachment") == 0);
  CHECK(std::string(*r.find_header("Content-Disposition")).find("filename*=UTF-8''") != std::string::npos);

  // Bob cannot reuse Alice's id with his own valid signature.
  const int64_t exp = api.ts.svc.now_ms() + 3600 * 1000;
  const std::string bob_url = api.ts.urls.file_url(id, bob.id, 'a', exp);
  CHECK_ERR(api.call(api::files_get, V::get, bob_url.substr(bob_url.find("/api/files/")), {}, std::nullopt, id_param(id)),
            404u, "not_found");
}

TEST_CASE("cancel-schedule maps Resend rejections", "[api][mail][.integration]") {
  // Exercises begin_cancel_schedule (WP-B) with a fake Resend client; message 1 is not alice's.
  Api api;
  const auto alice = api.account("alice@team.example");
  FakeResend fake;
  api.ts.svc.resend = &fake;
  CHECK_ERR(api.as(alice, api::messages_cancel_schedule, V::post, "/", {}, id_param(1)), 404u, "not_found");
  // A valid time (now + 2 h) so only ownership decides.
  const std::string at = std::to_string(api.ts.svc.now_ms() + 2 * 3600 * 1000);
  CHECK_ERR(api.as(alice, api::messages_reschedule, V::post, "/", R"({"scheduled_at":)" + at + "}", id_param(1)),
            404u, "not_found");
  CHECK(fake.canceled.empty());
}

TEST_CASE("admin outbox and job retry", "[api][admin][.integration]") {
  Api api;
  const auto admin = api.account("admin@team.example", true);
  int64_t job = 0;
  api.ts.db.write([&](db::Tx& tx) {
    job = jobs::enqueue(tx, jobs::kinds::kGcBlobs, {});
    tx.run("UPDATE jobs SET state='dead', attempts=8 WHERE id=?", job);
  });
  const auto r = api.as(admin, api::admin_jobs_retry, V::post, "/", {}, id_param(job));
  REQUIRE(r.status == 200u);
  CHECK(body_of(r).at("state") == "pending");
  CHECK(body_of(r).at("attempts") == 0);
  CHECK_ERR(api.as(admin, api::admin_jobs_retry, V::post, "/", {}, id_param(job)), 409u, "invalid_state");
  CHECK_ERR(api.as(admin, api::admin_outbox_retry, V::post, "/", {}, id_param(999)), 404u, "not_found");
}
