// Smoke tests for the build configuration owned by WP0: Asio/Beast separate compilation
// (core/asio_impl.cpp), C++20 coroutines, the run_blocking hand-off pattern (DESIGN A1) and
// OpenSSL/Asio SSL wiring. Loopback only; no external network.
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>

#include <catch2/catch_test_macros.hpp>

#include <openssl/ssl.h>

#include <thread>

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;

TEST_CASE("Asio/Beast: loopback HTTP round trip with coroutines", "[asio]") {
  asio::io_context ioc;
  tcp::acceptor acceptor(ioc, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
  const auto port = acceptor.local_endpoint().port();

  std::string server_target, server_body, client_body;
  unsigned client_status = 0;
  std::exception_ptr server_err, client_err;

  asio::co_spawn(
      ioc,
      [&]() -> asio::awaitable<void> {
        tcp::socket sock = co_await acceptor.async_accept(asio::use_awaitable);
        beast::tcp_stream stream(std::move(sock));
        stream.expires_after(std::chrono::seconds(5));
        beast::flat_buffer buf;
        http::request<http::string_body> req;
        co_await http::async_read(stream, buf, req, asio::use_awaitable);
        server_target = std::string(req.target());
        server_body = req.body();
        http::response<http::string_body> res{http::status::ok, req.version()};
        res.set(http::field::content_type, "text/plain; charset=utf-8");
        res.body() = "pong:" + req.body();
        res.prepare_payload();
        co_await http::async_write(stream, res, asio::use_awaitable);
        beast::error_code ec;
        stream.socket().shutdown(tcp::socket::shutdown_send, ec);
      },
      [&](std::exception_ptr e) { server_err = e; });

  asio::co_spawn(
      ioc,
      [&]() -> asio::awaitable<void> {
        beast::tcp_stream stream(co_await asio::this_coro::executor);
        stream.expires_after(std::chrono::seconds(5));
        co_await stream.async_connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port),
                                      asio::use_awaitable);
        http::request<http::string_body> req{http::verb::post, "/api/health?x=1", 11};
        req.set(http::field::host, "127.0.0.1");
        req.set(http::field::user_agent, "azmail-test");
        req.body() = "ping 中文";
        req.prepare_payload();
        co_await http::async_write(stream, req, asio::use_awaitable);
        beast::flat_buffer buf;
        http::response<http::string_body> res;
        co_await http::async_read(stream, buf, res, asio::use_awaitable);
        client_status = res.result_int();
        client_body = res.body();
      },
      [&](std::exception_ptr e) { client_err = e; });

  ioc.run_for(std::chrono::seconds(10));
  CHECK_FALSE(server_err);
  CHECK_FALSE(client_err);
  CHECK(server_target == "/api/health?x=1");
  CHECK(server_body == "ping 中文");
  CHECK(client_status == 200);
  CHECK(client_body == "pong:ping 中文");
}

TEST_CASE("Asio: co_spawn onto a thread_pool resumes on the caller's executor", "[asio]") {
  asio::thread_pool pool(2);
  asio::io_context ioc;
  const auto main_id = std::this_thread::get_id();
  std::thread::id worker_id, resumed_id;
  int result = 0;
  bool caught = false;

  asio::co_spawn(
      ioc,
      [&]() -> asio::awaitable<void> {
        result = co_await asio::co_spawn(
            pool,
            [&]() -> asio::awaitable<int> {
              worker_id = std::this_thread::get_id();
              co_return 42;
            },
            asio::use_awaitable);
        resumed_id = std::this_thread::get_id();
        try {
          co_await asio::co_spawn(
              pool,
              [fail = true]() -> asio::awaitable<int> {
                if (fail) throw std::runtime_error("from pool");
                co_return 0;
              },
              asio::use_awaitable);
        } catch (const std::runtime_error&) {
          caught = true;
        }
      },
      asio::detached);
  ioc.run();
  pool.join();

  CHECK(result == 42);
  CHECK(worker_id != main_id);
  CHECK(resumed_id == main_id);
  CHECK(caught);
}

TEST_CASE("Asio SSL + Beast WebSocket types instantiate (separate compilation links)", "[asio]") {
  asio::io_context ioc;
  asio::ssl::context ctx(asio::ssl::context::tls_client);
  ctx.set_verify_mode(asio::ssl::verify_peer);
  CHECK_NOTHROW(ctx.set_default_verify_paths());
  asio::ssl::stream<beast::tcp_stream> tls(ioc, ctx);
  tls.set_verify_callback(asio::ssl::host_name_verification("api.resend.com"));
  CHECK(SSL_set_tlsext_host_name(tls.native_handle(), "api.resend.com") == 1);

  beast::websocket::stream<beast::tcp_stream> ws(ioc);
  ws.set_option(beast::websocket::stream_base::timeout::suggested(beast::role_type::server));
  ws.read_message_max(16 * 1024);
  CHECK(ws.read_message_max() == 16 * 1024);

  // Header-first parsing with explicit limits (DESIGN A2) compiles and links.
  http::request_parser<http::empty_body> hp;
  hp.header_limit(16 * 1024);
  beast::error_code ec;
  const std::string raw = "GET /api/ws HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\n\r\n";
  hp.put(asio::buffer(raw), ec);
  CHECK_FALSE(ec);
  CHECK(hp.is_header_done());
  CHECK(beast::websocket::is_upgrade(hp.get()) == false);  // missing Connection/Sec-* headers
}
