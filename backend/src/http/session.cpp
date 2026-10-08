// Owner: WP-A
// Per-connection HTTP/1.1 coroutine (DESIGN A1/A2, "Request lifecycle"; see session.hpp for the
// step list). Everything here runs on the connection's strand; blocking work (dispatch, File-route
// pre-authentication) is handed to a thread pool with run_blocking and resumes on the strand.
#include "http/session.hpp"

#include "config.hpp"
#include "core/blob_store.hpp"
#include "core/crypto.hpp"
#include "core/log.hpp"
#include "core/strings.hpp"
#include "db/sqlite.hpp"
#include "http/blocking.hpp"
#include "http/dispatch.hpp"
#include "http/router.hpp"
#include "services.hpp"
#include "ws/hub.hpp"
#include "ws/ws_session.hpp"

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core/buffers_range.hpp>
#include <boost/beast/core/file.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket/rfc6455.hpp>

#include <array>
#include <charconv>
#include <chrono>
#include <system_error>

namespace azm::http {

namespace asio = boost::asio;
namespace bhttp = boost::beast::http;
using tcp = asio::ip::tcp;
using namespace std::chrono_literals;

namespace detail {
// One live connection as seen by close_idle_connections. Fields are only touched on the
// connection's strand (`ex`), except `ex` itself which is immutable after registration.
struct ConnHandle {
  asio::any_io_executor ex;
  // The connection's stream; null once ownership moved to a WebSocket session or the session
  // ended (strand-only, like `idle`).
  beast::tcp_stream* stream = nullptr;
  bool idle = false;  // waiting for the first byte of the next request
};
}  // namespace detail

namespace {

constexpr auto kTuple = asio::as_tuple(asio::use_awaitable);
constexpr std::size_t kReadChunk = 16 * 1024;
constexpr std::size_t kLingerMaxBytes = 4u << 20;
constexpr auto kLingerTimeout = 2s;
constexpr std::string_view kWsPath = "/api/ws";

// ---- bodies -------------------------------------------------------------------------------

// BodyMode::File request body: streamed into an already-open staging file with a running
// sha256 (Addendum A), so the handler never re-reads a 25 MiB upload to hash it.
struct StagingBody {
  struct value_type {
    beast::file file;
    crypto::Sha256 sha;
    std::uint64_t size = 0;
    bool write_failed = false;  // local disk error (vs. a network error while reading)
  };

  class reader {
   public:
    template <bool isRequest, class Fields>
    explicit reader(bhttp::header<isRequest, Fields>&, value_type& b) : body_(b) {}

    void init(const boost::optional<std::uint64_t>&, beast::error_code& ec) {
      if (!body_.file.is_open()) {
        ec = std::make_error_code(std::errc::bad_file_descriptor);
        return;
      }
      ec = {};
    }

    template <class ConstBufferSequence>
    std::size_t put(const ConstBufferSequence& buffers, beast::error_code& ec) {
      std::size_t n = 0;
      for (const auto b : beast::buffers_range_ref(buffers)) {
        body_.file.write(b.data(), b.size(), ec);
        if (ec) {
          body_.write_failed = true;
          return n;
        }
        body_.sha.update(b.data(), b.size());
        n += b.size();
      }
      body_.size += n;
      ec = {};
      return n;
    }

    void finish(beast::error_code& ec) { ec = {}; }

   private:
    value_type& body_;
  };
};

// Response file body serving [offset, offset + length) of an open file (single-range 206 and
// plain 200 alike; beast's file_body can only serve "from the position to the end").
struct RangeFileBody {
  struct value_type {
    beast::file file;
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
  };

  static std::uint64_t size(const value_type& v) { return v.length; }

  class writer {
   public:
    using const_buffers_type = asio::const_buffer;

    template <bool isRequest, class Fields>
    writer(bhttp::header<isRequest, Fields>&, value_type& b) : body_(b), remain_(b.length) {}

    void init(beast::error_code& ec) { body_.file.seek(body_.offset, ec); }

    boost::optional<std::pair<const_buffers_type, bool>> get(beast::error_code& ec) {
      const std::size_t amount =
          remain_ > sizeof(buf_) ? sizeof(buf_) : static_cast<std::size_t>(remain_);
      if (amount == 0) {
        ec = {};
        return boost::none;
      }
      const std::size_t n = body_.file.read(buf_, amount, ec);
      if (ec) return boost::none;
      if (n == 0) {
        ec = bhttp::make_error_code(bhttp::error::short_read);
        return boost::none;
      }
      remain_ -= n;
      return {{const_buffers_type(buf_, n), remain_ > 0}};
    }

   private:
    value_type& body_;
    std::uint64_t remain_;
    char buf_[64 * 1024];
  };
};

// ---- small helpers -------------------------------------------------------------------------

std::optional<asio::ip::address> parse_ip(std::string_view s) {
  s = trim(s);
  if (s.size() >= 2 && s.front() == '[' && s.back() == ']') s = s.substr(1, s.size() - 2);
  if (s.empty()) return std::nullopt;
  boost::system::error_code ec;
  auto a = asio::ip::make_address(std::string(s), ec);
  if (ec) return std::nullopt;
  if (a.is_v6() && a.to_v6().is_v4_mapped())
    return asio::ip::address(asio::ip::make_address_v4(asio::ip::v4_mapped, a.to_v6()));
  return a;
}

bool ip_matches(const asio::ip::address& ip, std::string_view spec) {
  spec = trim(spec);
  const auto slash = spec.find('/');
  auto base = parse_ip(spec.substr(0, slash));
  if (!base) return false;
  if (slash == std::string_view::npos) return *base == ip;
  if (base->is_v4() != ip.is_v4()) return false;
  const std::string_view bits_s = spec.substr(slash + 1);
  int bits = -1;
  auto [p, ec] = std::from_chars(bits_s.data(), bits_s.data() + bits_s.size(), bits);
  const int max_bits = ip.is_v4() ? 32 : 128;
  if (ec != std::errc() || p != bits_s.data() + bits_s.size() || bits < 0 || bits > max_bits) return false;
  auto cmp = [bits](const auto& a, const auto& b) {
    int left = bits;
    for (std::size_t i = 0; i < a.size() && left > 0; ++i, left -= 8) {
      const unsigned mask = left >= 8 ? 0xffu : (0xffu << (8 - left)) & 0xffu;
      if ((a[i] & mask) != (b[i] & mask)) return false;
    }
    return true;
  };
  if (ip.is_v4()) return cmp(ip.to_v4().to_bytes(), base->to_v4().to_bytes());
  return cmp(ip.to_v6().to_bytes(), base->to_v6().to_bytes());
}

bool ip_trusted(const asio::ip::address& ip, std::span<const std::string> entries) {
  for (const auto& e : entries)
    if (ip_matches(ip, e)) return true;
  return false;
}

bool header_safe(std::string_view s) {
  for (char c : s)
    if (c == '\r' || c == '\n' || c == '\0') return false;
  return true;
}

void set_or_replace(Response& res, std::string_view name, std::string value) {
  for (auto& [k, v] : res.headers) {
    if (iequals(k, name)) {
      v = std::move(value);
      return;
    }
  }
  res.headers.emplace_back(std::string(name), std::move(value));
}

template <class Body>
void fill_headers(bhttp::response<Body>& r, const Response& res) {
  if (!res.content_type.empty()) r.set(bhttp::field::content_type, res.content_type);
  for (const auto& [k, v] : res.headers) {
    if (k.empty() || !header_safe(k) || !header_safe(v)) {
      log::warn("dropping invalid response header", {{"name", k}});
      continue;
    }
    r.insert(k, v);
  }
}

// Writes a whole message, re-arming the idle timeout per chunk (tcp_stream deadlines cover a
// whole operation, so a single async_write of a large file would need a huge timeout).
template <class Body>
asio::awaitable<bool> write_message(beast::tcp_stream& stream, bhttp::response<Body>& msg,
                                    std::chrono::seconds idle) {
  bhttp::response_serializer<Body> sr{msg};
  while (!sr.is_done()) {
    stream.expires_after(idle);
    auto [ec, n] = co_await bhttp::async_write_some(stream, sr, kTuple);
    if (ec) co_return false;
  }
  co_return true;
}

template <class Parser>
asio::awaitable<beast::error_code> read_body(beast::tcp_stream& stream, beast::flat_buffer& buf,
                                             Parser& p, std::chrono::seconds idle) {
  while (!p.is_done()) {
    stream.expires_after(idle);  // idle timeout, re-armed for every chunk (A2)
    auto [ec, n] = co_await bhttp::async_read_some(stream, buf, p, kTuple);
    if (ec) co_return ec;
  }
  co_return beast::error_code{};
}

// Closes after a response that left request bytes unread: half-close, then drain for a short
// while, so the peer reads our response instead of getting a RST that may discard it.
asio::awaitable<void> linger_close(beast::tcp_stream& stream) {
  beast::error_code ec;
  stream.socket().shutdown(tcp::socket::shutdown_send, ec);
  if (ec) co_return;
  std::array<char, 16 * 1024> sink{};
  std::size_t total = 0;
  stream.expires_after(kLingerTimeout);
  while (total < kLingerMaxBytes) {
    auto [rec, n] = co_await stream.async_read_some(asio::buffer(sink), kTuple);
    if (rec) break;
    total += n;
  }
  stream.socket().close(ec);
}

enum class After { KeepAlive, Close, Linger, Upgraded };

std::string_view error_message_for(unsigned status) {
  switch (status) {
    case 400: return "请求无效";
    case 404: return "接口不存在";
    case 405: return "不支持该请求方法";
    case 413: return "请求内容过大";
    case 431: return "请求头过大";
    case 503: return "服务繁忙，请稍后再试";
    default: return "请求失败";
  }
}

std::string allow_header(const std::vector<beast::http::verb>& methods) {
  std::string out;
  for (auto m : methods) {
    if (!out.empty()) out += ", ";
    out += beast::http::to_string(m);
  }
  if (!out.empty()) out += ", OPTIONS";
  return out;
}

// State of one request/response exchange.
struct Exchange {
  beast::tcp_stream& stream;
  SessionShared& shared;
  const Config& cfg;
  Request req;
  std::optional<std::string> origin;
  unsigned version = 11;
  bool client_keep_alive = true;
  std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
  std::optional<std::filesystem::path> staged;  // File-mode staging file (removed in any case)

  Exchange(beast::tcp_stream& s, SessionShared& sh) : stream(s), shared(sh), cfg(sh.deps.cfg) {}
  Exchange(const Exchange&) = delete;
  Exchange& operator=(const Exchange&) = delete;
  ~Exchange() { remove_staged(); }

  void remove_staged() noexcept {
    if (!staged) return;
    std::error_code ec;
    std::filesystem::remove(*staged, ec);  // usually already consumed by put_file
    staged.reset();
  }

  std::chrono::seconds idle() const { return std::chrono::seconds(std::max(cfg.body_idle_timeout_sec, 1)); }

  asio::thread_pool& pool_for(Exec e) {
    switch (e) {
      case Exec::Db: return shared.deps.db_pool;
      case Exec::Net: return shared.deps.net_pool;
      case Exec::Files: return shared.deps.files_pool ? *shared.deps.files_pool : shared.deps.net_pool;
    }
    return shared.deps.db_pool;
  }

  void log_access(unsigned status) const {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    log::ScopedRequestId rid(req.request_id);
    // Path only: query strings may carry signed-URL signatures (D5) and are never logged.
    const auto level = req.path == "/api/health" ? log::Level::Debug : log::Level::Info;
    log::write(level, "http",
               {{"method", std::string(beast::http::to_string(req.method))},
                {"path", req.path},
                {"status", status},
                {"ms", static_cast<int64_t>(ms)},
                {"ip", req.remote_ip}});
  }

  // Finalizes and writes `res`. `body_unread`: request bytes remain on the wire → close after.
  asio::awaitable<After> respond(Response res, bool body_unread) {
    remove_staged();
    const bool keep = client_keep_alive && !body_unread && !shared.stopping.load();

    // File bodies: open now so a missing file becomes a clean 500, and resolve Range.
    std::optional<RangeFileBody::value_type> file;
    std::optional<std::string> content_range;
    std::optional<std::filesystem::path> remove_after;
    if (auto* ref = std::get_if<FileRef>(&res.body)) {
      if (ref->remove_after_send) remove_after = ref->path;
      RangeFileBody::value_type body;
      beast::error_code ec;
      body.file.open(ref->path.c_str(), beast::file_mode::scan, ec);
      std::uint64_t size = 0;
      if (!ec) size = body.file.size(ec);
      if (ec) {
        log::ScopedRequestId rid(req.request_id);
        log::error("cannot open response file", {{"path", req.path}, {"error", ec.message()}});
        res = Response::from_error(ApiError::internal());
      } else {
        body.offset = 0;
        body.length = size;
        auto range = req.header("Range");
        // If-Range would need validators we do not emit: serve the full representation.
        if (res.status == 200 && req.method == beast::http::verb::get && range && !req.header("If-Range")) {
          ByteRange br;
          switch (parse_range(*range, size, br)) {
            case RangeResult::Ok:
              res.status = 206;
              body.offset = br.first;
              body.length = br.last - br.first + 1;
              content_range = "bytes " + std::to_string(br.first) + "-" + std::to_string(br.last) + "/" +
                              std::to_string(size);
              break;
            case RangeResult::Unsatisfiable: {
              Response r = Response::error(416, "range_not_satisfiable", "请求的范围无效");
              for (const auto& [k, v] : res.headers)
                if (!iequals(k, "Content-Disposition")) r.headers.emplace_back(k, v);
              r.headers.emplace_back("Content-Range", "bytes */" + std::to_string(size));
              res = std::move(r);
              break;
            }
            case RangeResult::None: break;
          }
        }
        if (std::holds_alternative<FileRef>(res.body)) file.emplace(std::move(body));
      }
    }

    finalize_response(res, req.request_id, shared.cors, origin ? std::optional<std::string_view>(*origin) : std::nullopt);

    bool ok = false;
    try {
      const auto status = static_cast<bhttp::status>(res.status);
      if (file) {
        bhttp::response<RangeFileBody> r{status, version};
        fill_headers(r, res);
        r.set(bhttp::field::accept_ranges, "bytes");
        if (content_range) r.set(bhttp::field::content_range, *content_range);
        r.body() = std::move(*file);
        r.keep_alive(keep);
        r.prepare_payload();
        ok = co_await write_message(stream, r, idle());
      } else if (res.status == 204 || res.status == 304 || res.status < 200) {
        bhttp::response<bhttp::empty_body> r{status, version};
        fill_headers(r, res);
        r.keep_alive(keep);
        ok = co_await write_message(stream, r, idle());
      } else {
        bhttp::response<bhttp::string_body> r{status, version};
        fill_headers(r, res);
        if (auto* s = std::get_if<std::string>(&res.body)) r.body() = std::move(*s);
        r.keep_alive(keep);
        r.prepare_payload();
        ok = co_await write_message(stream, r, idle());
      }
    } catch (const std::exception& e) {
      log::ScopedRequestId rid(req.request_id);
      log::error("cannot serialize response", {{"path", req.path}, {"error", e.what()}});
      ok = false;
    }
    if (remove_after) {
      std::error_code ec;
      std::filesystem::remove(*remove_after, ec);  // after the write completed OR failed
    }
    log_access(res.status);
    if (!ok) co_return After::Close;
    if (body_unread) co_return After::Linger;
    co_return keep ? After::KeepAlive : After::Close;
  }

  asio::awaitable<After> fail(unsigned status, std::string_view code, bool body_unread) {
    co_return co_await respond(Response::error(status, code, error_message_for(status)), body_unread);
  }
};

// Handles one request whose header has been read into `hp`.
asio::awaitable<After> handle_request(beast::tcp_stream& stream, beast::flat_buffer& buf,
                                      bhttp::request_parser<bhttp::empty_body>& hp, SessionShared& shared,
                                      const std::string& peer_ip, detail::ConnHandle& handle) {
  Exchange x(stream, shared);
  const Config& cfg = shared.deps.cfg;
  auto& hreq = hp.get();
  x.version = hreq.version();
  x.client_keep_alive = hreq.keep_alive();
  x.req.request_id = make_request_id();
  x.req.method = hreq.method();
  x.req.remote_ip = peer_ip;
  if (auto o = hreq.find(bhttp::field::origin); o != hreq.end()) x.origin = std::string(o->value());
  const bool body_pending = !hp.is_done();

  bool bad_target = false;
  try {
    Request parsed = Request::make(hreq.method(), std::string_view(hreq.target()));
    parsed.request_id = std::move(x.req.request_id);
    x.req = std::move(parsed);
  } catch (const std::exception&) {
    bad_target = true;
  }
  if (bad_target) {
    x.req.path = "-";
    co_return co_await x.fail(400, "bad_request", body_pending);
  }
  x.req.headers = static_cast<const bhttp::fields&>(hreq);
  x.req.remote_ip = resolve_client_ip(peer_ip, x.req.header("X-Forwarded-For"), cfg.trusted_proxies);

  // 2. WebSocket upgrade: ownership of the stream moves to the WS session.
  if (x.req.path == kWsPath) {
    if (beast::websocket::is_upgrade(hreq) && hreq.method() == bhttp::verb::get) {
      ws::WsDeps wsd{cfg, shared.deps.svc, shared.deps.hub, shared.deps.db_pool, shared.cors,
                     shared.deps.ws_reauth_interval};
      {
        log::ScopedRequestId rid(x.req.request_id);
        log::debug("ws upgrade", {{"ip", x.req.remote_ip}});
      }
      handle.stream = nullptr;  // moved below: close_all_connections must not touch it any more
      co_await ws::run_ws_session(std::move(stream), hp.release(), wsd);
      co_return After::Upgraded;
    }
    co_return co_await x.respond(Response::error(400, "bad_request", "需要 WebSocket 升级请求"), body_pending);
  }

  // 3. CORS preflight, answered inline.
  if (x.req.method == bhttp::verb::options) {
    co_return co_await x.respond(
        preflight(shared.cors, x.req.header("Origin"), x.req.header("Access-Control-Request-Method"),
                  x.req.header("Access-Control-Request-Headers")),
        body_pending);
  }

  // 4. Route.
  auto m = shared.deps.router.match(x.req.method, x.req.path);
  if (m.route == nullptr) {
    if (m.path_exists) {
      Response r = Response::error(405, "method_not_allowed", error_message_for(405));
      r.headers.emplace_back("Allow", allow_header(shared.deps.router.allowed_methods(x.req.path)));
      co_return co_await x.respond(std::move(r), body_pending);
    }
    co_return co_await x.fail(404, "not_found", body_pending);
  }
  const Route& route = *m.route;

  // 5. Declared size over the route limit → 413 before reading anything.
  std::optional<std::uint64_t> declared;
  if (auto cl = hp.content_length()) declared = *cl;
  if (auto r = precheck_body(route, declared)) co_return co_await x.respond(std::move(*r), body_pending);

  // 5a. Authenticated routes check the Bearer token before a single body byte is read: uploads
  // are never staged on disk and no JSON body (8 MiB for drafts) is buffered for anonymous
  // clients (SEC-6). Bodiless requests skip this (dispatch authenticates them anyway).
  if ((body_pending || route.body == BodyMode::File) && (route.auth == AuthReq::User || route.auth == AuthReq::Admin)) {
    std::optional<Principal> who;
    std::optional<Response> failure;
    if (auto token = bearer_token(x.req)) {
      try {
        Services& svc = shared.deps.svc;
        std::string tok = std::move(*token);
        who = co_await run_blocking(shared.deps.db_pool, [&svc, &tok] { return authenticate(svc, tok); });
      } catch (const db::BusyError&) {
        failure = Response::from_error(ApiError::unavailable());
      } catch (const std::exception& e) {
        log::ScopedRequestId rid(x.req.request_id);
        log::error("request pre-authentication failed", {{"error", e.what()}});
        failure = Response::from_error(ApiError::internal());
      }
    }
    if (failure) co_return co_await x.respond(std::move(*failure), body_pending);
    if (!who) co_return co_await x.respond(Response::from_error(ApiError::unauthorized()), body_pending);
    if (route.auth == AuthReq::Admin && !who->is_admin)
      co_return co_await x.respond(Response::from_error(ApiError::forbidden()), body_pending);
  }

  // 5b. Body.
  if (body_pending) {
    if (auto expect = x.req.header("Expect"); expect && iequals(trim(*expect), "100-continue")) {
      bhttp::response<bhttp::empty_body> cont{bhttp::status::continue_, x.version};
      stream.expires_after(x.idle());
      auto [ec, n] = co_await bhttp::async_write(stream, cont, kTuple);
      if (ec) co_return After::Close;
    }
    auto body_error = [&](const beast::error_code& ec) -> std::optional<unsigned> {
      {
        log::ScopedRequestId rid(x.req.request_id);
        log::debug("request body read failed", {{"path", x.req.path}, {"error", ec.message()}});
      }
      if (ec == bhttp::error::body_limit || ec == bhttp::error::buffer_overflow) return 413u;
      if (ec == bhttp::error::bad_chunk || ec == bhttp::error::bad_chunk_extension ||
          ec == bhttp::error::bad_content_length || ec == bhttp::error::bad_transfer_encoding)
        return 400u;
      return std::nullopt;  // timeout / EOF / reset: nothing useful to answer
    };
    if (route.body == BodyMode::File) {
      bhttp::request_parser<StagingBody> fp(std::move(hp));
      fp.body_limit(route.body_limit);
      std::filesystem::path path;
      beast::error_code oec;
      try {
        path = make_staging_path(shared.deps.svc.blobs.tmp_dir());
        fp.get().body().file.open(path.c_str(), beast::file_mode::write_new, oec);
      } catch (const std::exception& e) {
        oec = std::make_error_code(std::errc::io_error);
        log::ScopedRequestId rid(x.req.request_id);
        log::error("cannot create staging file", {{"error", e.what()}});
      }
      if (oec) co_return co_await x.respond(
          Response::error(503, "storage_unavailable", "存储暂时不可用，请稍后重试"), true);
      x.staged = path;
      auto ec = co_await read_body(stream, buf, fp, x.idle());
      beast::error_code cec;
      fp.get().body().file.close(cec);
      if (ec) {
        if (fp.get().body().write_failed) {
          // A local disk error (e.g. disk full), not a network failure: tell the client.
          log::ScopedRequestId rid(x.req.request_id);
          log::error("upload staging failed", {{"error", ec.message()}});
          co_return co_await x.respond(Response::error(503, "storage_unavailable", "存储暂时不可用，请稍后重试"), true);
        }
        if (auto st = body_error(ec)) co_return co_await x.fail(*st, *st == 413 ? "payload_too_large" : "bad_request", true);
        co_return After::Close;
      }
      if (cec) co_return co_await x.respond(Response::error(503, "storage_unavailable", "存储暂时不可用，请稍后重试"), false);
      auto& body = fp.get().body();
      x.req.body_file = path;
      x.req.body_sha256 = body.sha.final_hex();
      x.req.body_size = static_cast<std::size_t>(body.size);
    } else {
      bhttp::request_parser<bhttp::string_body> sp(std::move(hp));
      sp.body_limit(route.body_limit);
      auto ec = co_await read_body(stream, buf, sp, x.idle());
      if (ec) {
        if (auto st = body_error(ec)) co_return co_await x.fail(*st, *st == 413 ? "payload_too_large" : "bad_request", true);
        co_return After::Close;
      }
      x.req.body_size = sp.get().body().size();
      if (route.body != BodyMode::None) x.req.body = std::move(sp.get().body());  // None: discarded
    }
  }

  // 6. In-flight cap, then the handler on its blocking pool.
  if (shared.inflight.fetch_add(1) >= cfg.inflight_cap) {
    shared.inflight.fetch_sub(1);
    co_return co_await x.fail(503, "service_unavailable", false);
  }
  Response res;
  {
    // Counts only the time on the pool, not the response write to a possibly slow client.
    struct InflightGuard {
      std::atomic<std::size_t>& n;
      ~InflightGuard() { n.fetch_sub(1); }
    } guard{shared.inflight};
    try {
      Services& svc = shared.deps.svc;
      const Request& req = x.req;
      Params params = std::move(m.params);
      res = co_await run_blocking(x.pool_for(route.exec), [&route, &req, &params, &svc] {
        return dispatch(route, req, std::move(params), svc);
      });
    } catch (const std::exception& e) {
      log::ScopedRequestId rid(x.req.request_id);
      log::error("dispatch failed", {{"path", x.req.path}, {"error", e.what()}});
      res = Response::from_error(ApiError::internal());
    }
  }
  co_return co_await x.respond(std::move(res), false);
}

std::string peer_address(const tcp::socket& s) {
  boost::system::error_code ec;
  auto ep = s.remote_endpoint(ec);
  if (ec) return {};
  auto a = ep.address();
  if (a.is_v6() && a.to_v6().is_v4_mapped()) return asio::ip::make_address_v4(asio::ip::v4_mapped, a.to_v6()).to_string();
  return a.to_string();
}

}  // namespace

// ---- public pure helpers ---------------------------------------------------------------------

bool ip_in_list(std::string_view ip, std::span<const std::string> entries) {
  auto a = parse_ip(ip);
  return a && ip_trusted(*a, entries);
}

std::string resolve_client_ip(std::string_view peer_ip, std::optional<std::string_view> xff,
                              std::span<const std::string> trusted_proxies) {
  auto peer = parse_ip(peer_ip);
  const std::string peer_s = peer ? peer->to_string() : std::string(peer_ip);
  if (!peer || !xff || !ip_trusted(*peer, trusted_proxies)) return peer_s;
  const auto parts = split(*xff, ',');
  std::string leftmost;
  // Right to left: entries appended by our own (trusted) proxies are skipped; the first
  // untrusted hop is the client as seen by the outermost trusted proxy.
  for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
    auto ip = parse_ip(*it);
    if (!ip) return peer_s;  // garbage where a proxy-written address should be: trust nothing
    if (!ip_trusted(*ip, trusted_proxies)) return ip->to_string();
    leftmost = ip->to_string();
  }
  return leftmost.empty() ? peer_s : leftmost;
}

std::optional<Response> precheck_body(const Route& route, std::optional<std::uint64_t> content_length) {
  if (content_length && *content_length > route.body_limit) {
    boost::json::object d;
    d["limit"] = route.body_limit;
    return Response::error(413, "payload_too_large", "请求内容过大", std::move(d));
  }
  return std::nullopt;
}

void finalize_response(Response& res, std::string_view request_id, const CorsPolicy& cors,
                       std::optional<std::string_view> origin) {
  set_or_replace(res, "X-Request-Id", std::string(request_id));
  if (!res.find_header("X-Content-Type-Options")) res.headers.emplace_back("X-Content-Type-Options", "nosniff");
  if (!res.find_header("Cache-Control")) res.headers.emplace_back("Cache-Control", "no-store");
  if (!res.find_header("Referrer-Policy")) res.headers.emplace_back("Referrer-Policy", "no-referrer");
  apply_cors(cors, origin, res);
}

std::string make_request_id() { return crypto::hex_encode(crypto::random_bytes(8)); }

RangeResult parse_range(std::string_view header, std::uint64_t size, ByteRange& out) {
  auto parse_u64 = [](std::string_view s, std::uint64_t& v) {
    if (s.empty()) return false;
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    return ec == std::errc() && p == s.data() + s.size();
  };
  std::string_view h = trim(header);
  if (!istarts_with(h, "bytes=")) return RangeResult::None;
  const std::string_view spec = trim(h.substr(6));
  if (spec.find(',') != std::string_view::npos) return RangeResult::None;  // multi-range: send 200
  const auto dash = spec.find('-');
  if (dash == std::string_view::npos) return RangeResult::None;
  const std::string_view a = trim(spec.substr(0, dash));
  const std::string_view b = trim(spec.substr(dash + 1));
  if (a.empty()) {  // suffix range: the last N bytes
    std::uint64_t n = 0;
    if (!parse_u64(b, n)) return RangeResult::None;
    if (n == 0 || size == 0) return RangeResult::Unsatisfiable;
    out.first = n >= size ? 0 : size - n;
    out.last = size - 1;
    return RangeResult::Ok;
  }
  std::uint64_t first = 0;
  if (!parse_u64(a, first)) return RangeResult::None;
  std::uint64_t last = 0;
  if (b.empty()) {
    last = size == 0 ? 0 : size - 1;
  } else {
    if (!parse_u64(b, last)) return RangeResult::None;
    if (last < first) return RangeResult::None;  // syntactically invalid → ignore the header
  }
  if (first >= size) return RangeResult::Unsatisfiable;
  if (last >= size) last = size - 1;
  out.first = first;
  out.last = last;
  return RangeResult::Ok;
}

// ---- the connection coroutine ----------------------------------------------------------------

void close_idle_connections(SessionShared& shared) {
  std::vector<std::shared_ptr<detail::ConnHandle>> live;
  {
    std::lock_guard lk(shared.conns_mu);
    for (auto& [id, w] : shared.conns)
      if (auto h = w.lock()) live.push_back(std::move(h));
  }
  for (auto& h : live) {
    std::weak_ptr<detail::ConnHandle> weak = h;
    asio::post(h->ex, [weak] {
      auto c = weak.lock();
      if (c && c->idle && c->stream) c->stream->cancel();
    });
  }
}

void close_all_connections(SessionShared& shared) {
  std::vector<std::shared_ptr<detail::ConnHandle>> live;
  {
    std::lock_guard lk(shared.conns_mu);
    for (auto& [id, w] : shared.conns)
      if (auto h = w.lock()) live.push_back(std::move(h));
  }
  for (auto& h : live) {
    std::weak_ptr<detail::ConnHandle> weak = h;
    asio::post(h->ex, [weak] {
      auto c = weak.lock();
      if (c && c->stream) {
        beast::error_code ec;
        c->stream->socket().shutdown(tcp::socket::shutdown_both, ec);
        c->stream->close();  // closes the socket and cancels its timer
      }
    });
  }
}

asio::awaitable<void> run_session(tcp::socket socket, SessionShared& shared) {
  shared.connections.fetch_add(1);
  struct ConnCount {
    SessionShared& s;
    std::uint64_t id = 0;
    ~ConnCount() {
      if (id != 0) {
        std::lock_guard lk(s.conns_mu);
        s.conns.erase(id);
      }
      s.connections.fetch_sub(1);
    }
  } count{shared};

  try {
    const Config& cfg = shared.deps.cfg;
    const std::string peer_ip = peer_address(socket);
    beast::tcp_stream stream(std::move(socket));
    auto handle = std::make_shared<detail::ConnHandle>();
    handle->ex = stream.get_executor();
    handle->stream = &stream;
    {
      std::lock_guard lk(shared.conns_mu);
      count.id = ++shared.next_conn_id;
      shared.conns.emplace(count.id, handle);
    }
    // Runs (on this strand) before `stream` is destroyed: a late close_idle_connections callback
    // holding the handle then finds nothing to cancel.
    struct Unlink {
      detail::ConnHandle& h;
      ~Unlink() {
        h.idle = false;
        h.stream = nullptr;
      }
    } unlink{*handle};

    // Large enough for any permitted header plus a read chunk.
    beast::flat_buffer buf(std::max<std::size_t>(64 * 1024, cfg.max_header_bytes + kReadChunk));
    const auto header_timeout = std::chrono::seconds(std::max(cfg.header_timeout_sec, 1));
    const auto keepalive_timeout = std::chrono::seconds(std::max(cfg.keepalive_timeout_sec, 1));
    bool first = true;
    After after = After::Close;

    for (;;) {
      after = After::Close;
      if (shared.stopping.load()) break;
      if (buf.size() == 0) {
        // Wait for the first byte of the next request (keep-alive idle, or connect → request).
        handle->idle = true;
        stream.expires_after(first ? header_timeout : keepalive_timeout);
        auto [ec, n] = co_await stream.async_read_some(buf.prepare(kReadChunk), kTuple);
        handle->idle = false;
        if (ec) break;
        buf.commit(n);
        if (shared.stopping.load()) break;
      }
      first = false;

      bhttp::request_parser<bhttp::empty_body> hp;
      hp.header_limit(static_cast<std::uint32_t>(std::min<std::size_t>(cfg.max_header_bytes, UINT32_MAX)));
      // No body limit on the header parser: Beast would otherwise reject a Content-Length over
      // its 1 MiB default while parsing the header. The route's limit is applied in step 5.
      hp.body_limit(boost::none);
      stream.expires_after(header_timeout);  // the whole header within 15 s (slowloris)
      auto [hec, hn] = co_await bhttp::async_read_header(stream, buf, hp, kTuple);
      if (hec) {
        if (hec == bhttp::error::header_limit || hec == bhttp::error::buffer_overflow) {
          Exchange x(stream, shared);
          x.req.request_id = make_request_id();
          x.req.path = "-";
          x.req.remote_ip = peer_ip;
          after = co_await x.respond(Response::error(431, "header_too_large", error_message_for(431)), true);
        } else if (hec != beast::error::timeout && hec != bhttp::error::end_of_stream &&
                   hec != asio::error::eof && hec != asio::error::connection_reset &&
                   hec != asio::error::operation_aborted && hec != bhttp::error::partial_message) {
          Exchange x(stream, shared);
          x.req.request_id = make_request_id();
          x.req.path = "-";
          x.req.remote_ip = peer_ip;
          after = co_await x.respond(Response::error(400, "bad_request", error_message_for(400)), true);
        }
        break;
      }

      after = co_await handle_request(stream, buf, hp, shared, peer_ip, *handle);
      if (after != After::KeepAlive) break;
    }

    if (after == After::Upgraded) co_return;  // the WS session owned (and closed) the stream
    if (after == After::Linger) {
      co_await linger_close(stream);
    } else {
      beast::error_code ec;
      stream.socket().shutdown(tcp::socket::shutdown_send, ec);
      stream.socket().close(ec);
    }
  } catch (const std::exception& e) {
    log::debug("http session ended with an exception", {{"error", e.what()}});
  } catch (...) {
    // Never let anything escape a detached coroutine (it would terminate an io thread).
    log::debug("http session ended with an unknown exception");
  }
}

}  // namespace azm::http
