// Owner: WP-A — the HTTP session against a real in-process server on an ephemeral port:
// standard headers, 404/405, preflight, 413 pre-check, body limits, keep-alive, timeouts
// (slowloris), Expect: 100-continue, auth modes, File-mode streaming (+sha, tmp deletion),
// Range responses, redirects, in-flight cap, pools, XFF, connection cap, graceful stop.
#include "core/crypto.hpp"
#include "core/strings.hpp"
#include "http/blocking.hpp"
#include "http/session.hpp"
#include "http_harness.hpp"

#include <catch2/catch_test_macros.hpp>

#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>

#include <condition_variable>
#include <fstream>
#include <latch>

using namespace azm;
using namespace azm::test;
using V = boost::beast::http::verb;

namespace {

boost::json::object json_of(const bhttp::response<bhttp::string_body>& r) {
  return boost::json::parse(r.body()).as_object();
}
std::string error_code_of(const bhttp::response<bhttp::string_body>& r) {
  return std::string(json_of(r)["error"].as_object()["code"].as_string());
}
std::string header(const bhttp::response<bhttp::string_body>& r, std::string_view name) {
  auto it = r.find(boost::core::string_view(name.data(), name.size()));
  return it == r.end() ? std::string() : std::string(it->value());
}
std::size_t count_files(const std::filesystem::path& dir) {
  std::size_t n = 0;
  std::error_code ec;
  for (auto it = std::filesystem::directory_iterator(dir, ec); !ec && it != std::filesystem::directory_iterator();
       it.increment(ec))
    ++n;
  return n;
}

http::Response echo(http::Ctx& ctx) {
  boost::json::object o;
  o["path"] = ctx.req.path;
  o["method"] = std::string(beast::http::to_string(ctx.req.method));
  o["body_size"] = ctx.req.body_size;
  o["body"] = ctx.req.body;
  o["ip"] = ctx.req.remote_ip;
  o["request_id"] = ctx.req.request_id;
  if (auto q = ctx.query("q")) o["q"] = *q;
  for (const auto& [k, v] : ctx.params) o["param_" + k] = v;
  if (ctx.principal) {
    o["user_id"] = ctx.principal->user_id;
    o["session_id"] = ctx.principal->session_id;
  }
  return http::Response::json(o);
}

std::string get(std::string_view target, std::string_view extra = {}) {
  return "GET " + std::string(target) + " HTTP/1.1\r\nHost: t\r\n" + std::string(extra) + "\r\n";
}

std::string post(std::string_view target, std::string_view body, std::string_view extra = {}) {
  return "POST " + std::string(target) + " HTTP/1.1\r\nHost: t\r\nContent-Type: application/json\r\nContent-Length: " +
         std::to_string(body.size()) + "\r\n" + std::string(extra) + "\r\n" + std::string(body);
}

// The standard routes most cases use.
void add_basic_routes(TestServer& s) {
  s.add(V::get, "/api/echo", echo);
  s.add(V::get, "/api/items/:id", echo);
  s.add(V::post, "/api/items", echo, http::AuthReq::None, http::BodyMode::Json, 1024);
  s.add(V::post, "/api/raw", echo, http::AuthReq::None, http::BodyMode::Raw, 64);
  s.add(V::post, "/api/bodyless", echo, http::AuthReq::None, http::BodyMode::None, 16);
  s.add(V::get, "/api/me", echo, http::AuthReq::User);
  s.add(V::get, "/api/admin/x", echo, http::AuthReq::Admin);
  s.add(V::get, "/api/fail/api", [](http::Ctx&) -> http::Response {
    throw ApiError::conflict("version_conflict", "已在其他窗口修改", {{"current", 3}});
  });
  s.add(V::get, "/api/fail/std", [](http::Ctx&) -> http::Response { throw std::runtime_error("boom"); });
  s.add(V::get, "/api/fail/notimpl", [](http::Ctx&) -> http::Response { throw NotImplemented("x"); });
  s.add(V::get, "/api/redirect",
        [](http::Ctx&) { return http::Response::redirect("https://r2.example/bucket/key?X-Amz-Signature=abc"); });
  s.add(V::delete_, "/api/items/:id", [](http::Ctx&) { return http::Response::no_content(); }, http::AuthReq::None,
        http::BodyMode::None, 4096);
}

}  // namespace

TEST_CASE("session: pure helpers", "[session]") {
  const std::vector<std::string> trusted = {"127.0.0.1", "::1", "10.0.0.0/8", "fd00::/8"};
  CHECK(http::resolve_client_ip("127.0.0.1", std::nullopt, trusted) == "127.0.0.1");
  CHECK(http::resolve_client_ip("127.0.0.1", std::string_view("203.0.113.5"), trusted) == "203.0.113.5");
  CHECK(http::resolve_client_ip("127.0.0.1", std::string_view("1.1.1.1, 203.0.113.5, 10.1.2.3"), trusted) ==
        "203.0.113.5");
  // All hops trusted → the left-most one.
  CHECK(http::resolve_client_ip("10.0.0.1", std::string_view("10.9.9.9, 10.8.8.8"), trusted) == "10.9.9.9");
  // Untrusted peer: XFF is ignored entirely (spoofing).
  CHECK(http::resolve_client_ip("198.51.100.7", std::string_view("1.2.3.4"), trusted) == "198.51.100.7");
  // Garbage where a proxy would have written an address → the peer.
  CHECK(http::resolve_client_ip("127.0.0.1", std::string_view("1.2.3.4, bogus"), trusted) == "127.0.0.1");
  CHECK(http::resolve_client_ip("::ffff:127.0.0.1", std::string_view("[2001:db8::1]"), trusted) == "2001:db8::1");
  CHECK(http::resolve_client_ip("::1", std::string_view("fd12::5, 2001:db8::9"), trusted) == "2001:db8::9");

  CHECK(http::ip_in_list("10.200.3.4", trusted));
  CHECK(http::ip_in_list("::ffff:10.0.0.1", trusted));
  CHECK(http::ip_in_list("fd00::1", trusted));
  CHECK_FALSE(http::ip_in_list("11.0.0.1", trusted));
  CHECK_FALSE(http::ip_in_list("fe80::1", trusted));
  CHECK_FALSE(http::ip_in_list("garbage", trusted));
  const std::vector<std::string> odd = {"192.168.1.0/24", "1.2.3.4/0x", "300.1.1.1", "0.0.0.0/0"};
  CHECK(http::ip_in_list("192.168.1.200", std::vector<std::string>{"192.168.1.0/24"}));
  CHECK_FALSE(http::ip_in_list("192.168.2.1", std::vector<std::string>{"192.168.1.0/24"}));
  CHECK(http::ip_in_list("8.8.8.8", odd));  // 0.0.0.0/0 matches every IPv4
  CHECK(http::ip_in_list("172.16.5.5", std::vector<std::string>{"172.16.0.0/12"}));
  CHECK_FALSE(http::ip_in_list("172.32.0.1", std::vector<std::string>{"172.16.0.0/12"}));

  http::Route r{V::post, "/x", http::AuthReq::None, http::BodyMode::Json, 100, http::Exec::Db, echo};
  CHECK_FALSE(http::precheck_body(r, std::nullopt));
  CHECK_FALSE(http::precheck_body(r, 100));
  auto too = http::precheck_body(r, 101);
  REQUIRE(too);
  CHECK(too->status == 413);
  CHECK(boost::json::parse(std::get<std::string>(too->body)).at("error").at("code") == "payload_too_large");
  r.body_limit = 0;
  CHECK(http::precheck_body(r, 1));
  CHECK_FALSE(http::precheck_body(r, 0));

  const auto id = http::make_request_id();
  CHECK(id.size() == 16);
  CHECK(id.find_first_not_of("0123456789abcdef") == std::string::npos);
  CHECK(http::make_request_id() != id);

  http::ByteRange br;
  CHECK(http::parse_range("bytes=0-9", 100, br) == http::RangeResult::Ok);
  CHECK((br.first == 0 && br.last == 9));
  CHECK(http::parse_range("bytes=90-", 100, br) == http::RangeResult::Ok);
  CHECK((br.first == 90 && br.last == 99));
  CHECK(http::parse_range("bytes=-10", 100, br) == http::RangeResult::Ok);
  CHECK((br.first == 90 && br.last == 99));
  CHECK(http::parse_range("bytes=-1000", 100, br) == http::RangeResult::Ok);
  CHECK((br.first == 0 && br.last == 99));
  CHECK(http::parse_range("bytes=50-1000", 100, br) == http::RangeResult::Ok);
  CHECK(br.last == 99);
  CHECK(http::parse_range(" Bytes = 1-2", 100, br) == http::RangeResult::None);
  CHECK(http::parse_range("BYTES=1-2", 100, br) == http::RangeResult::Ok);
  CHECK(http::parse_range("bytes=100-", 100, br) == http::RangeResult::Unsatisfiable);
  CHECK(http::parse_range("bytes=-0", 100, br) == http::RangeResult::Unsatisfiable);
  CHECK(http::parse_range("bytes=0-0", 0, br) == http::RangeResult::Unsatisfiable);
  CHECK(http::parse_range("bytes=5-1", 100, br) == http::RangeResult::None);
  CHECK(http::parse_range("bytes=0-1,5-6", 100, br) == http::RangeResult::None);
  CHECK(http::parse_range("items=0-1", 100, br) == http::RangeResult::None);
  CHECK(http::parse_range("bytes=a-b", 100, br) == http::RangeResult::None);
  CHECK(http::parse_range("bytes=", 100, br) == http::RangeResult::None);
}

TEST_CASE("session: standard headers, CORS, 404/405, errors", "[session][server]") {
  TestServer s;
  add_basic_routes(s);
  s.start();
  RawClient c(s.port());

  auto r = c.request(get("/api/echo?q=%E5%91%A8%20x", "Origin: http://localhost:5173\r\n"));
  CHECK(r.result_int() == 200);
  CHECK(header(r, "Content-Type") == "application/json; charset=utf-8");
  CHECK(header(r, "X-Content-Type-Options") == "nosniff");
  CHECK(header(r, "Cache-Control") == "no-store");
  CHECK(header(r, "Referrer-Policy") == "no-referrer");
  CHECK(header(r, "Vary") == "Origin");
  CHECK(header(r, "Access-Control-Allow-Origin") == "http://localhost:5173");
  CHECK(header(r, "Access-Control-Expose-Headers").find("X-Request-Id") != std::string::npos);
  const auto rid = header(r, "X-Request-Id");
  CHECK(rid.size() == 16);
  auto body = json_of(r);
  CHECK(body["request_id"].as_string() == rid);
  CHECK(body["q"] == "周 x");
  CHECK(body["ip"] == "127.0.0.1");

  // Disallowed origin: no ACAO, still Vary.
  r = c.request(get("/api/echo", "Origin: https://evil.example\r\n"));
  CHECK(r.result_int() == 200);
  CHECK(header(r, "Access-Control-Allow-Origin").empty());
  CHECK(header(r, "Vary") == "Origin");

  r = c.request(get("/api/items/42"));
  CHECK(json_of(r)["param_id"] == "42");

  r = c.request(get("/api/nope"));
  CHECK(r.result_int() == 404);
  CHECK(error_code_of(r) == "not_found");
  CHECK(header(r, "X-Request-Id").size() == 16);

  r = c.request("PUT /api/items HTTP/1.1\r\nHost: t\r\nContent-Length: 0\r\n\r\n");
  CHECK(r.result_int() == 405);
  CHECK(error_code_of(r) == "method_not_allowed");
  CHECK(header(r, "Allow") == "POST, OPTIONS");
  r = c.request("PATCH /api/items/1 HTTP/1.1\r\nHost: t\r\nContent-Length: 0\r\n\r\n");
  CHECK(r.result_int() == 405);
  CHECK(header(r, "Allow") == "GET, DELETE, OPTIONS");

  r = c.request(get("/api/fail/api"));
  CHECK(r.result_int() == 409);
  CHECK(error_code_of(r) == "version_conflict");
  CHECK(json_of(r)["error"].as_object()["details"].as_object()["current"] == 3);
  r = c.request(get("/api/fail/std"));
  CHECK(r.result_int() == 500);
  CHECK(error_code_of(r) == "internal_error");
  r = c.request(get("/api/fail/notimpl"));
  CHECK(r.result_int() == 500);

  r = c.request(get("/api/redirect"));
  CHECK(r.result_int() == 302);
  CHECK(header(r, "Location") == "https://r2.example/bucket/key?X-Amz-Signature=abc");
  CHECK(header(r, "Content-Length") == "0");

  r = c.request("DELETE /api/items/3 HTTP/1.1\r\nHost: t\r\n\r\n");
  CHECK(r.result_int() == 204);
  CHECK(header(r, "Content-Length").empty());
  CHECK(r.body().empty());

  // /api/ws without an upgrade.
  r = c.request(get("/api/ws"));
  CHECK(r.result_int() == 400);
  // Still the same keep-alive connection after all of that.
  r = c.request(get("/api/echo"));
  CHECK(r.result_int() == 200);
}

TEST_CASE("session: CORS preflight", "[session][server]") {
  TestServer s;
  add_basic_routes(s);
  s.start();
  RawClient c(s.port());
  auto r = c.request(
      "OPTIONS /api/items HTTP/1.1\r\nHost: t\r\nOrigin: http://localhost:5173\r\n"
      "Access-Control-Request-Method: POST\r\nAccess-Control-Request-Headers: authorization, content-type\r\n\r\n");
  CHECK(r.result_int() == 204);
  CHECK(header(r, "Access-Control-Allow-Origin") == "http://localhost:5173");
  CHECK(header(r, "Access-Control-Allow-Headers").find("Idempotency-Key") != std::string::npos);
  CHECK(header(r, "Access-Control-Max-Age") == "600");
  CHECK(header(r, "X-Request-Id").size() == 16);
  // A preflight for any path is answered inline (no route needed, never reaches a handler).
  r = c.request("OPTIONS /api/whatever HTTP/1.1\r\nHost: t\r\nOrigin: http://127.0.0.1:5173\r\n\r\n");
  CHECK(r.result_int() == 204);
  r = c.request("OPTIONS /api/items HTTP/1.1\r\nHost: t\r\nOrigin: https://evil.example\r\n\r\n");
  CHECK(r.result_int() == 403);
  CHECK(error_code_of(r) == "forbidden");
  CHECK(header(r, "Access-Control-Allow-Origin").empty());
}

TEST_CASE("session: body limits (413 pre-check, chunked, None mode)", "[session][server]") {
  TestServer s;
  add_basic_routes(s);
  s.start();

  {  // Declared length over the limit: 413 without reading (and without waiting for) the body.
    RawClient c(s.port());
    c.send("POST /api/items HTTP/1.1\r\nHost: t\r\nContent-Type: application/json\r\nContent-Length: 5000\r\n\r\n");
    auto r = c.read();
    CHECK(r.result_int() == 413);
    CHECK(error_code_of(r) == "payload_too_large");
    CHECK(json_of(r)["error"].as_object()["details"].as_object()["limit"] == 1024);
    CHECK_FALSE(r.keep_alive());
    CHECK(RawClient::is_closed_error(c.wait_closed()));
  }
  {  // Within the limit.
    RawClient c(s.port());
    auto r = c.request(post("/api/items", R"({"a":1})"));
    CHECK(r.result_int() == 200);
    CHECK(json_of(r)["body"] == R"({"a":1})");
    CHECK(json_of(r)["body_size"] == 7);
  }
  {  // Chunked over the limit: detected while reading.
    RawClient c(s.port());
    const std::string chunk(800, 'x');
    std::ostringstream req;
    req << "POST /api/items HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: chunked\r\n\r\n";
    for (int i = 0; i < 3; ++i) req << std::hex << chunk.size() << "\r\n" << chunk << "\r\n";
    req << "0\r\n\r\n";
    auto r = c.request(req.str());
    CHECK(r.result_int() == 413);
  }
  {  // Chunked within the limit works.
    RawClient c(s.port());
    auto r = c.request("POST /api/raw HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n");
    CHECK(r.result_int() == 200);
    CHECK(json_of(r)["body"] == "hello");
  }
  {  // BodyMode::None: small bodies are read and discarded, larger ones are 413.
    RawClient c(s.port());
    auto r = c.request(post("/api/bodyless", "{}"));
    CHECK(r.result_int() == 200);
    CHECK(json_of(r)["body"] == "");
    CHECK(json_of(r)["body_size"] == 2);
    r = c.request(post("/api/bodyless", std::string(17, 'x')));
    CHECK(r.result_int() == 413);
  }
  {  // GET with a body: limit 0.
    RawClient c(s.port());
    auto r = c.request("GET /api/echo HTTP/1.1\r\nHost: t\r\nContent-Length: 3\r\n\r\nabc");
    CHECK(r.result_int() == 413);
  }
}

TEST_CASE("session: keep-alive, HTTP/1.0, pipelining", "[session][server]") {
  TestServer s([](Config& c) { c.keepalive_timeout_sec = 1; });
  add_basic_routes(s);
  s.start();
  {
    RawClient c(s.port());
    std::string rid1, rid2;
    auto r = c.request(get("/api/echo"));
    CHECK(r.keep_alive());
    rid1 = header(r, "X-Request-Id");
    r = c.request(get("/api/echo"));
    rid2 = header(r, "X-Request-Id");
    CHECK(rid1 != rid2);
    // Two pipelined requests in one write.
    c.send(get("/api/items/1") + get("/api/items/2"));
    CHECK(json_of(c.read())["param_id"] == "1");
    CHECK(json_of(c.read())["param_id"] == "2");
    // Idle keep-alive timeout (1 s) closes the connection.
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(RawClient::is_closed_error(c.wait_closed(4000ms)));
    CHECK(std::chrono::steady_clock::now() - t0 >= 800ms);
  }
  {
    RawClient c(s.port());
    auto r = c.request("GET /api/echo HTTP/1.0\r\nHost: t\r\n\r\n");
    CHECK(r.result_int() == 200);
    CHECK(r.version() == 10);
    CHECK_FALSE(r.keep_alive());
    CHECK(RawClient::is_closed_error(c.wait_closed()));
  }
  {
    RawClient c(s.port());
    auto r = c.request(get("/api/echo", "Connection: close\r\n"));
    CHECK_FALSE(r.keep_alive());
    CHECK(RawClient::is_closed_error(c.wait_closed()));
  }
}

TEST_CASE("session: slowloris header timeout, oversized and malformed headers", "[session][server]") {
  TestServer s([](Config& c) {
    c.header_timeout_sec = 1;
    c.max_header_bytes = 2048;
  });
  add_basic_routes(s);
  s.start();
  {  // Trickling a header: cut at the header deadline even though bytes keep arriving.
    RawClient c(s.port());
    const auto t0 = std::chrono::steady_clock::now();
    c.send("GET /api/echo HTTP/1.1\r\n");
    bool closed = false;
    for (int i = 0; i < 30 && !closed; ++i) {
      try {
        c.send("X-Slow: 1\r\n");
      } catch (const std::exception&) {
        closed = true;
        break;
      }
      std::this_thread::sleep_for(100ms);
      if (std::chrono::steady_clock::now() - t0 > 1500ms) break;
    }
    if (!closed) CHECK(RawClient::is_closed_error(c.wait_closed(3000ms)));
    CHECK(std::chrono::steady_clock::now() - t0 < 3500ms);
    CHECK(std::chrono::steady_clock::now() - t0 >= 900ms);
  }
  {  // A connection that never sends anything is closed after the header timeout too.
    RawClient c(s.port());
    CHECK(RawClient::is_closed_error(c.wait_closed(3000ms)));
  }
  {
    RawClient c(s.port());
    auto r = c.request(get("/api/echo", "X-Big: " + std::string(4000, 'a') + "\r\n"));
    CHECK(r.result_int() == 431);
    CHECK(error_code_of(r) == "header_too_large");
  }
  {
    RawClient c(s.port());
    auto r = c.request("THIS IS NOT HTTP\r\n\r\n");
    CHECK(r.result_int() == 400);
  }
  {  // Absolute-form target is not origin-form → 400.
    RawClient c(s.port());
    auto r = c.request("GET http://evil/api/echo HTTP/1.1\r\nHost: t\r\n\r\n");
    CHECK(r.result_int() == 400);
  }
}

TEST_CASE("session: body idle timeout", "[session][server]") {
  TestServer s([](Config& c) { c.body_idle_timeout_sec = 1; });
  add_basic_routes(s);
  s.start();
  RawClient c(s.port());
  c.send("POST /api/items HTTP/1.1\r\nHost: t\r\nContent-Length: 100\r\n\r\n{\"a\":");
  const auto t0 = std::chrono::steady_clock::now();
  CHECK(RawClient::is_closed_error(c.wait_closed(4000ms)));
  CHECK(std::chrono::steady_clock::now() - t0 >= 800ms);
}

TEST_CASE("session: Expect: 100-continue", "[session][server]") {
  TestServer s;
  add_basic_routes(s);
  s.start();
  {
    RawClient c(s.port());
    c.send(
        "POST /api/items HTTP/1.1\r\nHost: t\r\nContent-Type: application/json\r\nContent-Length: 2\r\n"
        "Expect: 100-continue\r\n\r\n");
    auto cont = c.read();
    CHECK(cont.result_int() == 100);
    c.send("{}");
    auto r = c.read();
    CHECK(r.result_int() == 200);
    CHECK(json_of(r)["body"] == "{}");
  }
  {  // Rejected before the body: the final status comes instead of 100.
    RawClient c(s.port());
    c.send("POST /api/items HTTP/1.1\r\nHost: t\r\nContent-Length: 99999\r\nExpect: 100-continue\r\n\r\n");
    auto r = c.read();
    CHECK(r.result_int() == 413);
  }
}

TEST_CASE("session: auth modes", "[session][server]") {
  TestServer s;
  add_basic_routes(s);
  s.resolver.add("tok-user", 7, 70);
  s.resolver.add("tok-admin", 1, 10, true);
  s.start();
  RawClient c(s.port());
  auto r = c.request(get("/api/me"));
  CHECK(r.result_int() == 401);
  CHECK(error_code_of(r) == "unauthorized");
  r = c.request(get("/api/me", "Authorization: Bearer wrong\r\n"));
  CHECK(r.result_int() == 401);
  r = c.request(get("/api/me", "Authorization: Basic dG9rLXVzZXI=\r\n"));
  CHECK(r.result_int() == 401);
  r = c.request(get("/api/me", "Authorization: bearer   tok-user  \r\n"));
  REQUIRE(r.result_int() == 200);
  CHECK(json_of(r)["user_id"] == 7);
  CHECK(json_of(r)["session_id"] == 70);
  r = c.request(get("/api/admin/x", "Authorization: Bearer tok-user\r\n"));
  CHECK(r.result_int() == 403);
  CHECK(error_code_of(r) == "forbidden");
  r = c.request(get("/api/admin/x", "Authorization: Bearer tok-admin\r\n"));
  CHECK(r.result_int() == 200);
  // Public routes never consult the resolver.
  const int before = s.resolver.calls.load();
  r = c.request(get("/api/echo", "Authorization: Bearer tok-user\r\n"));
  CHECK(r.result_int() == 200);
  CHECK_FALSE(json_of(r).contains("user_id"));
  CHECK(s.resolver.calls.load() == before);
}

TEST_CASE("session: File-mode upload streams to tmp with sha256 and is cleaned up", "[session][server]") {
  TestServer s;
  struct Seen {
    std::mutex mu;
    std::optional<std::filesystem::path> file;
    bool existed = false;
    std::string sha, content;
    std::size_t size = 0;
    std::string content_type;
  } seen;
  s.add(
      V::post, "/api/attachments",
      [&](http::Ctx& ctx) {
        std::lock_guard lk(seen.mu);
        seen.file = ctx.req.body_file;
        seen.existed = ctx.req.body_file && std::filesystem::exists(*ctx.req.body_file);
        seen.sha = ctx.req.body_sha256.value_or("");
        seen.size = ctx.req.body_size;
        seen.content_type = std::string(ctx.req.header("Content-Type").value_or(""));
        if (ctx.req.body_file) {
          std::ifstream in(*ctx.req.body_file, std::ios::binary);
          seen.content.assign(std::istreambuf_iterator<char>(in), {});
        }
        if (ctx.query("fail")) throw ApiError::bad_request("invalid_field", "参数无效");
        return http::Response::json(boost::json::object{{"ok", true}}, 201);
      },
      http::AuthReq::User, http::BodyMode::File, 1u << 20, http::Exec::Files);
  s.resolver.add("tok", 3, 30);
  s.start();

  const std::string payload = [] {
    std::string p;
    for (int i = 0; i < 200000; ++i) p.push_back(static_cast<char>('a' + i % 26));
    return p;
  }();
  {
    RawClient c(s.port());
    auto r = c.request("POST /api/attachments?filename=a.txt HTTP/1.1\r\nHost: t\r\nAuthorization: Bearer tok\r\n"
                       "Content-Type: text/plain\r\nContent-Length: " +
                       std::to_string(payload.size()) + "\r\n\r\n" + payload);
    CHECK(r.result_int() == 201);
    std::lock_guard lk(seen.mu);
    REQUIRE(seen.file);
    CHECK(seen.existed);
    CHECK(seen.file->parent_path() == s.tmp_dir());
    CHECK(seen.sha == crypto::sha256_hex(payload));
    CHECK(seen.size == payload.size());
    CHECK(seen.content == payload);
    CHECK(seen.content_type == "text/plain");
  }
  // The session deleted the staged file (the handler did not consume it).
  CHECK(eventually([&] { return !std::filesystem::exists(*seen.file); }));
  CHECK(count_files(s.tmp_dir()) == 0);

  {  // Handler error: still deleted.
    RawClient c(s.port());
    auto r = c.request("POST /api/attachments?fail=1 HTTP/1.1\r\nHost: t\r\nAuthorization: Bearer tok\r\n"
                       "Content-Length: 5\r\n\r\nhello");
    CHECK(r.result_int() == 400);
    CHECK(eventually([&] { return count_files(s.tmp_dir()) == 0; }));
  }
  {  // Chunked upload.
    RawClient c(s.port());
    auto r = c.request("POST /api/attachments HTTP/1.1\r\nHost: t\r\nAuthorization: Bearer tok\r\n"
                       "Transfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n2\r\nde\r\n0\r\n\r\n");
    CHECK(r.result_int() == 201);
    std::lock_guard lk(seen.mu);
    CHECK(seen.sha == crypto::sha256_hex("abcde"));
    CHECK(seen.size == 5);
  }
  {  // Unauthenticated: rejected before anything is staged, connection closed.
    RawClient c(s.port());
    c.send("POST /api/attachments HTTP/1.1\r\nHost: t\r\nContent-Length: 100000\r\n\r\n");
    auto r = c.read();
    CHECK(r.result_int() == 401);
    CHECK_FALSE(r.keep_alive());
    CHECK(count_files(s.tmp_dir()) == 0);
  }
  {  // Over the limit (declared) → 413, nothing staged.
    RawClient c(s.port());
    c.send("POST /api/attachments HTTP/1.1\r\nHost: t\r\nAuthorization: Bearer tok\r\nContent-Length: 2000000\r\n\r\n");
    auto r = c.read();
    CHECK(r.result_int() == 413);
    CHECK(count_files(s.tmp_dir()) == 0);
  }
  {  // Over the limit (chunked, discovered while streaming) → 413, staged file removed.
    RawClient c(s.port());
    std::ostringstream req;
    req << "POST /api/attachments HTTP/1.1\r\nHost: t\r\nAuthorization: Bearer tok\r\nTransfer-Encoding: chunked\r\n\r\n";
    const std::string chunk(300000, 'z');
    for (int i = 0; i < 4; ++i) req << std::hex << chunk.size() << "\r\n" << chunk << "\r\n";
    req << "0\r\n\r\n";
    try {
      c.send(req.str());
      auto r = c.read();
      CHECK(r.result_int() == 413);
    } catch (const std::exception&) {
      // The server may close while we are still writing; the cleanup is what matters here.
    }
    CHECK(eventually([&] { return count_files(s.tmp_dir()) == 0; }));
  }
  {  // Client disconnects mid-upload → staged file removed.
    {
      RawClient c(s.port());
      c.send("POST /api/attachments HTTP/1.1\r\nHost: t\r\nAuthorization: Bearer tok\r\nContent-Length: 1000\r\n\r\n" +
             std::string(10, 'q'));
      std::this_thread::sleep_for(100ms);
    }
    CHECK(eventually([&] { return count_files(s.tmp_dir()) == 0; }));
  }
}

TEST_CASE("session: file responses, Range, remove_after_send", "[session][server]") {
  TestServer s;
  const auto file = s.ts.dir / "doc.pdf";
  std::string content;
  for (int i = 0; i < 100000; ++i) content.push_back(static_cast<char>(i % 251));
  std::ofstream(file, std::ios::binary) << content;
  const auto temp = s.ts.dir / "temp.eml";
  s.add(V::get, "/api/files/:id", [&](http::Ctx&) {
    auto r = http::Response::file(file, "application/pdf", "inline; filename=\"doc.pdf\"");
    r.add_header("Cache-Control", "private, max-age=3600");
    return r;
  });
  s.add(V::get, "/api/temp", [&](http::Ctx&) {
    std::ofstream(temp, std::ios::binary) << "Subject: hi\r\n\r\nbody";
    auto r = http::Response::file(temp, "text/plain; charset=utf-8", "");
    std::get<http::FileRef>(r.body).remove_after_send = true;
    return r;
  });
  s.add(V::get, "/api/missing", [&](http::Ctx&) { return http::Response::file(s.ts.dir / "nope", "text/plain", ""); });
  s.start();
  RawClient c(s.port());

  auto r = c.request(get("/api/files/1"));
  CHECK(r.result_int() == 200);
  CHECK(r.body() == content);
  CHECK(header(r, "Content-Type") == "application/pdf");
  CHECK(header(r, "Content-Disposition") == "inline; filename=\"doc.pdf\"");
  CHECK(header(r, "Accept-Ranges") == "bytes");
  CHECK(header(r, "Cache-Control") == "private, max-age=3600");
  CHECK(header(r, "X-Content-Type-Options") == "nosniff");
  CHECK(header(r, "Content-Length") == std::to_string(content.size()));

  r = c.request(get("/api/files/1", "Range: bytes=10-19\r\n"));
  CHECK(r.result_int() == 206);
  CHECK(r.body() == content.substr(10, 10));
  CHECK(header(r, "Content-Range") == "bytes 10-19/100000");
  CHECK(header(r, "Content-Length") == "10");

  r = c.request(get("/api/files/1", "Range: bytes=-5\r\n"));
  CHECK(r.result_int() == 206);
  CHECK(r.body() == content.substr(content.size() - 5));

  r = c.request(get("/api/files/1", "Range: bytes=99990-\r\n"));
  CHECK(r.result_int() == 206);
  CHECK(r.body() == content.substr(99990));

  r = c.request(get("/api/files/1", "Range: bytes=200000-\r\n"));
  CHECK(r.result_int() == 416);
  CHECK(header(r, "Content-Range") == "bytes */100000");
  CHECK(error_code_of(r) == "range_not_satisfiable");

  // Multi-range and If-Range fall back to the full representation.
  r = c.request(get("/api/files/1", "Range: bytes=0-1,4-5\r\n"));
  CHECK(r.result_int() == 200);
  CHECK(r.body().size() == content.size());
  r = c.request(get("/api/files/1", "Range: bytes=0-1\r\nIf-Range: \"etag\"\r\n"));
  CHECK(r.result_int() == 200);

  r = c.request(get("/api/temp"));
  CHECK(r.result_int() == 200);
  CHECK(r.body() == "Subject: hi\r\n\r\nbody");
  CHECK(eventually([&] { return !std::filesystem::exists(temp); }));

  r = c.request(get("/api/missing"));
  CHECK(r.result_int() == 500);
  CHECK(error_code_of(r) == "internal_error");
}

TEST_CASE("session: pools per route exec and the in-flight cap", "[session][server]") {
  TestServer s([](Config& c) { c.inflight_cap = 1; });
  std::atomic<int> on_db{0}, on_net{0}, on_files{0};
  std::mutex mu;
  std::condition_variable cv;
  bool release = false;
  std::atomic<bool> in_slow{false};
  auto where = [&](http::Ctx& ctx) {
    if (s.db_pool->get_executor().running_in_this_thread()) ++on_db;
    if (s.net_pool->get_executor().running_in_this_thread()) ++on_net;
    if (s.files_pool->get_executor().running_in_this_thread()) ++on_files;
    if (ctx.query("slow")) {
      in_slow = true;
      std::unique_lock lk(mu);
      cv.wait_for(lk, 5s, [&] { return release; });
    }
    return http::Response::no_content();
  };
  s.add(V::get, "/api/db", where);
  s.add(V::get, "/api/net", where, http::AuthReq::None, http::BodyMode::None, 0, http::Exec::Net);
  s.add(V::get, "/api/files", where, http::AuthReq::None, http::BodyMode::None, 0, http::Exec::Files);
  s.start();
  {
    RawClient c(s.port());
    CHECK(c.request(get("/api/db")).result_int() == 204);
    CHECK(c.request(get("/api/net")).result_int() == 204);
    CHECK(c.request(get("/api/files")).result_int() == 204);
    CHECK(on_db == 1);
    CHECK(on_net == 1);
    CHECK(on_files == 1);
  }
  RawClient slow(s.port());
  slow.send(get("/api/db?slow=1"));
  REQUIRE(eventually([&] { return in_slow.load(); }));
  CHECK(s.server->inflight() == 1);
  {
    RawClient c(s.port());
    auto r = c.request(get("/api/net"));
    CHECK(r.result_int() == 503);
    CHECK(error_code_of(r) == "service_unavailable");
  }
  {
    std::lock_guard lk(mu);
    release = true;
  }
  cv.notify_all();
  CHECK(slow.read().result_int() == 204);
  CHECK(eventually([&] { return s.server->inflight() == 0; }));
  RawClient c(s.port());
  CHECK(c.request(get("/api/net")).result_int() == 204);
}

TEST_CASE("session: X-Forwarded-For only from trusted proxies", "[session][server]") {
  {
    TestServer s;  // default trusted proxies: 127.0.0.1, ::1
    add_basic_routes(s);
    s.start();
    RawClient c(s.port());
    auto r = c.request(get("/api/echo", "X-Forwarded-For: 198.51.100.1, 203.0.113.9\r\n"));
    CHECK(json_of(r)["ip"] == "203.0.113.9");
  }
  {
    TestServer s([](Config& c) { c.trusted_proxies.clear(); });
    add_basic_routes(s);
    s.start();
    RawClient c(s.port());
    auto r = c.request(get("/api/echo", "X-Forwarded-For: 203.0.113.9\r\n"));
    CHECK(json_of(r)["ip"] == "127.0.0.1");
  }
}

TEST_CASE("server: connection cap and graceful stop", "[session][server]") {
  TestServer s([](Config& c) { c.max_connections = 2; });
  add_basic_routes(s);
  s.start();
  RawClient a(s.port());
  RawClient b(s.port());
  CHECK(a.request(get("/api/echo")).result_int() == 200);
  CHECK(b.request(get("/api/echo")).result_int() == 200);
  CHECK(eventually([&] { return s.server->connections() == 2; }));
  RawClient third(s.port());  // accepted by the kernel, closed by the server
  CHECK(RawClient::is_closed_error(third.wait_closed(3000ms)));
  CHECK(s.server->connections() == 2);

  // stop(): idle keep-alive connections are closed, new connections refused.
  s.server->stop();
  CHECK(RawClient::is_closed_error(a.wait_closed(3000ms)));
  CHECK(RawClient::is_closed_error(b.wait_closed(3000ms)));
  CHECK(eventually([&] { return s.server->connections() == 0; }));
  bool refused = false;
  try {
    RawClient late(s.port());
    late.send(get("/api/echo"));
    late.read(2s);
  } catch (const std::exception&) {
    refused = true;
  }
  CHECK(refused);
  s.server->stop();  // idempotent
}

TEST_CASE("server: a request in flight during stop still gets its response", "[session][server]") {
  TestServer s;
  std::atomic<bool> entered{false};
  std::atomic<bool> go{false};
  s.add(V::get, "/api/slow", [&](http::Ctx&) {
    entered = true;
    eventually([&] { return go.load(); });
    return http::Response::json(boost::json::object{{"done", true}});
  });
  s.start();
  RawClient c(s.port());
  c.send(get("/api/slow"));
  REQUIRE(eventually([&] { return entered.load(); }));
  s.server->stop();
  go = true;
  auto r = c.read();
  CHECK(r.result_int() == 200);
  CHECK_FALSE(r.keep_alive());  // keep-alive is off once stopping
  CHECK(RawClient::is_closed_error(c.wait_closed()));
}

TEST_CASE("server: start errors", "[session][server]") {
  TestServer s([](Config& c) { c.listen_address = "not-an-ip"; });
  add_basic_routes(s);
  CHECK_THROWS_AS(s.start(), std::system_error);
}

// ---- SEC-6: authentication before any JSON body is buffered ------------------------------------

TEST_CASE("session: authenticated JSON routes reject anonymous clients before reading the body (SEC-6)",
          "[session][server][sec]") {
  TestServer s;
  std::atomic<int> handled{0};
  s.add(V::put, "/api/drafts/:id", [&](http::Ctx& ctx) {
    ++handled;
    return echo(ctx);
  }, http::AuthReq::User, http::BodyMode::Json, 8u << 20);
  s.add(V::post, "/api/admin/thing", echo, http::AuthReq::Admin, http::BodyMode::Json, 1u << 20);
  s.resolver.add("tok-user", 7, 70);
  s.start();
  const std::string big_head = "PUT /api/drafts/1 HTTP/1.1\r\nHost: t\r\nContent-Type: application/json\r\n"
                               "Content-Length: " + std::to_string((8u << 20) - 1) + "\r\n";
  {
    // No token: 401 right after the header, without waiting for (or buffering) 8 MiB.
    RawClient c(s.port());
    c.send(big_head + "\r\n" + std::string(1000, ' '));
    auto r = c.read(3s);
    CHECK(r.result_int() == 401);
    CHECK(error_code_of(r) == "unauthorized");
    CHECK_FALSE(r.keep_alive());
    CHECK(RawClient::is_closed_error(c.wait_closed(3000ms)));
  }
  {
    RawClient c(s.port());
    c.send(big_head + "Authorization: Bearer wrong\r\n\r\n");
    CHECK(c.read(3s).result_int() == 401);
  }
  {
    // Admin route with a user token: 403 before the body.
    RawClient c(s.port());
    c.send("POST /api/admin/thing HTTP/1.1\r\nHost: t\r\nAuthorization: Bearer tok-user\r\nContent-Type: "
           "application/json\r\nContent-Length: 500000\r\n\r\n{");
    auto r = c.read(3s);
    CHECK(r.result_int() == 403);
    CHECK(error_code_of(r) == "forbidden");
  }
  CHECK(handled == 0);
  // With a valid token the body is read and the handler runs as before.
  RawClient ok(s.port());
  auto r = ok.request(
      "PUT /api/drafts/1 HTTP/1.1\r\nHost: t\r\nAuthorization: Bearer tok-user\r\nContent-Type: application/json\r\n"
      "Content-Length: 11\r\n\r\n{\"a\":\"bcd\"}");
  REQUIRE(r.result_int() == 200);
  CHECK(json_of(r)["user_id"] == 7);
  CHECK(json_of(r)["body"] == "{\"a\":\"bcd\"}");
  CHECK(handled == 1);
}

// ---- RT-5: the end of the shutdown drain closes busy connections too ------------------------------

TEST_CASE("server: close_all closes connections in the middle of a request (RT-5)", "[session][server]") {
  TestServer s([](Config& c) {
    c.header_timeout_sec = 60;
    c.body_idle_timeout_sec = 60;
  });
  add_basic_routes(s);
  s.start();
  RawClient idle(s.port());
  CHECK(idle.request(get("/api/echo")).result_int() == 200);
  RawClient header(s.port());
  header.send("GET /api/echo HTTP/1.1\r\nHost: t\r\n");  // header never finished
  RawClient body(s.port());
  body.send("POST /api/items HTTP/1.1\r\nHost: t\r\nContent-Type: application/json\r\nContent-Length: 500\r\n\r\n{");
  REQUIRE(eventually([&] { return s.server->connections() == 3; }));
  std::this_thread::sleep_for(300ms);  // the server has read what was sent: two requests in progress
  s.server->stop();  // closes only the idle one
  CHECK(RawClient::is_closed_error(idle.wait_closed(3000ms)));
  std::this_thread::sleep_for(200ms);
  CHECK(s.server->connections() == 2);  // busy connections survive stop()
  const auto t0 = std::chrono::steady_clock::now();
  s.server->close_all();
  CHECK(RawClient::is_closed_error(header.wait_closed(3000ms)));
  CHECK(RawClient::is_closed_error(body.wait_closed(3000ms)));
  CHECK(eventually([&] { return s.server->connections() == 0; }));
  CHECK(std::chrono::steady_clock::now() - t0 < 2s);  // not the 60 s timeouts
}
