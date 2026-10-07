// Owner: WP-A — WebSocket session against the in-process server with a Beast client: Origin
// check, first-message auth (deadline, bad token → 4401), ready/ping/pong, Hub fan-out,
// revocation, periodic re-authentication, message size limit, send-queue overflow, shutdown.
#include "http_harness.hpp"
#include "ws/events.hpp"
#include "ws/ws_session.hpp"

#include <catch2/catch_test_macros.hpp>

#include <boost/json/parse.hpp>

using namespace azm;
using namespace azm::test;

namespace {

boost::json::object obj(const std::string& s) { return boost::json::parse(s).as_object(); }

std::string auth_msg(std::string_view token) { return R"({"type":"auth","token":")" + std::string(token) + "\"}"; }

// Connects, authenticates and consumes the ready frame.
void login(WsClient& c, TestServer& s, std::string_view token) {
  REQUIRE_FALSE(c.connect(s.port()));
  REQUIRE_FALSE(c.send(auth_msg(token)));
  auto r = c.read();
  REQUIRE(std::holds_alternative<std::string>(r));
  REQUIRE(obj(std::get<std::string>(r))["type"] == "ready");
}

}  // namespace

TEST_CASE("ws: parse_client_message", "[ws]") {
  auto m = ws::parse_client_message(R"({"type":"auth","token":"abc"})");
  CHECK(m.type == ws::ClientMessageType::Auth);
  CHECK(m.token == "abc");
  m = ws::parse_client_message(R"({"type":"auth"})");
  CHECK(m.type == ws::ClientMessageType::Auth);
  CHECK(m.token.empty());
  m = ws::parse_client_message(R"({"type":"auth","token":42})");
  CHECK(m.type == ws::ClientMessageType::Auth);
  CHECK(m.token.empty());
  CHECK(ws::parse_client_message(R"({"type":"ping"})").type == ws::ClientMessageType::Ping);
  CHECK(ws::parse_client_message(R"({"type":"subscribe"})").type == ws::ClientMessageType::Other);
  CHECK(ws::parse_client_message(R"({"type":1})").type == ws::ClientMessageType::Invalid);
  CHECK(ws::parse_client_message(R"({"token":"x"})").type == ws::ClientMessageType::Invalid);
  CHECK(ws::parse_client_message("[1,2]").type == ws::ClientMessageType::Invalid);
  CHECK(ws::parse_client_message("not json").type == ws::ClientMessageType::Invalid);
  CHECK(ws::parse_client_message("").type == ws::ClientMessageType::Invalid);
  std::string deep(100, '[');
  CHECK(ws::parse_client_message(deep).type == ws::ClientMessageType::Invalid);
}

TEST_CASE("ws: auth, ready, ping/pong, hub fan-out, detach", "[ws][server]") {
  TestServer s;
  s.resolver.add("good", 5, 50);
  s.start();
  WsClient c;
  REQUIRE_FALSE(c.connect(s.port()));
  REQUIRE_FALSE(c.send(auth_msg("good")));
  auto r = c.read();
  REQUIRE(std::holds_alternative<std::string>(r));
  auto ready = obj(std::get<std::string>(r));
  CHECK(ready["type"] == "ready");
  CHECK(ready["user_id"] == 5);
  CHECK(ready["server_time"].as_int64() > 1'600'000'000'000);
  CHECK(eventually([&] { return s.hub.connection_count(5) == 1; }));

  REQUIRE_FALSE(c.send(R"({"type":"ping"})"));
  r = c.read();
  REQUIRE(std::holds_alternative<std::string>(r));
  CHECK(std::get<std::string>(r) == R"({"type":"pong"})");

  // Unknown and non-JSON messages are ignored; the socket stays open.
  REQUIRE_FALSE(c.send(R"({"type":"subscribe"})"));
  REQUIRE_FALSE(c.send("garbage"));
  REQUIRE_FALSE(c.send(auth_msg("good")));  // a second auth is ignored too
  REQUIRE_FALSE(c.send(R"({"type":"ping"})"));
  r = c.read();
  REQUIRE(std::holds_alternative<std::string>(r));
  CHECK(std::get<std::string>(r) == R"({"type":"pong"})");

  // Events published through the Notifier arrive as flat frames.
  const int64_t ids[] = {11, 12};
  Notifier& n = s.hub;
  n.publish(5, std::string(ws::events::kThreadsChanged), ws::threads_changed_payload(ids));
  n.publish(6, std::string(ws::events::kThreadsChanged), ws::threads_changed_payload(ids));  // other user
  n.publish(5, std::string(ws::events::kMailNew),
            ws::mail_new_payload(1, 2, Address{"张三", "z@x.cn"}, "周报", "摘要", true, false));
  r = c.read();
  REQUIRE(std::holds_alternative<std::string>(r));
  auto ev = obj(std::get<std::string>(r));
  CHECK(ev["type"] == "threads.changed");
  CHECK(ev["thread_ids"].as_array().size() == 2);
  r = c.read();
  REQUIRE(std::holds_alternative<std::string>(r));
  ev = obj(std::get<std::string>(r));
  CHECK(ev["type"] == "mail.new");
  CHECK(ev["from"].as_object()["name"] == "张三");
  CHECK(ev["subject"] == "周报");

  // A second socket of the same user gets the same events.
  WsClient c2;
  login(c2, s, "good");
  CHECK(eventually([&] { return s.hub.connection_count(5) == 2; }));
  n.publish(5, std::string(ws::events::kLabelsChanged), {});
  CHECK(c.read_type("labels.changed"));
  CHECK(c2.read_type("labels.changed"));

  // Client-initiated close → the Hub forgets the socket.
  {
    beast::error_code ec;
    beast::get_lowest_layer(c2.ws()).expires_after(3s);
    c2.ws().close(websocket::close_code::normal, ec);
  }
  CHECK(eventually([&] { return s.hub.connection_count(5) == 1; }));
}

TEST_CASE("ws: invalid token or first message closes 4401", "[ws][server]") {
  TestServer s;
  s.resolver.add("good", 1, 1);
  s.start();
  {
    WsClient c;
    REQUIRE_FALSE(c.connect(s.port()));
    REQUIRE_FALSE(c.send(auth_msg("bad")));
    CHECK(c.wait_close() == ws::kCloseAuthFailed);
  }
  {
    WsClient c;
    REQUIRE_FALSE(c.connect(s.port()));
    REQUIRE_FALSE(c.send(R"({"type":"ping"})"));  // anything but auth first
    CHECK(c.wait_close() == ws::kCloseAuthFailed);
  }
  {
    WsClient c;
    REQUIRE_FALSE(c.connect(s.port()));
    REQUIRE_FALSE(c.send("{"));
    CHECK(c.wait_close() == ws::kCloseAuthFailed);
  }
  {
    WsClient c;
    REQUIRE_FALSE(c.connect(s.port()));
    REQUIRE_FALSE(c.send(auth_msg("")));
    CHECK(c.wait_close() == ws::kCloseAuthFailed);
  }
  CHECK(s.hub.connection_count() == 0);
}

TEST_CASE("ws: auth deadline", "[ws][server]") {
  TestServer s([](Config& c) { c.ws_auth_timeout_ms = 300; });
  s.start();
  WsClient c;
  REQUIRE_FALSE(c.connect(s.port()));
  const auto t0 = std::chrono::steady_clock::now();
  CHECK(c.wait_close(3000ms) == ws::kCloseAuthFailed);
  const auto took = std::chrono::steady_clock::now() - t0;
  CHECK(took >= 250ms);
  CHECK(took < 2500ms);
}

TEST_CASE("ws: Origin must be in the CORS allowlist", "[ws][server]") {
  TestServer s;
  s.resolver.add("good", 1, 1);
  s.start();
  {
    WsClient c;
    auto ec = c.connect(s.port(), std::string("https://evil.example"));
    CHECK(ec == websocket::error::upgrade_declined);
    CHECK(c.response.result_int() == 403);
  }
  {
    WsClient c;
    auto ec = c.connect(s.port(), std::nullopt);  // no Origin at all (non-browser clients must send one)
    CHECK(ec == websocket::error::upgrade_declined);
    CHECK(c.response.result_int() == 403);
  }
  {
    WsClient c;  // allowed origin, case-insensitive host
    CHECK_FALSE(c.connect(s.port(), std::string("HTTP://LOCALHOST:5173")));
  }
}

TEST_CASE("ws: revoke_session / revoke_user send session.revoked then 4401", "[ws][server]") {
  TestServer s;
  s.resolver.add("t1", 9, 91);
  s.resolver.add("t2", 9, 92);
  s.start();
  WsClient a, b;
  login(a, s, "t1");
  login(b, s, "t2");
  CHECK(eventually([&] { return s.hub.connection_count(9) == 2; }));
  s.hub.revoke_session(91);
  auto r = a.read();
  REQUIRE(std::holds_alternative<std::string>(r));
  CHECK(std::get<std::string>(r) == R"({"type":"session.revoked"})");
  CHECK(a.wait_close() == ws::kCloseAuthFailed);
  // The other session is untouched.
  s.hub.publish(9, "labels.changed", {});
  CHECK(b.read_type("labels.changed"));
  s.hub.revoke_user(9);
  CHECK(b.read_type("session.revoked"));
  CHECK(b.wait_close() == ws::kCloseAuthFailed);
  CHECK(eventually([&] { return s.hub.connection_count() == 0; }));
}

TEST_CASE("ws: periodic re-authentication closes revoked sessions", "[ws][server]") {
  TestServer s;
  s.resolver.add("t", 4, 40);
  s.start(250ms);
  WsClient c;
  login(c, s, "t");
  // Still valid after a few periods.
  std::this_thread::sleep_for(600ms);
  REQUIRE_FALSE(c.send(R"({"type":"ping"})"));
  CHECK(c.read_type("pong"));
  // Revoked elsewhere (e.g. `azmail reset-password` in another process).
  s.resolver.remove("t");
  CHECK(c.read_type("session.revoked", 3000ms));
  CHECK(c.wait_close() == ws::kCloseAuthFailed);
}

TEST_CASE("ws: messages over read_message_max close the socket", "[ws][server]") {
  TestServer s([](Config& c) { c.ws_max_message_bytes = 1024; });
  s.resolver.add("t", 1, 1);
  s.start();
  WsClient c;
  login(c, s, "t");
  REQUIRE_FALSE(c.send(std::string(4096, 'x')));
  // Beast fails the connection with 1009 as soon as the frame header announces the size; the
  // unread payload may turn the TCP close into a RST that overtakes the close frame (code 0).
  const auto code = c.wait_close();
  CHECK((code == static_cast<std::uint16_t>(websocket::close_code::too_big) || code == 0));
  CHECK(eventually([&] { return s.hub.connection_count() == 0; }));
}

TEST_CASE("ws: send-queue overflow closes a client that does not read", "[ws][server]") {
  TestServer s([](Config& c) { c.ws_queue_cap = 4; });
  s.resolver.add("t", 2, 20);
  s.start();
  WsClient c;
  login(c, s, "t");
  REQUIRE(eventually([&] { return s.hub.connection_count(2) == 1; }));
  // The client stops reading; big frames fill the socket buffers, then the queue.
  boost::json::object big;
  big["pad"] = std::string(256 * 1024, 'p');
  for (int i = 0; i < 400 && s.hub.connection_count(2) == 1; ++i) {
    s.hub.publish(2, "threads.changed", big);
    if (i % 50 == 49) std::this_thread::sleep_for(20ms);
  }
  // Overflow → close requested; the stalled close handshake is cut by the hard-close timer.
  CHECK(eventually([&] { return s.hub.connection_count(2) == 0; }, 10000ms));
  // Draining what is left ends in an error rather than hanging.
  bool ended = false;
  for (int i = 0; i < 1000 && !ended; ++i) ended = std::holds_alternative<beast::error_code>(c.read(3000ms));
  CHECK(ended);
}

TEST_CASE("ws: shutdown sends going_away", "[ws][server]") {
  TestServer s;
  s.resolver.add("t", 3, 30);
  s.start();
  WsClient c;
  login(c, s, "t");
  REQUIRE(eventually([&] { return s.hub.connection_count(3) == 1; }));
  s.server->stop();
  s.hub.close_all();
  CHECK(c.wait_close() == ws::kCloseGoingAway);
  CHECK(eventually([&] { return s.server->connections() == 0; }));
}

TEST_CASE("ws: a database error during auth closes 1011", "[ws][server]") {
  struct Throwing final : http::SessionResolver {
    std::optional<http::Principal> resolve(Services&, std::string_view) override {
      throw std::runtime_error("db down");
    }
  } throwing;
  TestServer s;
  s.ts.svc.session_resolver = &throwing;
  s.start();
  WsClient c;
  REQUIRE_FALSE(c.connect(s.port()));
  REQUIRE_FALSE(c.send(auth_msg("x")));
  CHECK(c.wait_close() == ws::kCloseInternal);
}
