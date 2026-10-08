// Owner: WP-A
// HTTP listener (see server.hpp): accept loop on its own strand, one strand per connection,
// connection cap, graceful stop.
//
// Lifetime: the acceptor and the SessionShared live in a State held by shared_ptr. The accept
// coroutine and every connection coroutine keep it alive, so destroying the Server while the
// io_context still holds suspended frames (or before it ran them) never leaves a dangling
// reference; the frames release it when they finish or when the io_context is destroyed.
#include "http/server.hpp"

#include "config.hpp"
#include "core/log.hpp"
#include "http/session.hpp"

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <atomic>
#include <system_error>

namespace azm::http {

namespace asio = boost::asio;
using tcp = asio::ip::tcp;

namespace {

struct State {
  asio::io_context& ioc;
  std::shared_ptr<SessionShared> shared;
  asio::strand<asio::io_context::executor_type> strand;
  tcp::acceptor acceptor;

  State(asio::io_context& io, const ServerDeps& deps)
      : ioc(io),
        shared(new SessionShared{deps, cors_policy_from(deps.cfg), {0}, {0}, {false}, {}, {}, 0}),
        strand(asio::make_strand(io)),
        acceptor(strand) {}

  void close_acceptor() {
    boost::system::error_code ignored;
    if (acceptor.is_open()) acceptor.close(ignored);
  }
};

asio::awaitable<void> session_entry(tcp::socket socket, std::shared_ptr<SessionShared> shared) {
  co_await run_session(std::move(socket), *shared);  // never throws
}

asio::awaitable<void> accept_loop(std::shared_ptr<State> st) {
  const auto max_conn = std::max<std::size_t>(st->shared->deps.cfg.max_connections, 1);
  asio::steady_timer backoff(st->strand);
  for (;;) {
    auto [ec, socket] =
        co_await st->acceptor.async_accept(asio::make_strand(st->ioc), asio::as_tuple(asio::use_awaitable));
    if (ec) {
      if (ec == asio::error::operation_aborted || !st->acceptor.is_open() || st->shared->stopping) break;
      // EMFILE / ENFILE etc.: back off briefly instead of spinning.
      log::warn("accept failed", {{"error", ec.message()}});
      backoff.expires_after(std::chrono::milliseconds(100));
      co_await backoff.async_wait(asio::as_tuple(asio::use_awaitable));
      continue;
    }
    boost::system::error_code ignored;
    if (st->shared->stopping) {
      socket.close(ignored);
      break;
    }
    if (st->shared->connections.load() >= max_conn) {
      log::warn("connection limit reached, closing new connection", {{"limit", max_conn}});
      socket.close(ignored);
      continue;
    }
    // Disable Nagle: responses are written in one or two chunks and latency matters more.
    socket.set_option(tcp::no_delay(true), ignored);
    try {
      auto ex = socket.get_executor();
      asio::co_spawn(ex, session_entry(std::move(socket), st->shared), asio::detached);
    } catch (const std::exception& e) {  // bad_alloc: drop this connection, keep accepting
      log::error("cannot start connection", {{"error", e.what()}});
    }
  }
}

}  // namespace

struct Server::Impl {
  std::shared_ptr<State> st;
  std::atomic<uint16_t> port{0};
  std::atomic<bool> started{false};
  std::atomic<bool> stopped{false};
};

Server::Server(asio::io_context& ioc, ServerDeps deps) : impl_(std::make_unique<Impl>()) {
  impl_->st = std::make_shared<State>(ioc, deps);
}

Server::~Server() { stop(); }

void Server::start() {
  if (impl_->started.exchange(true)) throw std::logic_error("http::Server::start called twice");
  State& st = *impl_->st;
  const Config& cfg = st.shared->deps.cfg;
  boost::system::error_code ec;
  const auto addr = asio::ip::make_address(cfg.listen_address, ec);
  if (ec) throw std::system_error(ec, "invalid listen address '" + cfg.listen_address + "'");
  const tcp::endpoint ep(addr, cfg.listen_port);
  st.acceptor.open(ep.protocol(), ec);
  if (ec) throw std::system_error(ec, "cannot open listening socket");
  st.acceptor.set_option(asio::socket_base::reuse_address(true), ec);
  if (ec) throw std::system_error(ec, "cannot set SO_REUSEADDR");
  st.acceptor.bind(ep, ec);
  if (ec) {
    st.close_acceptor();
    throw std::system_error(ec, "cannot bind " + cfg.listen_address + ":" + std::to_string(cfg.listen_port));
  }
  st.acceptor.listen(asio::socket_base::max_listen_connections, ec);
  if (ec) {
    st.close_acceptor();
    throw std::system_error(ec, "cannot listen on " + cfg.listen_address);
  }
  impl_->port = st.acceptor.local_endpoint().port();
  asio::co_spawn(st.strand, accept_loop(impl_->st), asio::detached);
}

void Server::stop() {
  if (impl_->stopped.exchange(true)) return;
  auto st = impl_->st;
  st->shared->stopping = true;
  // The acceptor is not thread-safe: close it on its strand. Then drop idle keep-alive
  // connections; busy ones finish their current request and close.
  asio::post(st->strand, [st] { st->close_acceptor(); });
  close_idle_connections(*st->shared);
}

void Server::close_all() {
  stop();
  close_all_connections(*impl_->st->shared);
}

uint16_t Server::port() const { return impl_->port.load(); }
std::size_t Server::connections() const { return impl_->st->shared->connections.load(); }
std::size_t Server::inflight() const { return impl_->st->shared->inflight.load(); }

}  // namespace azm::http
