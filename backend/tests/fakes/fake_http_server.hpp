// Owner: WP-C — in-process HTTP/1.1 (+ optional TLS) test server for hermetic network tests
// (net::HttpClient, resend::Client, R2BlobStore end-to-end). Header-only; included by
// tests/unit/test_*.cpp as "../fakes/fake_http_server.hpp".
//
// One server thread runs an io_context with coroutine sessions; every connection serves exactly
// one request ("Connection: close"). The handler runs on the server thread and returns a
// FakeResponse describing the bytes to send, including misbehaviour: stalls before the header,
// a stall in the middle of the body, a close without any response, chunked or until-EOF bodies.
// Stalls are timers, so stop() (or the destructor) is always prompt.
#pragma once

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace azm::test {

struct FakeRequest {
  std::string method;  // "GET", "PUT", …
  std::string target;  // raw request target "/path?query"
  std::string path;    // before '?'
  std::string query;   // after '?' ("" when none)
  std::vector<std::pair<std::string, std::string>> headers;  // lowercased names, in order
  std::string body;

  std::optional<std::string> header(std::string_view name) const {
    for (const auto& [k, v] : headers)
      if (k == name) return v;
    return std::nullopt;
  }
  bool has_header(std::string_view name) const { return header(name).has_value(); }
};

struct FakeResponse {
  unsigned status = 200;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;
  bool chunked = false;               // Transfer-Encoding: chunked (body split in 3 chunks)
  bool omit_content_length = false;   // body delimited by connection close
  bool close_without_response = false;
  std::chrono::milliseconds delay_before_headers{0};  // stall before sending anything
  std::size_t partial_bytes = 0;                      // with stall_after_partial: bytes sent first
  std::chrono::milliseconds stall_after_partial{0};   // stall after header + partial_bytes of body

  static FakeResponse text(unsigned status, std::string body, std::string content_type = "text/plain") {
    FakeResponse r;
    r.status = status;
    r.body = std::move(body);
    r.headers.emplace_back("Content-Type", std::move(content_type));
    return r;
  }
  static FakeResponse json(unsigned status, std::string body) {
    return text(status, std::move(body), "application/json");
  }
};

using FakeHandler = std::function<FakeResponse(const FakeRequest&)>;

// Self-signed certificate + key (PEM) for TLS tests. `san` like "DNS:localhost,IP:127.0.0.1".
struct TestCert {
  std::string cert_pem;
  std::string key_pem;
};

inline TestCert make_self_signed_cert(const std::string& common_name, const std::string& san) {
  auto fail = [](const char* what) -> TestCert { throw std::runtime_error(std::string("test cert: ") + what); };
  EVP_PKEY* pkey = EVP_EC_gen("P-256");
  if (pkey == nullptr) return fail("keygen");
  X509* x = X509_new();
  X509_set_version(x, 2);
  ASN1_INTEGER_set(X509_get_serialNumber(x), static_cast<long>(std::random_device{}() & 0x7fffffff));
  X509_gmtime_adj(X509_getm_notBefore(x), -3600);
  X509_gmtime_adj(X509_getm_notAfter(x), 24 * 3600);
  X509_set_pubkey(x, pkey);
  X509_NAME* name = X509_get_subject_name(x);
  X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                             reinterpret_cast<const unsigned char*>(common_name.c_str()), -1, -1, 0);
  X509_set_issuer_name(x, name);
  X509V3_CTX ctx;
  X509V3_set_ctx_nodb(&ctx);
  X509V3_set_ctx(&ctx, x, x, nullptr, nullptr, 0);
  const std::pair<int, std::string> exts[] = {
      {NID_subject_alt_name, san},
      {NID_basic_constraints, "critical,CA:TRUE"},
      {NID_key_usage, "critical,digitalSignature,keyCertSign"},
  };
  for (const auto& [nid, value] : exts) {
    X509_EXTENSION* ext = X509V3_EXT_conf_nid(nullptr, &ctx, nid, value.c_str());
    if (ext == nullptr) return fail("extension");
    X509_add_ext(x, ext, -1);
    X509_EXTENSION_free(ext);
  }
  if (X509_sign(x, pkey, EVP_sha256()) == 0) return fail("sign");
  auto to_pem = [](auto writer) {
    BIO* bio = BIO_new(BIO_s_mem());
    writer(bio);
    char* data = nullptr;
    const long n = BIO_get_mem_data(bio, &data);
    std::string out(data, static_cast<std::size_t>(n));
    BIO_free(bio);
    return out;
  };
  TestCert c;
  c.cert_pem = to_pem([&](BIO* b) { PEM_write_bio_X509(b, x); });
  c.key_pem = to_pem([&](BIO* b) { PEM_write_bio_PrivateKey(b, pkey, nullptr, nullptr, 0, nullptr, nullptr); });
  X509_free(x);
  EVP_PKEY_free(pkey);
  return c;
}

class FakeHttpServer {
 public:
  // Plain HTTP server on 127.0.0.1:<ephemeral>.
  explicit FakeHttpServer(FakeHandler handler) : FakeHttpServer(std::move(handler), nullptr) {}
  // HTTPS server presenting `cert`.
  FakeHttpServer(FakeHandler handler, const TestCert& cert)
      : FakeHttpServer(std::move(handler), make_tls_context(cert)) {}

  ~FakeHttpServer() { stop(); }
  FakeHttpServer(const FakeHttpServer&) = delete;
  FakeHttpServer& operator=(const FakeHttpServer&) = delete;

  std::uint16_t port() const { return port_; }
  std::string base_url(std::string_view host = "127.0.0.1") const {
    return std::string(tls_ ? "https://" : "http://") + std::string(host) + ":" + std::to_string(port_);
  }
  std::vector<FakeRequest> requests() const {
    std::lock_guard lk(mu_);
    return requests_;
  }
  std::size_t request_count() const {
    std::lock_guard lk(mu_);
    return requests_.size();
  }
  std::size_t connection_count() const {
    std::lock_guard lk(mu_);
    return connections_;
  }
  void set_handler(FakeHandler h) {
    std::lock_guard lk(mu_);
    handler_ = std::move(h);
  }
  void stop() {
    if (!thread_.joinable()) return;
    ioc_.stop();
    thread_.join();
  }

 private:
  using tcp = boost::asio::ip::tcp;

  static std::shared_ptr<boost::asio::ssl::context> make_tls_context(const TestCert& cert) {
    auto ctx = std::make_shared<boost::asio::ssl::context>(boost::asio::ssl::context::tls_server);
    ctx->use_certificate_chain(boost::asio::buffer(cert.cert_pem));
    ctx->use_private_key(boost::asio::buffer(cert.key_pem), boost::asio::ssl::context::pem);
    return ctx;
  }

  FakeHttpServer(FakeHandler handler, std::shared_ptr<boost::asio::ssl::context> tls)
      : handler_(std::move(handler)), tls_(std::move(tls)), acceptor_(ioc_) {
    const tcp::endpoint ep(boost::asio::ip::make_address("127.0.0.1"), 0);
    acceptor_.open(ep.protocol());
    acceptor_.set_option(boost::asio::socket_base::reuse_address(true));
    acceptor_.bind(ep);
    acceptor_.listen();
    port_ = acceptor_.local_endpoint().port();
    boost::asio::co_spawn(ioc_, accept_loop(), boost::asio::detached);
    thread_ = std::thread([this] { ioc_.run(); });
  }

  boost::asio::awaitable<void> accept_loop() {
    for (;;) {
      auto [ec, sock] = co_await acceptor_.async_accept(boost::asio::as_tuple(boost::asio::use_awaitable));
      if (ec) co_return;
      {
        std::lock_guard lk(mu_);
        ++connections_;
      }
      if (tls_) boost::asio::co_spawn(ioc_, tls_session(std::move(sock)), boost::asio::detached);
      else boost::asio::co_spawn(ioc_, plain_session(std::move(sock)), boost::asio::detached);
    }
  }

  boost::asio::awaitable<void> plain_session(tcp::socket sock) {
    boost::beast::tcp_stream stream(std::move(sock));
    co_await serve_one(stream);
    boost::system::error_code ec;
    stream.socket().shutdown(tcp::socket::shutdown_send, ec);
  }

  boost::asio::awaitable<void> tls_session(tcp::socket sock) {
    boost::asio::ssl::stream<boost::beast::tcp_stream> stream(boost::beast::tcp_stream(std::move(sock)), *tls_);
    auto [ec] = co_await stream.async_handshake(boost::asio::ssl::stream_base::server,
                                                boost::asio::as_tuple(boost::asio::use_awaitable));
    if (ec) co_return;  // e.g. the client rejected our certificate
    co_await serve_one(stream);
    auto [ec2] = co_await stream.async_shutdown(boost::asio::as_tuple(boost::asio::use_awaitable));
    (void)ec2;
  }

  boost::asio::awaitable<void> sleep_for(std::chrono::milliseconds d) {
    boost::asio::steady_timer t(ioc_, d);
    auto [ec] = co_await t.async_wait(boost::asio::as_tuple(boost::asio::use_awaitable));
    (void)ec;
  }

  static std::string reason(unsigned status) {
    return std::string(boost::beast::http::obsolete_reason(static_cast<boost::beast::http::status>(status)));
  }

  template <class Stream>
  boost::asio::awaitable<void> write_all(Stream& s, const std::string& data) {
    auto [ec, n] = co_await boost::asio::async_write(s, boost::asio::buffer(data),
                                                     boost::asio::as_tuple(boost::asio::use_awaitable));
    (void)ec;
    (void)n;
  }

  template <class Stream>
  boost::asio::awaitable<void> serve_one(Stream& stream) {
    namespace http = boost::beast::http;
    boost::beast::flat_buffer buf;
    http::request_parser<http::string_body> parser;
    parser.body_limit(512u << 20);
    auto [ec, n] = co_await http::async_read(stream, buf, parser, boost::asio::as_tuple(boost::asio::use_awaitable));
    (void)n;
    if (ec) co_return;
    const auto& msg = parser.get();
    FakeRequest req;
    req.method = std::string(msg.method_string());
    req.target = std::string(msg.target());
    const auto q = req.target.find('?');
    req.path = req.target.substr(0, q);
    req.query = q == std::string::npos ? "" : req.target.substr(q + 1);
    for (const auto& f : msg) {
      std::string name(f.name_string());
      for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      req.headers.emplace_back(std::move(name), std::string(f.value()));
    }
    req.body = msg.body();
    FakeHandler handler;
    {
      std::lock_guard lk(mu_);
      requests_.push_back(req);
      handler = handler_;
    }
    FakeResponse r = handler ? handler(req) : FakeResponse::text(500, "no handler");
    if (r.close_without_response) co_return;
    if (r.delay_before_headers.count() > 0) co_await sleep_for(r.delay_before_headers);

    std::string head = "HTTP/1.1 " + std::to_string(r.status) + " " + reason(r.status) + "\r\n";
    for (const auto& [k, v] : r.headers) head += k + ": " + v + "\r\n";
    head += "Connection: close\r\n";
    const bool is_head = req.method == "HEAD";
    std::string body_bytes;
    if (r.chunked) {
      head += "Transfer-Encoding: chunked\r\n";
      const std::size_t third = r.body.size() / 3 + 1;
      for (std::size_t off = 0; off < r.body.size(); off += third) {
        const std::string part = r.body.substr(off, third);
        char size_hex[32];
        std::snprintf(size_hex, sizeof size_hex, "%zx", part.size());
        body_bytes += std::string(size_hex) + "\r\n" + part + "\r\n";
      }
      body_bytes += "0\r\n\r\n";
    } else {
      if (!r.omit_content_length) head += "Content-Length: " + std::to_string(r.body.size()) + "\r\n";
      body_bytes = r.body;
    }
    head += "\r\n";
    if (is_head) body_bytes.clear();
    if (r.stall_after_partial.count() > 0) {
      const std::size_t first = std::min(r.partial_bytes, body_bytes.size());
      co_await write_all(stream, head + body_bytes.substr(0, first));
      co_await sleep_for(r.stall_after_partial);
      co_await write_all(stream, body_bytes.substr(first));
    } else {
      co_await write_all(stream, head + body_bytes);
    }
  }

  mutable std::mutex mu_;
  FakeHandler handler_;
  std::vector<FakeRequest> requests_;
  std::size_t connections_ = 0;
  std::shared_ptr<boost::asio::ssl::context> tls_;
  boost::asio::io_context ioc_;
  tcp::acceptor acceptor_;
  std::uint16_t port_ = 0;
  std::thread thread_;
};

}  // namespace azm::test
