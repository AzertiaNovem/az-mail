// Owner: WP-A — app::App lifecycle. The full wiring (DB, HttpClient, Runner, routes) needs WP-C
// and WP-D implementations, so the end-to-end cases are tagged [.integration].
#include "../fakes/fake_http_server.hpp"
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
  c.server_secret = "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08";  // 256 bits
  c.resend_api_key = "re_test";
  c.resend_api_base = "http://127.0.0.1:9";  // nothing listens: background jobs just retry
  c.allow_insecure_http = true;
  c.resend_webhook_secret = "whsec_MfKQ9r8GKYqrTwjUPD8ILPZIo2LaLaSw";
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

// ---- RT-5 / RT-7 / F6: bounded graceful shutdown ------------------------------------------------

TEST_CASE("app: the shutdown budget fits in AZMAIL_SHUTDOWN_GRACE_SEC", "[app]") {
  using namespace std::chrono_literals;
  const auto b = app::shutdown_budget(25);
  CHECK(b.total == 25s);
  CHECK(b.drain == 5s);
  CHECK(b.after_abort == 5s);
  CHECK(b.abort_at == 20s);
  for (int grace : {1, 2, 5, 10, 25, 30, 120}) {
    INFO(grace);
    const auto x = app::shutdown_budget(grace);
    CHECK(x.total == std::chrono::seconds(grace));
    CHECK(x.drain <= x.abort_at);
    CHECK(x.abort_at + x.after_abort == x.total);
    CHECK(x.after_abort >= 500ms);
  }
  CHECK(app::shutdown_budget(0).total == 1s);
}

TEST_CASE("app: stop() ends within the grace period although a job and a request are stuck (RT-5/RT-7)",
          "[app][.integration]") {
  using namespace std::chrono_literals;
  std::atomic<int> polls{0};
  // Resend that never answers the poller (as a slow or black-holed upstream would).
  test::FakeHttpServer resend([&](const test::FakeRequest& req) {
    if (req.path.rfind("/emails/receiving", 0) == 0) ++polls;
    auto r = test::FakeResponse::json(200, R"({"object":"list","has_more":false,"data":[]})");
    r.delay_before_headers = 120s;
    return r;
  });
  TempDir td;
  Config c = app_config(td);
  c.resend_api_base = resend.base_url();
  c.resend_timeout_sec = 120;
  c.header_timeout_sec = 120;
  c.shutdown_grace_sec = 3;
  app::App a(c);
  a.start();
  REQUIRE(eventually([&] { return polls.load() > 0; }, 10s));  // poll.receiving is waiting on Resend
  RawClient slow(a.port());
  slow.send("GET /api/health HTTP/1.1\r\nHost: t\r\n");  // a request header that never ends
  std::this_thread::sleep_for(200ms);

  const auto t0 = std::chrono::steady_clock::now();
  a.stop();
  const auto took = std::chrono::steady_clock::now() - t0;
  CHECK(took < 3500ms);  // not the 120 s Resend stall nor the 120 s header timeout
  CHECK(RawClient::is_closed_error(slow.wait_closed(1000ms)));

  // The interrupted job recorded its outcome: nothing is left 'running' behind a lease.
  db::Pool pool(c.db_path, 1);
  pool.read([](db::Conn& conn) {
    CHECK(conn.scalar<int64_t>("SELECT count(*) FROM jobs WHERE state='running'") == std::optional<int64_t>(0));
  });
}
