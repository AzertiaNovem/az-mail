// Owner: WP-A
// WebSocket session for GET /api/ws (DESIGN A4; step list in ws_session.hpp).
//
// Concurrency model: everything runs on the connection strand. One coroutine reads
// (run_ws_session → WsConn::serve); outgoing frames and the close frame go through a single
// write pump (at most one write op at a time, as Beast requires); timers (auth deadline, hard
// close, re-authentication) are strand-bound. Other threads (Hub::publish from DB threads) only
// post onto the strand via WsSink::send / close.
#include "ws/ws_session.hpp"

#include "config.hpp"
#include "core/errors.hpp"
#include "core/log.hpp"
#include "http/blocking.hpp"
#include "http/dispatch.hpp"
#include "services.hpp"
#include "ws/events.hpp"
#include "ws/hub.hpp"

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core/buffers_to_string.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http/string_body.hpp>
#include <boost/beast/http/write.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>

#include <deque>
#include <memory>

namespace azm::ws {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace bhttp = boost::beast::http;
namespace websocket = boost::beast::websocket;

namespace {

constexpr auto kTuple = asio::as_tuple(asio::use_awaitable);
// After a close was requested, the socket is torn down if the close handshake has not finished
// by then (stalled peer, full send buffer behind an overflowed queue).
constexpr auto kHardCloseAfter = std::chrono::seconds(5);

std::shared_ptr<const std::string> frame_of(std::string_view type, const boost::json::object& data = {}) {
  return std::make_shared<const std::string>(make_frame(type, data));
}

class WsConn final : public WsSink, public std::enable_shared_from_this<WsConn> {
 public:
  using Stream = websocket::stream<beast::tcp_stream>;

  WsConn(beast::tcp_stream&& s, std::size_t cap)
      : ws_(std::move(s)),
        cap_(std::max<std::size_t>(cap, 1)),
        auth_timer_(ws_.get_executor()),
        close_timer_(ws_.get_executor()),
        reauth_timer_(ws_.get_executor()) {}

  Stream& ws() { return ws_; }

  // ---- WsSink (any thread) --------------------------------------------------------------------
  void send(std::shared_ptr<const std::string> frame) override {
    asio::post(ws_.get_executor(),
               [self = shared_from_this(), f = std::move(frame)]() mutable { self->enqueue(std::move(f)); });
  }
  void close(std::uint16_t code, std::string_view reason) override {
    asio::post(ws_.get_executor(), [self = shared_from_this(), code, r = std::string(reason)] {
      self->request_close(code, r);
    });
  }

  // ---- strand only -----------------------------------------------------------------------------
  void enqueue(std::shared_ptr<const std::string> f) {
    if (finished_ || close_req_) return;
    if (queue_.size() >= cap_) {
      // The client is not keeping up: drop everything and close; it resyncs on reconnect.
      queue_.clear();
      log::warn("ws send queue overflow, closing", {{"user_id", user_id_}, {"cap", cap_}});
      request_close(kClosePolicy, "send queue overflow");
      return;
    }
    queue_.push_back(std::move(f));
    pump();
  }

  void request_close(std::uint16_t code, const std::string& reason) {
    if (finished_ || close_req_) return;
    close_req_.emplace();
    close_req_->code = code;  // application codes (4401) are not in beast's close_code enum
    close_req_->reason = reason.substr(0, 120);  // control frame payload ≤ 125 bytes
    close_timer_.expires_after(kHardCloseAfter);
    close_timer_.async_wait([self = shared_from_this()](const boost::system::error_code& ec) {
      if (!ec) self->abort();
    });
    pump();
  }

  void arm_auth_deadline(std::chrono::milliseconds ms) {
    auth_timer_.expires_after(ms);
    auth_timer_.async_wait([self = shared_from_this()](const boost::system::error_code& ec) {
      if (!ec && !self->authed_) self->request_close(kCloseAuthFailed, "auth timeout");
    });
  }

  // Read loop: first-message auth, then pings. Returns when the socket is done; always leaves
  // the Hub and ends the session, whatever happened.
  asio::awaitable<void> serve(WsDeps& deps) {
    uint64_t registration = 0;
    try {
      co_await serve_loop(deps, registration);
    } catch (const std::exception& e) {
      log::warn("ws session failed", {{"error", e.what()}});
    } catch (...) {
      log::warn("ws session failed");
    }
    if (registration != 0) deps.hub.detach(registration);
    finish();
  }

  asio::awaitable<void> serve_loop(WsDeps& deps, uint64_t& registration) {
    beast::flat_buffer buf;
    for (;;) {
      buf.clear();
      auto [ec, n] = co_await ws_.async_read(buf, kTuple);
      if (ec) break;  // closed by either side, timeout, message too big (Beast sent 1009), …
      if (!authed_) {
        if (close_req_) continue;  // closing: keep reading until the peer answers our close
        ClientMessage msg;
        if (ws_.got_text()) msg = parse_client_message(beast::buffers_to_string(buf.data()));
        if (msg.type != ClientMessageType::Auth || msg.token.empty()) {
          request_close(kCloseAuthFailed, "auth required");
          continue;
        }
        std::optional<http::Principal> who;
        try {
          Services& svc = deps.svc;
          const std::string& token = msg.token;
          who = co_await http::run_blocking(deps.db_pool, [&svc, &token] { return http::authenticate(svc, token); });
        } catch (const std::exception& e) {
          log::warn("ws authentication error", {{"error", e.what()}});
          request_close(kCloseInternal, "internal error");
          continue;
        }
        if (close_req_ || finished_) continue;  // the deadline fired during the lookup
        if (!who) {
          log::info("ws auth failed");
          request_close(kCloseAuthFailed, "auth failed");
          continue;
        }
        authed_ = true;
        user_id_ = who->user_id;
        auth_timer_.cancel();
        registration = deps.hub.attach(who->user_id, who->session_id, shared_from_this());
        if (registration == 0) continue;  // shutting down: the Hub already asked us to close
        // Queued synchronously, so it precedes any event the Hub posts for us from now on.
        enqueue(frame_of(events::kReady, ready_payload(who->user_id, deps.svc.now_ms())));
        asio::co_spawn(ws_.get_executor(),
                       reauth_loop(shared_from_this(), &deps.svc, &deps.db_pool, msg.token,
                                   deps.reauth_interval),
                       asio::detached);
        continue;
      }
      if (!ws_.got_text()) continue;
      const auto msg = parse_client_message(beast::buffers_to_string(buf.data()));
      if (msg.type == ClientMessageType::Ping) enqueue(pong_frame());
    }
  }

  // Ends the session: no more writes, timers cancelled, socket closed (aborts a stalled write).
  void finish() {
    finished_ = true;
    queue_.clear();
    auth_timer_.cancel();
    close_timer_.cancel();
    reauth_timer_.cancel();
    abort();
  }

 private:
  static const std::shared_ptr<const std::string>& pong_frame() {
    static const auto f = frame_of(events::kPong);
    return f;
  }

  void abort() {
    beast::error_code ec;
    beast::get_lowest_layer(ws_).socket().close(ec);
  }

  void pump() {
    if (writing_ || finished_) return;
    writing_ = true;
    asio::co_spawn(ws_.get_executor(), write_loop(shared_from_this()), asio::detached);
  }

  static asio::awaitable<void> write_loop(std::shared_ptr<WsConn> self) {
    while (!self->finished_) {
      if (!self->queue_.empty()) {
        auto f = std::move(self->queue_.front());
        self->queue_.pop_front();
        auto [ec, n] = co_await self->ws_.async_write(asio::buffer(*f), kTuple);
        if (ec) {
          self->abort();
          break;
        }
        continue;
      }
      if (self->close_req_ && !self->close_sent_) {
        self->close_sent_ = true;
        auto [ec] = co_await self->ws_.async_close(*self->close_req_, kTuple);
        if (ec) self->abort();
      }
      break;
    }
    self->writing_ = false;
  }

  // DESIGN step 5b: revocations by other processes (reset-password), expiry, disable.
  static asio::awaitable<void> reauth_loop(std::shared_ptr<WsConn> self, Services* svc,
                                           asio::thread_pool* pool, std::string token,
                                           std::chrono::milliseconds every) {
    for (;;) {
      self->reauth_timer_.expires_after(every);
      auto [ec] = co_await self->reauth_timer_.async_wait(kTuple);
      if (ec || self->finished_ || self->close_req_) co_return;
      std::optional<http::Principal> who;
      bool lookup_failed = false;
      try {
        who = co_await http::run_blocking(*pool, [svc, &token] { return http::authenticate(*svc, token); });
      } catch (const std::exception& e) {
        // Database trouble is not a revocation: keep the socket and try again next period.
        log::warn("ws re-authentication error", {{"error", e.what()}});
        lookup_failed = true;
      }
      if (self->finished_ || self->close_req_) co_return;
      if (lookup_failed) continue;
      if (!who) {
        self->enqueue(frame_of(events::kSessionRevoked));
        self->request_close(kCloseAuthFailed, "session revoked");
        co_return;
      }
    }
  }

  Stream ws_;
  std::size_t cap_;
  std::deque<std::shared_ptr<const std::string>> queue_;
  bool writing_ = false;
  bool close_sent_ = false;
  bool finished_ = false;
  bool authed_ = false;
  int64_t user_id_ = 0;
  std::optional<websocket::close_reason> close_req_;
  asio::steady_timer auth_timer_;
  asio::steady_timer close_timer_;
  asio::steady_timer reauth_timer_;
};

asio::awaitable<void> reject_handshake(beast::tcp_stream& stream, unsigned version) {
  bhttp::response<bhttp::string_body> res{bhttp::status::forbidden, version};
  res.set(bhttp::field::content_type, "application/json; charset=utf-8");
  res.set("X-Content-Type-Options", "nosniff");
  res.set(bhttp::field::cache_control, "no-store");
  res.body() = boost::json::serialize(ApiError::forbidden("forbidden", "不允许的来源").to_json());
  res.keep_alive(false);
  res.prepare_payload();
  stream.expires_after(std::chrono::seconds(10));
  co_await bhttp::async_write(stream, res, kTuple);
  beast::error_code ec;
  stream.socket().shutdown(asio::ip::tcp::socket::shutdown_send, ec);
  stream.socket().close(ec);
}

}  // namespace

ClientMessage parse_client_message(std::string_view text) {
  ClientMessage m;
  boost::system::error_code ec;
  boost::json::parse_options opt;
  opt.max_depth = 16;
  auto v = boost::json::parse(text, ec, {}, opt);
  if (ec || !v.is_object()) return m;
  const auto& o = v.as_object();
  const auto* type = o.if_contains("type");
  if (type == nullptr || !type->is_string()) return m;
  const auto& t = type->get_string();
  if (t == client_messages::kAuth) {
    m.type = ClientMessageType::Auth;
    if (const auto* tok = o.if_contains("token"); tok != nullptr && tok->is_string())
      m.token = std::string(tok->get_string());
  } else if (t == client_messages::kPing) {
    m.type = ClientMessageType::Ping;
  } else {
    m.type = ClientMessageType::Other;
  }
  return m;
}

asio::awaitable<void> run_ws_session(beast::tcp_stream stream, bhttp::request<bhttp::empty_body> upgrade,
                                     WsDeps& deps) {
  try {
    // Browsers do not apply CORS to WebSockets: the Origin allowlist is the CSWSH defence.
    std::string_view origin;
    if (auto it = upgrade.find(bhttp::field::origin); it != upgrade.end()) origin = it->value();
    if (!http::origin_allowed(deps.cors, origin)) {
      log::info("ws upgrade rejected: origin not allowed");
      co_await reject_handshake(stream, upgrade.version());
      co_return;
    }

    beast::get_lowest_layer(stream).expires_never();  // the websocket stream manages timeouts
    auto conn = std::make_shared<WsConn>(std::move(stream), deps.cfg.ws_queue_cap);
    auto& ws = conn->ws();
    ws.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));
    ws.set_option(websocket::stream_base::decorator(
        [](websocket::response_type& r) { r.set(bhttp::field::server, "azmail"); }));
    ws.read_message_max(deps.cfg.ws_max_message_bytes);
    auto [ec] = co_await ws.async_accept(upgrade, kTuple);
    if (ec) {
      conn->finish();
      co_return;
    }
    ws.text(true);
    conn->arm_auth_deadline(std::chrono::milliseconds(std::max(deps.cfg.ws_auth_timeout_ms, 1)));
    co_await conn->serve(deps);
  } catch (const std::exception& e) {
    log::debug("ws session ended with an exception", {{"error", e.what()}});
  } catch (...) {
    log::debug("ws session ended with an unknown exception");
  }
}

}  // namespace azm::ws
