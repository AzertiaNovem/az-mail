// Owner: WP-A — in-process HTTP/WS server on an ephemeral port + blocking test clients.
// Used by test_session_limits.cpp and test_ws_auth.cpp. The session resolver is a fake
// (Services::session_resolver), so nothing here depends on WP-D's repo.
#pragma once

#include "http/dispatch.hpp"
#include "http/router.hpp"
#include "http/server.hpp"
#include "services.hpp"
#include "test_support.hpp"
#include "ws/hub.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>

#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <variant>
#include <vector>

namespace azm::test {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace bhttp = boost::beast::http;
namespace websocket = boost::beast::websocket;
using tcp = asio::ip::tcp;
using namespace std::chrono_literals;

inline constexpr std::string_view kOrigin = "http://localhost:5173";  // in Config's default allowlist

// Token → principal map standing in for repo::find_session.
struct FakeResolver final : http::SessionResolver {
  std::mutex mu;
  std::map<std::string, http::Principal, std::less<>> tokens;
  std::atomic<int> calls{0};

  std::optional<http::Principal> resolve(Services&, std::string_view token) override {
    ++calls;
    std::lock_guard lk(mu);
    auto it = tokens.find(token);
    if (it == tokens.end()) return std::nullopt;
    return it->second;
  }
  void add(std::string token, int64_t user_id, int64_t session_id, bool admin = false) {
    std::lock_guard lk(mu);
    tokens[std::move(token)] = http::Principal{user_id, session_id, admin, "u" + std::to_string(user_id) + "@x.cn"};
  }
  void remove(std::string_view token) {
    std::lock_guard lk(mu);
    if (auto it = tokens.find(token); it != tokens.end()) tokens.erase(it);
  }
};

// Polls `pred` until true or the timeout passes.
inline bool eventually(const std::function<bool()>& pred, std::chrono::milliseconds timeout = 5000ms) {
  const auto end = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < end) {
    if (pred()) return true;
    std::this_thread::sleep_for(10ms);
  }
  return pred();
}

class TestServer {
 public:
  explicit TestServer(std::function<void(Config&)> tweak = {}) {
    ts.cfg.listen_address = "127.0.0.1";
    ts.cfg.listen_port = 0;
    if (tweak) tweak(ts.cfg);
    ts.svc.session_resolver = &resolver;
  }
  TestServer(const TestServer&) = delete;
  TestServer& operator=(const TestServer&) = delete;

  // Starts listening; add routes first.
  void start(std::chrono::milliseconds reauth = std::chrono::minutes(5)) {
    db_pool = std::make_unique<asio::thread_pool>(2);
    net_pool = std::make_unique<asio::thread_pool>(1);
    files_pool = std::make_unique<asio::thread_pool>(1);
    ts.svc.db_workers = db_pool.get();
    ts.svc.net_workers = net_pool.get();
    ts.svc.files_workers = files_pool.get();
    server = std::make_unique<http::Server>(
        ioc, http::ServerDeps{ts.cfg, router, ts.svc, *db_pool, *net_pool, hub, files_pool.get(), reauth});
    server->start();
    work.emplace(asio::make_work_guard(ioc));
    for (int i = 0; i < 2; ++i) threads.emplace_back([this] { ioc.run(); });
  }

  ~TestServer() {
    if (server) {
      server->stop();
      hub.close_all();
      eventually([&] { return server->connections() == 0; }, 7000ms);
      db_pool->join();
      net_pool->join();
      files_pool->join();
    }
    work.reset();
    ioc.stop();
    for (auto& t : threads) t.join();
    server.reset();
  }

  uint16_t port() const { return server->port(); }
  std::filesystem::path tmp_dir() const { return ts.blobs->tmp_dir(); }

  void add(boost::beast::http::verb m, std::string pattern, http::Handler h, http::AuthReq auth = http::AuthReq::None,
           http::BodyMode body = http::BodyMode::None, std::size_t limit = 0, http::Exec exec = http::Exec::Db) {
    router.add(http::Route{m, std::move(pattern), auth, body, limit, exec, std::move(h)});
  }

  TestServices ts;
  FakeResolver resolver;
  http::Router router;
  ws::Hub hub{8};
  std::unique_ptr<asio::thread_pool> db_pool, net_pool, files_pool;
  asio::io_context ioc{2};
  std::optional<asio::executor_work_guard<asio::io_context::executor_type>> work;
  std::unique_ptr<http::Server> server;
  std::vector<std::thread> threads;
};

// Blocking HTTP client over a private io_context; every operation has a deadline, so a server
// bug fails the test instead of hanging it.
class RawClient {
 public:
  explicit RawClient(uint16_t port) : stream_(io_) {
    beast::error_code ec;
    stream_.expires_after(5s);
    stream_.async_connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port),
                          [&](beast::error_code e) { ec = e; });
    run();
    if (ec) throw beast::system_error(ec);
  }

  void send(std::string_view data) {
    beast::error_code ec;
    stream_.expires_after(10s);
    asio::async_write(stream_, asio::buffer(data), [&](beast::error_code e, std::size_t) { ec = e; });
    run();
    if (ec) throw beast::system_error(ec);
  }

  // Reads one response (a 1xx counts as one). Throws on timeout / connection errors.
  bhttp::response<bhttp::string_body> read(std::chrono::seconds timeout = 5s) {
    bhttp::response_parser<bhttp::string_body> p;
    p.body_limit(64u << 20);
    p.header_limit(64u << 10);
    beast::error_code ec;
    stream_.expires_after(timeout);
    bhttp::async_read(stream_, buf_, p, [&](beast::error_code e, std::size_t) { ec = e; });
    run();
    if (ec) throw beast::system_error(ec);
    return p.release();
  }

  bhttp::response<bhttp::string_body> request(std::string_view raw) {
    send(raw);
    return read();
  }

  // Waits until the peer closes. Returns the error that ended the read (eof / reset) or
  // beast::error::timeout when the connection stayed open for `timeout`.
  beast::error_code wait_closed(std::chrono::milliseconds timeout = 5000ms) {
    beast::error_code ec;
    stream_.expires_after(timeout);
    for (;;) {
      std::array<char, 4096> b{};
      bool done = false;
      std::size_t got = 0;
      stream_.async_read_some(asio::buffer(b), [&](beast::error_code e, std::size_t n) {
        ec = e;
        got = n;
        done = true;
      });
      run();
      if (!done || ec) return ec;
      (void)got;
    }
  }

  static bool is_closed_error(const beast::error_code& ec) {
    return ec == asio::error::eof || ec == asio::error::connection_reset || ec == asio::error::broken_pipe ||
           ec == bhttp::error::end_of_stream;
  }

  beast::tcp_stream& stream() { return stream_; }

 private:
  void run() {
    io_.restart();
    io_.run();
  }
  asio::io_context io_;
  beast::tcp_stream stream_;
  beast::flat_buffer buf_;
};

// Blocking WebSocket client (Beast) with per-operation deadlines.
class WsClient {
 public:
  WsClient() : ws_(io_) {}

  // TCP connect + handshake. Returns the handshake error (e.g. upgrade_declined); the HTTP
  // response of a declined upgrade is in `response`.
  beast::error_code connect(uint16_t port, std::optional<std::string> origin = std::string(kOrigin),
                            std::string target = "/api/ws") {
    beast::error_code ec;
    auto& low = beast::get_lowest_layer(ws_);
    low.expires_after(5s);
    low.async_connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port), [&](beast::error_code e) { ec = e; });
    run();
    if (ec) return ec;
    ws_.set_option(websocket::stream_base::decorator([origin](websocket::request_type& req) {
      if (origin) req.set(bhttp::field::origin, *origin);
    }));
    low.expires_after(5s);
    ws_.async_handshake(response, "127.0.0.1:" + std::to_string(port), target, [&](beast::error_code e) { ec = e; });
    run();
    return ec;
  }

  beast::error_code send(std::string_view text) {
    beast::error_code ec;
    beast::get_lowest_layer(ws_).expires_after(5s);
    ws_.text(true);
    ws_.async_write(asio::buffer(text), [&](beast::error_code e, std::size_t) { ec = e; });
    run();
    return ec;
  }

  // Next text message, or the error that ended the read (websocket::error::closed after a
  // close frame — see close_code()).
  std::variant<std::string, beast::error_code> read(std::chrono::milliseconds timeout = 5000ms) {
    beast::error_code ec;
    buf_.clear();
    beast::get_lowest_layer(ws_).expires_after(timeout);
    ws_.async_read(buf_, [&](beast::error_code e, std::size_t) { ec = e; });
    run();
    if (ec) return ec;
    return beast::buffers_to_string(buf_.data());
  }

  // Reads until a message of `type` arrives (skipping others) or the socket ends.
  std::optional<std::string> read_type(std::string_view type, std::chrono::milliseconds timeout = 5000ms) {
    const auto end = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < end) {
      auto r = read(std::chrono::duration_cast<std::chrono::milliseconds>(end - std::chrono::steady_clock::now()));
      if (auto* s = std::get_if<std::string>(&r)) {
        if (s->find("\"type\":\"" + std::string(type) + "\"") != std::string::npos) return *s;
        continue;
      }
      return std::nullopt;
    }
    return std::nullopt;
  }

  // Reads until the server closes; returns the close code (0 when the TCP connection just died).
  std::uint16_t wait_close(std::chrono::milliseconds timeout = 5000ms) {
    const auto end = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < end) {
      auto r = read(std::chrono::duration_cast<std::chrono::milliseconds>(end - std::chrono::steady_clock::now()));
      if (std::holds_alternative<std::string>(r)) continue;
      const auto ec = std::get<beast::error_code>(r);
      if (ec == websocket::error::closed) return ws_.reason().code;
      return 0;
    }
    return 0;
  }

  websocket::response_type response;
  websocket::stream<beast::tcp_stream>& ws() { return ws_; }

 private:
  void run() {
    io_.restart();
    io_.run();
  }
  asio::io_context io_;
  websocket::stream<beast::tcp_stream> ws_;
  beast::flat_buffer buf_;
};

}  // namespace azm::test
