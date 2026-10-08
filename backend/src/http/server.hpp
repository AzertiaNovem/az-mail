// Owner: WP-A
//
// HTTP listener: binds cfg.listen_address:cfg.listen_port, accepts connections and spawns one
// http::run_session coroutine per connection on its own strand (DESIGN A1). Enforces
// cfg.max_connections (excess connections are closed immediately) and shares the in-flight
// request counter (cfg.inflight_cap → 503 "service_unavailable").
#pragma once

#include <boost/asio/io_context.hpp>
#include <boost/asio/thread_pool.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace azm {
struct Config;
struct Services;
}  // namespace azm
namespace azm::ws {
class Hub;
}

namespace azm::http {

class Router;

// Everything a connection needs. All references outlive the Server.
struct ServerDeps {
  const Config& cfg;
  const Router& router;                 // fully populated before start()
  Services& svc;
  boost::asio::thread_pool& db_pool;    // Exec::Db handlers (+ WS auth, File-route pre-auth)
  boost::asio::thread_pool& net_pool;   // Exec::Net handlers (synchronous Resend calls)
  ws::Hub& hub;                         // WebSocket registry (/api/ws upgrades)
  // Exec::Files handlers (R2/disk file I/O, cfg.files_threads). A pointer so existing aggregate
  // initializers stay valid; App always sets it; nullptr → Exec::Files runs on net_pool.
  boost::asio::thread_pool* files_pool = nullptr;
  // Additive (WP-A): WebSocket re-authentication period (ws::WsDeps::reauth_interval, DESIGN
  // step 5b of ws_session.hpp); tests shorten it.
  std::chrono::milliseconds ws_reauth_interval{std::chrono::minutes(5)};
};

class Server {
 public:
  Server(boost::asio::io_context& ioc, ServerDeps deps);
  ~Server();
  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  // Binds and listens (port 0 = ephemeral, see port()) and starts the accept loop on `ioc`.
  // Throws std::system_error when the address cannot be bound.
  void start();
  // Closes the acceptor. Open connections finish their in-flight request and then close
  // (keep-alive is disabled once stopping). Idempotent; thread-safe.
  void stop();
  // Additive (RT-5): closes every open connection now, busy ones included (end of the shutdown
  // drain; WebSockets are closed by Hub::close_all). Implies stop(). Thread-safe.
  void close_all();

  uint16_t port() const;           // actual bound port (valid after start())
  std::size_t connections() const;  // currently open connections
  std::size_t inflight() const;     // requests currently dispatched to a blocking pool

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace azm::http
