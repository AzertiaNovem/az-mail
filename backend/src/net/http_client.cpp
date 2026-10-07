// Owner: WP-C
// Synchronous HTTP(S) client over async Beast with real deadlines (DESIGN A3, Addendum A).
//
// Each hop (request + response, redirects are separate hops) runs a coroutine on a PRIVATE
// io_context driven by the calling thread, so every network operation is bounded by
// tcp_stream::expires_after: connect/handshake by ClientOptions::connect_timeout, each
// read/write by ClientOptions::read_timeout, all capped by the request's overall deadline.
// DNS lookups of host names run on a short-lived helper thread so they obey the deadline too
// (asio's resolver cannot be cancelled while getaddrinfo blocks).
#include "net/http_client.hpp"

#include "config.hpp"
#include "core/crypto.hpp"
#include "core/strings.hpp"

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/url.hpp>

#include <openssl/ssl.h>
#include <openssl/x509_vfy.h>

#include <fstream>
#include <future>
#include <system_error>
#include <thread>

namespace azm::net {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace ssl = asio::ssl;
namespace fs = std::filesystem;
using tcp = asio::ip::tcp;
using SteadyClock = std::chrono::steady_clock;

constexpr std::size_t kErrorBodyCap = 64u << 10;  // non-2xx bodies when a sink was requested
constexpr std::size_t kChunk = 64u << 10;
constexpr std::uint32_t kHeaderLimit = 64u << 10;

constexpr auto tuple_awaitable = asio::as_tuple(asio::use_awaitable);

// ---- URL --------------------------------------------------------------------------------------

struct Target {
  bool tls = false;
  std::string host;         // decoded host / IP literal without brackets (connect, SNI, verify)
  std::string port;         // numeric service
  std::string host_header;  // Host header value: host[:port] (port only when non-default)
  std::string origin;       // scheme://host:port, compared across redirects
  std::string target;       // origin-form request target "/path?query"
  std::string url;          // the absolute URL of this hop
};

Target parse_target(std::string_view url, bool allow_insecure) {
  auto parsed = boost::urls::parse_uri(url);
  if (!parsed) throw NetError(NetError::Kind::Protocol, "invalid URL");
  const boost::urls::url_view u = *parsed;
  const std::string scheme = to_lower_ascii(u.scheme());
  Target t;
  if (scheme == "https") {
    t.tls = true;
  } else if (scheme == "http") {
    if (!allow_insecure)
      throw NetError(NetError::Kind::InsecureScheme,
                     "plain http:// is not allowed (AZMAIL_ALLOW_INSECURE_HTTP=1 enables it for the mock)");
  } else {
    throw NetError(NetError::Kind::InsecureScheme, "unsupported URL scheme '" + scheme + "'");
  }
  t.host = u.host_address();
  if (t.host.empty()) throw NetError(NetError::Kind::Protocol, "URL has no host");
  const std::uint16_t default_port = t.tls ? 443 : 80;
  std::uint16_t port = default_port;
  if (u.has_port()) {
    if (u.port().empty()) throw NetError(NetError::Kind::Protocol, "URL has an empty port");
    port = u.port_number();
    if (port == 0) throw NetError(NetError::Kind::Protocol, "URL has an invalid port");
  }
  t.port = std::to_string(port);
  t.host_header = std::string(u.encoded_host());  // keeps [] around IPv6 literals
  if (port != default_port) t.host_header += ":" + t.port;
  t.origin = scheme + "://" + to_lower_ascii(t.host) + ":" + t.port;
  t.target = u.encoded_path().empty() ? "/" : std::string(u.encoded_path());
  if (u.has_query()) {
    t.target.push_back('?');
    t.target += u.encoded_query();
  }
  t.url = std::string(url);
  return t;
}

bool is_ip_literal(const std::string& host) {
  boost::system::error_code ec;
  (void)asio::ip::make_address(host, ec);
  return !ec;
}

// ---- deadlines --------------------------------------------------------------------------------

struct Deadline {
  SteadyClock::time_point at = SteadyClock::time_point::max();
  std::chrono::milliseconds total{0};
  bool expired() const { return SteadyClock::now() >= at; }
  // Budget for one operation: min(per_op, time left). Throws Timeout when nothing is left.
  std::chrono::steady_clock::duration budget(std::chrono::milliseconds per_op) const {
    const auto now = SteadyClock::now();
    if (now >= at)
      throw NetError(NetError::Kind::Timeout,
                     "request deadline of " + std::to_string(total.count()) + " ms exceeded");
    const auto left = at - now;
    return per_op.count() > 0 ? std::min<SteadyClock::duration>(per_op, left) : left;
  }
};

// ---- errors -----------------------------------------------------------------------------------

enum class Phase { Connect, Handshake, Write, ReadHeader, ReadBody };

std::string_view phase_name(Phase p) {
  switch (p) {
    case Phase::Connect: return "connecting to";
    case Phase::Handshake: return "TLS handshake with";
    case Phase::Write: return "sending request to";
    case Phase::ReadHeader: return "waiting for response from";
    case Phase::ReadBody: return "reading response body from";
  }
  return "talking to";
}

bool is_tls_error(const boost::system::error_code& ec) {
  return ec.category() == asio::error::get_ssl_category() ||
         ec.category() == ssl::error::get_stream_category();
}

[[noreturn]] void fail(const boost::system::error_code& ec, Phase phase, const Target& t,
                       const Deadline& dl, std::string_view tls_detail = {}) {
  const std::string where = std::string(phase_name(phase)) + " " + t.host + ":" + t.port;
  if (ec == beast::error::timeout) {
    if (dl.expired())
      throw NetError(NetError::Kind::Timeout, "request deadline of " + std::to_string(dl.total.count()) +
                                                  " ms exceeded while " + where);
    throw NetError(NetError::Kind::Timeout, "timed out " + where);
  }
  if (phase == Phase::Connect)
    throw NetError(NetError::Kind::Connect, "cannot connect to " + t.host + ":" + t.port + ": " + ec.message());
  if (phase == Phase::Handshake || is_tls_error(ec)) {
    std::string msg = "TLS error " + where + ": " + ec.message();
    if (!tls_detail.empty()) msg += " (" + std::string(tls_detail) + ")";
    throw NetError(NetError::Kind::Tls, msg);
  }
  if (ec == http::error::end_of_stream || ec == asio::error::eof ||
      ec == asio::error::connection_reset || ec == asio::error::broken_pipe ||
      ec == http::error::partial_message)
    throw NetError(NetError::Kind::Protocol, "connection closed while " + where);
  throw NetError(NetError::Kind::Protocol, "HTTP error while " + where + ": " + ec.message());
}

// ---- DNS ---------------------------------------------------------------------------------------

std::vector<tcp::endpoint> resolve(const Target& t, const Deadline& dl,
                                   std::chrono::milliseconds connect_timeout) {
  const auto port = static_cast<std::uint16_t>(std::stoi(t.port));
  if (is_ip_literal(t.host)) return {tcp::endpoint(asio::ip::make_address(t.host), port)};
  // getaddrinfo cannot be interrupted: run it on a detached helper so the caller can give up at
  // the deadline (the helper finishes on its own and drops the result).
  auto promise = std::make_shared<std::promise<std::vector<tcp::endpoint>>>();
  auto future = promise->get_future();
  std::thread([promise, host = t.host, service = t.port] {
    try {
      asio::io_context ioc;
      tcp::resolver r(ioc);
      std::vector<tcp::endpoint> out;
      for (const auto& e : r.resolve(host, service)) out.push_back(e.endpoint());
      promise->set_value(std::move(out));
    } catch (...) {
      promise->set_exception(std::current_exception());
    }
  }).detach();
  if (future.wait_for(dl.budget(connect_timeout)) != std::future_status::ready)
    throw NetError(NetError::Kind::Timeout, "DNS lookup of " + t.host + " timed out");
  try {
    auto eps = future.get();
    if (eps.empty()) throw NetError(NetError::Kind::Connect, "DNS lookup of " + t.host + " returned no addresses");
    return eps;
  } catch (const NetError&) {
    throw;
  } catch (const std::exception& e) {
    throw NetError(NetError::Kind::Connect, "cannot resolve " + t.host + ": " + e.what());
  }
}

// ---- one hop -----------------------------------------------------------------------------------

enum class BodyDest {
  String,     // into HttpResponse::body, limit max_body (TooLarge beyond)
  Sink,       // 2xx with a sink: stream to the file, limit max_body, sha256
  Truncated,  // error document / redirect body: first 64 KiB kept, rest dropped
};

struct HopInput {
  const Target* target = nullptr;
  http::verb method = http::verb::get;
  std::vector<std::pair<std::string, std::string>> headers;  // already filtered
  const std::string* body = nullptr;                          // nullptr = none
  const fs::path* body_file = nullptr;
  bool follow_redirects = false;  // read only the head of 3xx bodies
  const fs::path* sink = nullptr;
  std::size_t max_body = 0;
};

struct HopResult {
  HttpResponse resp;
  std::optional<std::string> location;
};

bool is_redirect(unsigned s) { return s == 301 || s == 302 || s == 303 || s == 307 || s == 308; }

class SinkFile {
 public:
  explicit SinkFile(const fs::path& p) : path_(p) {
    out_.open(p, std::ios::binary | std::ios::trunc);
    if (!out_) throw NetError(NetError::Kind::Io, "cannot create download file " + p.filename().string());
  }
  ~SinkFile() {
    if (!committed_) {
      out_.close();
      std::error_code ec;
      fs::remove(path_, ec);  // never leave a partial download behind
    }
  }
  SinkFile(const SinkFile&) = delete;
  SinkFile& operator=(const SinkFile&) = delete;
  void write(const char* p, std::size_t n) {
    out_.write(p, static_cast<std::streamsize>(n));
    if (!out_) throw NetError(NetError::Kind::Io, "cannot write download file " + path_.filename().string());
    sha_.update(p, n);
  }
  std::string commit() {
    out_.flush();
    out_.close();
    if (!out_) throw NetError(NetError::Kind::Io, "cannot finish download file " + path_.filename().string());
    committed_ = true;
    return sha_.final_hex();
  }

 private:
  fs::path path_;
  std::ofstream out_;
  crypto::Sha256 sha_;
  bool committed_ = false;
};

template <class Stream>
auto& lowest(Stream& s) {
  return beast::get_lowest_layer(s);
}

template <class Stream, class Body>
asio::awaitable<void> write_request(Stream& stream, http::request<Body>& req, const Target& t,
                                    const Deadline& dl, std::chrono::milliseconds io_timeout) {
  http::request_serializer<Body> sr{req};
  while (!sr.is_done()) {
    lowest(stream).expires_after(dl.budget(io_timeout));
    auto [ec, n] = co_await http::async_write_some(stream, sr, tuple_awaitable);
    (void)n;
    if (ec) fail(ec, Phase::Write, t, dl);
  }
}

template <class Stream>
asio::awaitable<HopResult> exchange(Stream& stream, const HopInput& in, const Deadline& dl,
                                    const ClientOptions& opts) {
  const Target& t = *in.target;

  // Request.
  auto fill_fields = [&](auto& req) {
    req.method(in.method);
    req.target(t.target);
    req.version(11);
    req.set(http::field::host, t.host_header);
    for (const auto& [k, v] : in.headers) req.insert(k, v);
    req.set(http::field::connection, "close");  // one exchange per connection
  };
  if (in.body_file != nullptr) {
    http::request<http::file_body> req;
    fill_fields(req);
    boost::system::error_code ec;
    req.body().open(in.body_file->c_str(), beast::file_mode::scan, ec);
    if (ec) throw NetError(NetError::Kind::Io, "cannot open request body file " + in.body_file->filename().string());
    req.prepare_payload();  // Content-Length = file size (R2 rejects chunked uploads)
    co_await write_request(stream, req, t, dl, opts.read_timeout);
  } else {
    http::request<http::string_body> req;
    fill_fields(req);
    if (in.body != nullptr) req.body() = *in.body;
    req.prepare_payload();
    if (in.body != nullptr && !in.body->empty() && !req.has_content_length())
      req.content_length(in.body->size());
    co_await write_request(stream, req, t, dl, opts.read_timeout);
  }

  // Response header.
  beast::flat_buffer buf;
  http::response_parser<http::buffer_body> parser;
  parser.header_limit(kHeaderLimit);
  parser.body_limit(boost::none);  // enforced below per destination
  if (in.method == http::verb::head) parser.skip(true);
  lowest(stream).expires_after(dl.budget(opts.read_timeout));
  {
    auto [ec, n] = co_await http::async_read_header(stream, buf, parser, tuple_awaitable);
    (void)n;
    if (ec) fail(ec, Phase::ReadHeader, t, dl);
  }

  HopResult out;
  const auto& head = parser.get();
  out.resp.status = head.result_int();
  out.resp.final_url = t.url;
  for (const auto& f : head) out.resp.headers.emplace_back(to_lower_ascii(f.name_string()), std::string(f.value()));
  if (in.follow_redirects && is_redirect(out.resp.status)) {
    if (auto loc = out.resp.header("location"); loc && !trim(*loc).empty()) out.location = std::string(trim(*loc));
  }

  BodyDest dest = BodyDest::String;
  if (out.location) dest = BodyDest::Truncated;
  else if (in.sink != nullptr) dest = (out.resp.status >= 200 && out.resp.status < 300) ? BodyDest::Sink : BodyDest::Truncated;
  if (dest != BodyDest::Truncated) {
    if (auto len = parser.content_length(); len && *len > in.max_body)
      throw NetError(NetError::Kind::TooLarge, "response body of " + std::to_string(*len) +
                                                   " bytes exceeds the limit of " + std::to_string(in.max_body));
  }

  std::optional<SinkFile> sink;
  if (dest == BodyDest::Sink) sink.emplace(*in.sink);
  std::vector<char> chunk(kChunk);
  std::uint64_t total = 0;
  // Takes `got` freshly parsed body bytes; false = stop reading (truncated error document).
  auto consume = [&](std::size_t got) -> bool {
    if (got == 0) return true;
    total += got;
    if (dest == BodyDest::Truncated) {
      const std::size_t room = kErrorBodyCap - std::min(kErrorBodyCap, out.resp.body.size());
      out.resp.body.append(chunk.data(), std::min(room, got));
      return out.resp.body.size() < kErrorBodyCap;  // rest dropped; the connection is discarded
    }
    if (total > in.max_body)
      throw NetError(NetError::Kind::TooLarge,
                     "response body exceeds the limit of " + std::to_string(in.max_body) + " bytes");
    if (dest == BodyDest::Sink) sink->write(chunk.data(), got);
    else out.resp.body.append(chunk.data(), got);
    return true;
  };
  while (!parser.is_done()) {
    parser.get().body().data = chunk.data();
    parser.get().body().size = chunk.size();
    lowest(stream).expires_after(dl.budget(opts.read_timeout));
    auto [ec, n] = co_await http::async_read(stream, buf, parser, tuple_awaitable);
    (void)n;
    if (ec == http::error::need_buffer) ec = {};
    if (!consume(chunk.size() - parser.get().body().size)) break;
    if (ec) {
      // A TLS peer that closes without close_notify ends an until-EOF body.
      if (ec == ssl::error::stream_truncated && parser.need_eof()) {
        boost::system::error_code eof_ec;
        parser.put_eof(eof_ec);
        if (!eof_ec) break;
      }
      fail(ec, Phase::ReadBody, t, dl);
    }
  }
  out.resp.body_size = total;
  if (sink) out.resp.sink_sha256 = sink->commit();
  co_return out;
}

asio::awaitable<HopResult> plain_hop(asio::io_context& ioc, std::vector<tcp::endpoint> eps, HopInput in,
                                     Deadline dl, ClientOptions opts) {
  beast::tcp_stream stream(ioc);
  stream.expires_after(dl.budget(opts.connect_timeout));
  {
    auto [ec, ep] = co_await stream.async_connect(eps, tuple_awaitable);
    (void)ep;
    if (ec) fail(ec, Phase::Connect, *in.target, dl);
  }
  HopResult r = co_await exchange(stream, in, dl, opts);
  boost::system::error_code ignored;
  stream.socket().shutdown(tcp::socket::shutdown_both, ignored);
  co_return r;
}

asio::awaitable<HopResult> tls_hop(asio::io_context& ioc, ssl::context& ctx, std::vector<tcp::endpoint> eps,
                                   HopInput in, Deadline dl, ClientOptions opts) {
  const Target& t = *in.target;
  ssl::stream<beast::tcp_stream> stream(ioc, ctx);
  if (!is_ip_literal(t.host)) {
    // SNI (RFC 6066 forbids IP literals here).
    if (!SSL_set_tlsext_host_name(stream.native_handle(), t.host.c_str()))
      throw NetError(NetError::Kind::Tls, "cannot set TLS SNI for " + t.host);
  }
  stream.set_verify_mode(ssl::verify_peer);
  stream.set_verify_callback(ssl::host_name_verification(t.host));
  lowest(stream).expires_after(dl.budget(opts.connect_timeout));
  {
    auto [ec, ep] = co_await lowest(stream).async_connect(eps, tuple_awaitable);
    (void)ep;
    if (ec) fail(ec, Phase::Connect, t, dl);
  }
  lowest(stream).expires_after(dl.budget(opts.connect_timeout));
  {
    auto [ec] = co_await stream.async_handshake(ssl::stream_base::client, tuple_awaitable);
    if (ec) {
      const long vr = SSL_get_verify_result(stream.native_handle());
      std::string detail;
      if (vr != X509_V_OK) detail = std::string("certificate verify failed: ") + X509_verify_cert_error_string(vr);
      fail(ec, Phase::Handshake, t, dl, detail);
    }
  }
  HopResult r = co_await exchange(stream, in, dl, opts);
  // No TLS close_notify round trip: the response is complete and the connection is discarded.
  boost::system::error_code ignored;
  lowest(stream).socket().shutdown(tcp::socket::shutdown_both, ignored);
  co_return r;
}

// Headers callers may not set (derived here) or that must never be sent (Accept-Encoding).
bool reserved_header(std::string_view name) {
  return iequals(name, "host") || iequals(name, "content-length") || iequals(name, "transfer-encoding") ||
         iequals(name, "connection") || iequals(name, "accept-encoding");
}

}  // namespace

// =============================================================================================

struct HttpClient::Impl {
  ClientOptions opts;
  std::unique_ptr<ssl::context> tls;  // null when the trust store could not be loaded
  std::string tls_error;

  void init_tls() {
    try {
      auto ctx = std::make_unique<ssl::context>(ssl::context::tls_client);
      ctx->set_options(ssl::context::default_workarounds | ssl::context::no_sslv2 |
                       ssl::context::no_sslv3 | ssl::context::no_tlsv1 | ssl::context::no_tlsv1_1);
      SSL_CTX_set_min_proto_version(ctx->native_handle(), TLS1_2_VERSION);
      ctx->set_verify_mode(ssl::verify_peer);
      if (!opts.ca_file.empty()) ctx->load_verify_file(opts.ca_file);
      else ctx->set_default_verify_paths();
      tls = std::move(ctx);
    } catch (const std::exception& e) {
      tls_error = opts.ca_file.empty() ? std::string("cannot load the system CA store: ") + e.what()
                                       : "cannot load CA file " + opts.ca_file + ": " + e.what();
    }
  }
};

ClientOptions client_options_from(const Config& cfg) {
  ClientOptions o;
  o.user_agent = cfg.resend_user_agent;
  o.ca_file = cfg.ca_file;
  o.allow_insecure_http = cfg.allow_insecure_http;
  if (cfg.resend_timeout_sec > 0) o.read_timeout = std::chrono::seconds(cfg.resend_timeout_sec);
  return o;
}

std::optional<std::string> HttpResponse::header(std::string_view name) const {
  for (const auto& [k, v] : headers)
    if (iequals(k, name)) return v;
  return std::nullopt;
}

HttpClient::HttpClient(ClientOptions opts) : impl_(std::make_unique<Impl>()) {
  impl_->opts = std::move(opts);
  impl_->init_tls();
}
HttpClient::HttpClient() : impl_(std::make_unique<Impl>()) {}  // test doubles override send()
HttpClient::~HttpClient() = default;

const ClientOptions& HttpClient::options() const { return impl_->opts; }

HttpResponse HttpClient::send(const HttpRequest& req) {
  const ClientOptions& opts = impl_->opts;
  if (!req.body.empty() && req.body_file)
    throw std::invalid_argument("HttpRequest: set either body or body_file, not both");
  Deadline dl;
  dl.total = req.timeout;
  if (req.timeout.count() > 0) dl.at = SteadyClock::now() + req.timeout;

  // Caller headers, minus the reserved ones; a caller User-Agent replaces the default.
  std::vector<std::pair<std::string, std::string>> headers;
  bool has_ua = false;
  for (const auto& [k, v] : req.headers) {
    if (reserved_header(k)) continue;
    if (iequals(k, "user-agent")) has_ua = true;
    headers.emplace_back(k, v);
  }
  if (!has_ua && !opts.user_agent.empty()) headers.emplace_back("User-Agent", opts.user_agent);

  http::verb method = req.method;
  bool with_body = true;
  std::string url = req.url;
  std::string first_origin;
  bool auth_dropped = false;
  for (int hop = 0;; ++hop) {
    const Target t = parse_target(url, opts.allow_insecure_http);
    if (hop == 0) {
      first_origin = t.origin;
    } else if (!auth_dropped && t.origin != first_origin) {
      // Cross-origin redirect (e.g. to an S3 presigned URL): never forward credentials.
      std::erase_if(headers, [](const auto& h) { return iequals(h.first, "authorization"); });
      auth_dropped = true;
    }
    if (t.tls && !impl_->tls) {
      throw NetError(NetError::Kind::Tls,
                     impl_->tls_error.empty() ? "TLS is not available in this client" : impl_->tls_error);
    }
    auto eps = resolve(t, dl, opts.connect_timeout);

    HopInput in;
    in.target = &t;
    in.method = method;
    in.headers = headers;
    if (with_body) {
      if (req.body_file) in.body_file = &*req.body_file;
      else if (!req.body.empty()) in.body = &req.body;
    }
    in.follow_redirects = req.max_redirects > 0;
    in.sink = req.sink ? &*req.sink : nullptr;
    in.max_body = req.max_body;

    asio::io_context ioc;
    std::exception_ptr err;
    std::optional<HopResult> result;
    auto on_done = [&](std::exception_ptr e, HopResult r) {
      err = e;
      if (!e) result.emplace(std::move(r));
    };
    if (t.tls) asio::co_spawn(ioc, tls_hop(ioc, *impl_->tls, std::move(eps), in, dl, opts), on_done);
    else asio::co_spawn(ioc, plain_hop(ioc, std::move(eps), in, dl, opts), on_done);
    ioc.run();
    if (err) std::rethrow_exception(err);
    if (!result) throw NetError(NetError::Kind::Protocol, "request did not complete");

    if (!result->location) return std::move(result->resp);
    if (hop >= req.max_redirects)
      throw NetError(NetError::Kind::Protocol,
                     "too many redirects (limit " + std::to_string(req.max_redirects) + ")");
    // Resolve the Location relative to this hop's URL.
    auto base = boost::urls::parse_uri(t.url);
    auto ref = boost::urls::parse_uri_reference(*result->location);
    if (!base || !ref) throw NetError(NetError::Kind::Protocol, "invalid redirect Location");
    boost::urls::url next;
    if (auto rv = boost::urls::resolve(*base, *ref, next); !rv)
      throw NetError(NetError::Kind::Protocol, "invalid redirect Location");
    next.remove_fragment();
    url = std::string(next.buffer());
    const unsigned status = result->resp.status;
    if ((status == 303 && method != http::verb::head) ||
        ((status == 301 || status == 302) && method == http::verb::post)) {
      method = http::verb::get;  // the body is not re-sent after a method change
      with_body = false;
      std::erase_if(headers, [](const auto& h) { return iequals(h.first, "content-type"); });
    }
  }
}

}  // namespace azm::net
