// Owner: WP-A — app::App lifecycle. The full wiring (DB, HttpClient, Runner, routes) needs WP-C
// and WP-D implementations, so the end-to-end cases are tagged [.integration].
#include "app/app.hpp"
#include "core/crypto.hpp"
#include "http_harness.hpp"
#include "services.hpp"
#include "test_support.hpp"
#include "ws/events.hpp"

#include <catch2/catch_test_macros.hpp>

#include <boost/json/parse.hpp>

using namespace azm;
using namespace azm::test;

namespace {

Config app_config(const TempDir& td) {
  Config c;
  c.listen_address = "127.0.0.1";
  c.listen_port = 0;
  c.data_dir = (td / "data").string();
  c.db_path = (td / "data" / "azmail.db").string();
  c.server_secret = std::string(kTestSecret);
  c.resend_api_key = "re_test";
  c.resend_api_base = "http://127.0.0.1:9";  // nothing listens: background jobs just retry
  c.allow_insecure_http = true;
  c.resend_webhook_secret = "whsec_dGVzdA==";
  c.io_threads = 1;
  c.db_threads = 2;
  c.net_threads = 1;
  c.files_threads = 1;
  c.db_pool_size = 12;
  c.log_level = "warn";
  c.shutdown_grace_sec = 2;
  return c;
}

std::string login_body(std::string_view email, std::string_view password) {
  return R"({"email":")" + std::string(email) + R"(","password":")" + std::string(password) + "\"}";
}

std::string post(std::string_view target, std::string_view body, std::string_view extra = {}) {
  return "POST " + std::string(target) +
         " HTTP/1.1\r\nHost: t\r\nContent-Type: application/json\r\nOrigin: http://localhost:5173\r\nContent-Length: " +
         std::to_string(body.size()) + "\r\n" + std::string(extra) + "\r\n" + std::string(body);
}

}  // namespace

TEST_CASE("app: invalid configuration is rejected before anything starts", "[app]") {
  TempDir td;
  Config c = app_config(td);
  c.server_secret.clear();
  c.cors_origins = {"*"};
  app::App a(c);
  try {
    a.start();
    FAIL("expected start() to throw");
  } catch (const std::runtime_error& e) {
    const std::string msg = e.what();
    CHECK(msg.find("AZMAIL_SECRET") != std::string::npos);
    CHECK(msg.find("AZMAIL_CORS_ORIGINS") != std::string::npos);
  }
  CHECK_FALSE(std::filesystem::exists(c.db_path));  // nothing was created
  CHECK_THROWS_AS(a.start(), std::logic_error);    // one start per App
  CHECK_NOTHROW(a.stop());
  CHECK(a.config().data_dir == c.data_dir);
}

TEST_CASE("app: accessors before start, stop without start", "[app]") {
  TempDir td;
  app::App a(app_config(td));
  CHECK_THROWS_AS(a.port(), std::logic_error);
  CHECK_THROWS_AS(a.services(), std::logic_error);
  CHECK_NOTHROW(a.stop());
  CHECK_NOTHROW(a.stop());
}

TEST_CASE("app: health and login over real HTTP, WS revoke on logout", "[app][.integration]") {
  TempDir td;
  app::App a(app_config(td));
  a.start();
  REQUIRE(a.port() != 0);
  Services& svc = a.services();
  CHECK(svc.runner != nullptr);
  CHECK(svc.resend != nullptr);
  CHECK(svc.login_throttle != nullptr);
  CHECK(svc.db_workers != nullptr);
  CHECK(std::string(svc.blobs.kind()) == "local");

  RawClient c(a.port());
  auto r = c.request("GET /api/health HTTP/1.1\r\nHost: t\r\n\r\n");
  CHECK(r.result_int() == 200);
  auto health = boost::json::parse(r.body()).as_object();
  CHECK(health["status"] == "ok");

  const std::string hash = crypto::password_hash("correct horse battery");
  svc.db.write([&](db::Tx& tx) { seed_user(tx, "alice@team.example", true, "Alice", hash); });

  r = c.request(post("/api/auth/login", login_body("alice@team.example", "wrong password")));
  CHECK(r.result_int() == 401);
  r = c.request(post("/api/auth/login", login_body("alice@team.example", "correct horse battery")));
  REQUIRE(r.result_int() == 200);
  CHECK(std::string(r[bhttp::field::access_control_allow_origin]) == "http://localhost:5173");
  const std::string token(boost::json::parse(r.body()).as_object()["token"].as_string());

  r = c.request("GET /api/auth/me HTTP/1.1\r\nHost: t\r\nAuthorization: Bearer " + token + "\r\n\r\n");
  CHECK(r.result_int() == 200);

  WsClient ws;
  REQUIRE_FALSE(ws.connect(a.port()));
  REQUIRE_FALSE(ws.send(R"({"type":"auth","token":")" + token + "\"}"));
  CHECK(ws.read_type("ready"));

  r = c.request(post("/api/auth/logout", "", "Authorization: Bearer " + token + "\r\n"));
  CHECK(r.result_int() == 204);
  CHECK(ws.read_type("session.revoked"));
  CHECK(ws.wait_close() == ws::kCloseAuthFailed);
  r = c.request("GET /api/auth/me HTTP/1.1\r\nHost: t\r\nAuthorization: Bearer " + token + "\r\n\r\n");
  CHECK(r.result_int() == 401);

  // Login throttle: 5 failures per email → 429 with retry_after.
  for (int i = 0; i < 5; ++i) c.request(post("/api/auth/login", login_body("alice@team.example", "nope nope")));
  r = c.request(post("/api/auth/login", login_body("alice@team.example", "correct horse battery")));
  CHECK(r.result_int() == 429);
  auto err = boost::json::parse(r.body()).as_object()["error"].as_object();
  CHECK(err["code"] == "too_many_attempts");
  CHECK(err["details"].as_object()["retry_after"].as_int64() >= 1);

  a.stop();
  a.stop();
}
