// Owner: WP-C — net::HttpClient against the in-process fake server (plain + TLS): deadlines,
// redirects without credential leaks, streamed uploads/downloads, limits, error kinds.
#include "../fakes/fake_http_server.hpp"
#include "config.hpp"
#include "core/crypto.hpp"
#include "net/http_client.hpp"
#include "test_support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <atomic>
#include <fstream>
#include <thread>

using namespace azm;
using namespace azm::net;
using azm::test::FakeHttpServer;
using azm::test::FakeRequest;
using azm::test::FakeResponse;
using Catch::Matchers::ContainsSubstring;
using namespace std::chrono_literals;
namespace bhttp = boost::beast::http;

namespace {

ClientOptions insecure_opts() {
  ClientOptions o;
  o.allow_insecure_http = true;
  o.user_agent = "azmail-test/1.0";
  o.connect_timeout = 2s;
  o.read_timeout = 5s;
  return o;
}

HttpRequest get(std::string url) {
  HttpRequest r;
  r.url = std::move(url);
  r.timeout = 10s;
  return r;
}

std::string read_file(const std::filesystem::path& p) {
  std::ifstream in(p, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), {});
}

void write_file(const std::filesystem::path& p, const std::string& data) {
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  out << data;
}

FakeResponse echo(const FakeRequest& req) {
  boost::json::object o;
  o["method"] = req.method;
  o["target"] = req.target;
  o["body"] = req.body;
  boost::json::object h;
  for (const auto& [k, v] : req.headers) h[k] = v;
  o["headers"] = h;
  return FakeResponse::json(200, boost::json::serialize(o));
}

NetError::Kind kind_of(HttpClient& c, const HttpRequest& r) {
  try {
    (void)c.send(r);
  } catch (const NetError& e) {
    return e.kind;
  }
  FAIL("expected NetError");
  return NetError::Kind::Protocol;
}

uint16_t closed_port() {
  boost::asio::io_context ioc;
  boost::asio::ip::tcp::acceptor a(ioc, {boost::asio::ip::make_address("127.0.0.1"), 0});
  return a.local_endpoint().port();  // closed when `a` goes away
}

}  // namespace

TEST_CASE("http client: GET basics, lowercased headers, default User-Agent, no Accept-Encoding",
          "[http_client]") {
  FakeHttpServer srv([](const FakeRequest& req) {
    auto r = echo(req);
    r.headers.emplace_back("X-Custom-Header", "Value");
    return r;
  });
  HttpClient client(insecure_opts());
  auto req = get(srv.base_url() + "/v1/emails?x=1%202");
  req.headers = {{"Accept-Encoding", "gzip"}, {"Authorization", "Bearer k"}, {"Host", "evil"}};
  const auto resp = client.send(req);
  CHECK(resp.status == 200);
  CHECK(resp.final_url == req.url);
  CHECK(resp.header("x-custom-header") == std::optional<std::string>("Value"));
  CHECK(resp.header("X-CUSTOM-HEADER") == std::optional<std::string>("Value"));
  for (const auto& [k, v] : resp.headers) CHECK(k == to_lower_ascii(k));
  CHECK(resp.body_size == resp.body.size());

  const auto reqs = srv.requests();
  REQUIRE(reqs.size() == 1);
  CHECK(reqs[0].method == "GET");
  CHECK(reqs[0].target == "/v1/emails?x=1%202");
  CHECK(reqs[0].header("host") == std::optional<std::string>("127.0.0.1:" + std::to_string(srv.port())));
  CHECK(reqs[0].header("user-agent") == std::optional<std::string>("azmail-test/1.0"));
  CHECK(reqs[0].header("authorization") == std::optional<std::string>("Bearer k"));
  CHECK_FALSE(reqs[0].has_header("accept-encoding"));
  CHECK_FALSE(reqs[0].has_header("content-length"));

  // A caller-provided User-Agent replaces the default (exactly one is sent).
  req.headers = {{"User-Agent", "custom/2"}};
  (void)client.send(req);
  const auto r2 = srv.requests().back();
  CHECK(r2.header("user-agent") == std::optional<std::string>("custom/2"));
  int ua = 0;
  for (const auto& [k, v] : r2.headers) ua += k == "user-agent";
  CHECK(ua == 1);
}

TEST_CASE("http client: request bodies (string and streamed file) carry Content-Length",
          "[http_client]") {
  FakeHttpServer srv(echo);
  HttpClient client(insecure_opts());
  test::TempDir td;

  HttpRequest post = get(srv.base_url() + "/emails");
  post.method = bhttp::verb::post;
  post.headers = {{"Content-Type", "application/json"}};
  post.body = R"({"a":1})";
  CHECK(client.send(post).status == 200);
  auto last = srv.requests().back();
  CHECK(last.body == R"({"a":1})");
  CHECK(last.header("content-length") == std::optional<std::string>("7"));
  CHECK(last.header("content-type") == std::optional<std::string>("application/json"));

  // Empty POST still sends Content-Length: 0 (Resend cancel).
  HttpRequest empty_post = get(srv.base_url() + "/emails/x/cancel");
  empty_post.method = bhttp::verb::post;
  CHECK(client.send(empty_post).status == 200);
  CHECK(srv.requests().back().header("content-length") == std::optional<std::string>("0"));

  // 1 MiB + 3 bytes streamed from disk; never chunked (R2 needs Content-Length).
  std::string big(1048579, '\0');
  for (std::size_t i = 0; i < big.size(); ++i) big[i] = static_cast<char>(i * 31 % 251);
  const auto file = td / "upload.bin";
  write_file(file, big);
  HttpRequest put = get(srv.base_url() + "/bucket/key");
  put.method = bhttp::verb::put;
  put.body_file = file;
  CHECK(client.send(put).status == 200);
  last = srv.requests().back();
  CHECK(last.method == "PUT");
  CHECK(last.header("content-length") == std::optional<std::string>(std::to_string(big.size())));
  CHECK_FALSE(last.has_header("transfer-encoding"));
  CHECK(last.body == big);

  HttpRequest both = put;
  both.body = "x";
  CHECK_THROWS_AS(client.send(both), std::invalid_argument);
  HttpRequest missing = put;
  missing.body_file = td / "nope.bin";
  CHECK(kind_of(client, missing) == NetError::Kind::Io);
}

TEST_CASE("http client: response sink streams 2xx bodies with sha256; errors stay in memory",
          "[http_client]") {
  std::string payload(300000, 'p');
  for (std::size_t i = 0; i < payload.size(); i += 7) payload[i] = 'q';
  FakeHttpServer srv([&](const FakeRequest& req) {
    if (req.path == "/ok") return FakeResponse::text(200, payload, "application/octet-stream");
    if (req.path == "/chunked") {
      auto r = FakeResponse::text(200, payload);
      r.chunked = true;
      return r;
    }
    return FakeResponse::text(404, std::string(100000, 'e'));  // larger than the 64 KiB cap
  });
  HttpClient client(insecure_opts());
  test::TempDir td;

  for (std::string path : {"/ok", "/chunked"}) {
    auto req = get(srv.base_url() + path);
    req.sink = td / ("dl" + path.substr(1));
    const auto resp = client.send(req);
    CHECK(resp.status == 200);
    CHECK(resp.body.empty());
    CHECK(resp.body_size == payload.size());
    REQUIRE(resp.sink_sha256.has_value());
    CHECK(*resp.sink_sha256 == crypto::sha256_hex(payload));
    CHECK(read_file(*req.sink) == payload);
  }

  auto req = get(srv.base_url() + "/missing");
  req.sink = td / "never.bin";
  const auto resp = client.send(req);
  CHECK(resp.status == 404);
  CHECK(resp.body.size() == 64u << 10);  // truncated error document
  CHECK_FALSE(resp.sink_sha256.has_value());
  CHECK_FALSE(std::filesystem::exists(td / "never.bin"));
}

TEST_CASE("http client: max_body enforced for memory and sink (partial file removed)",
          "[http_client]") {
  FakeHttpServer srv([](const FakeRequest& req) {
    auto r = FakeResponse::text(200, std::string(200000, 'z'));
    if (req.path == "/chunked") r.chunked = true;
    if (req.path == "/eof") r.omit_content_length = true;
    return r;
  });
  HttpClient client(insecure_opts());
  test::TempDir td;
  for (std::string path : {"/len", "/chunked", "/eof"}) {
    auto req = get(srv.base_url() + path);
    req.max_body = 100000;
    CHECK(kind_of(client, req) == NetError::Kind::TooLarge);
    req.sink = td / "big.bin";
    CHECK(kind_of(client, req) == NetError::Kind::TooLarge);
    CHECK_FALSE(std::filesystem::exists(td / "big.bin"));
    req.sink.reset();
    req.max_body = 200000;  // exactly at the limit is fine
    CHECK(client.send(req).body.size() == 200000);
  }
}

TEST_CASE("http client: chunked and until-EOF bodies, HEAD, 4xx/5xx are not exceptions",
          "[http_client]") {
  FakeHttpServer srv([](const FakeRequest& req) {
    if (req.path == "/chunked") {
      auto r = FakeResponse::text(200, "hello chunked world");
      r.chunked = true;
      return r;
    }
    if (req.path == "/eof") {
      auto r = FakeResponse::text(200, "until eof");
      r.omit_content_length = true;
      return r;
    }
    if (req.path == "/head") return FakeResponse::text(200, std::string(5000, 'h'));
    return FakeResponse::json(503, R"({"error":"down"})");
  });
  HttpClient client(insecure_opts());
  CHECK(client.send(get(srv.base_url() + "/chunked")).body == "hello chunked world");
  CHECK(client.send(get(srv.base_url() + "/eof")).body == "until eof");
  auto head = get(srv.base_url() + "/head");
  head.method = bhttp::verb::head;
  const auto hr = client.send(head);
  CHECK(hr.status == 200);
  CHECK(hr.body.empty());
  CHECK(hr.header("content-length") == std::optional<std::string>("5000"));
  const auto err = client.send(get(srv.base_url() + "/err"));
  CHECK(err.status == 503);
  CHECK(err.body == R"({"error":"down"})");
}

TEST_CASE("http client: a stalled server hits the overall deadline", "[http_client][timeout]") {
  FakeHttpServer srv([](const FakeRequest&) {
    auto r = FakeResponse::text(200, "late");
    r.delay_before_headers = 5s;
    return r;
  });
  HttpClient client(insecure_opts());
  auto req = get(srv.base_url() + "/slow");
  req.timeout = 300ms;
  const auto t0 = std::chrono::steady_clock::now();
  try {
    (void)client.send(req);
    FAIL("expected timeout");
  } catch (const NetError& e) {
    CHECK(e.kind == NetError::Kind::Timeout);
    CHECK(e.retryable());
    CHECK_THAT(std::string(e.what()), ContainsSubstring("deadline"));
  }
  const auto elapsed = std::chrono::steady_clock::now() - t0;
  CHECK(elapsed >= 250ms);
  CHECK(elapsed < 2s);
}

TEST_CASE("http client: idle read timeout while the body stalls", "[http_client][timeout]") {
  FakeHttpServer srv([](const FakeRequest&) {
    auto r = FakeResponse::text(200, std::string(1000, 'b'));
    r.partial_bytes = 10;
    r.stall_after_partial = 5s;
    return r;
  });
  auto opts = insecure_opts();
  opts.read_timeout = 200ms;
  HttpClient client(opts);
  auto req = get(srv.base_url() + "/stall");
  req.timeout = 20s;
  const auto t0 = std::chrono::steady_clock::now();
  try {
    (void)client.send(req);
    FAIL("expected timeout");
  } catch (const NetError& e) {
    CHECK(e.kind == NetError::Kind::Timeout);
    CHECK_THAT(std::string(e.what()), ContainsSubstring("reading response body"));
  }
  CHECK(std::chrono::steady_clock::now() - t0 < 3s);
}

TEST_CASE("http client: connection refused and closed-without-response errors", "[http_client]") {
  HttpClient client(insecure_opts());
  auto req = get("http://127.0.0.1:" + std::to_string(closed_port()) + "/");
  try {
    (void)client.send(req);
    FAIL("expected connect error");
  } catch (const NetError& e) {
    CHECK(e.kind == NetError::Kind::Connect);
    CHECK(e.retryable());
  }
  FakeHttpServer srv([](const FakeRequest&) {
    FakeResponse r;
    r.close_without_response = true;
    return r;
  });
  CHECK(kind_of(client, get(srv.base_url() + "/")) == NetError::Kind::Protocol);
}

TEST_CASE("http client: scheme policy and URL validation", "[http_client]") {
  ClientOptions strict;
  HttpClient client(strict);
  CHECK(kind_of(client, get("http://127.0.0.1:1/")) == NetError::Kind::InsecureScheme);
  CHECK(kind_of(client, get("ftp://example.com/x")) == NetError::Kind::InsecureScheme);
  CHECK(kind_of(client, get("not a url")) == NetError::Kind::Protocol);
  CHECK(kind_of(client, get("https:///nohost")) == NetError::Kind::Protocol);
  CHECK_FALSE(NetError(NetError::Kind::InsecureScheme, "x").retryable());
}

TEST_CASE("http client: redirects keep Authorization same-origin and drop it cross-origin",
          "[http_client][redirect]") {
  FakeHttpServer target(echo);
  FakeHttpServer origin([&](const FakeRequest& req) {
    FakeResponse r;
    r.status = 302;
    if (req.path == "/same") r.headers.emplace_back("Location", "/landing?ok=1");
    else if (req.path == "/cross") r.headers.emplace_back("Location", target.base_url("localhost") + "/blob?sig=abc");
    else if (req.path == "/landing") return echo(req);
    else if (req.path == "/loop") r.headers.emplace_back("Location", "/loop");
    else if (req.path == "/see-other") {
      r.status = 303;
      r.headers.emplace_back("Location", "/landing");
    } else if (req.path == "/temp") {
      r.status = 307;
      r.headers.emplace_back("Location", "/landing");
    }
    return r;
  });
  HttpClient client(insecure_opts());

  auto req = get(origin.base_url() + "/same");
  req.headers = {{"Authorization", "Bearer secret"}};
  req.max_redirects = 3;
  auto resp = client.send(req);
  CHECK(resp.status == 200);
  CHECK(resp.final_url == origin.base_url() + "/landing?ok=1");
  CHECK(origin.requests().back().header("authorization") == std::optional<std::string>("Bearer secret"));

  req.url = origin.base_url() + "/cross";
  resp = client.send(req);
  CHECK(resp.status == 200);
  CHECK(resp.final_url == target.base_url("localhost") + "/blob?sig=abc");
  REQUIRE(target.request_count() == 1);
  CHECK_FALSE(target.requests()[0].has_header("authorization"));
  CHECK(target.requests()[0].target == "/blob?sig=abc");

  // max_redirects = 0: the 3xx is returned as-is.
  req.url = origin.base_url() + "/cross";
  req.max_redirects = 0;
  resp = client.send(req);
  CHECK(resp.status == 302);
  CHECK(resp.header("location").has_value());
  CHECK(target.request_count() == 1);

  req.url = origin.base_url() + "/loop";
  req.max_redirects = 2;
  try {
    (void)client.send(req);
    FAIL("expected too many redirects");
  } catch (const NetError& e) {
    CHECK(e.kind == NetError::Kind::Protocol);
    CHECK_THAT(std::string(e.what()), ContainsSubstring("too many redirects"));
  }

  // 303 turns a POST into a body-less GET; 307 keeps the method and body.
  HttpRequest post = get(origin.base_url() + "/see-other");
  post.method = bhttp::verb::post;
  post.body = "payload";
  post.headers = {{"Content-Type", "text/plain"}};
  post.max_redirects = 1;
  CHECK(client.send(post).status == 200);
  auto landed = origin.requests().back();
  CHECK(landed.method == "GET");
  CHECK(landed.body.empty());
  CHECK_FALSE(landed.has_header("content-type"));
  post.url = origin.base_url() + "/temp";
  CHECK(client.send(post).status == 200);
  landed = origin.requests().back();
  CHECK(landed.method == "POST");
  CHECK(landed.body == "payload");
}

TEST_CASE("http client: concurrent sends are independent", "[http_client]") {
  FakeHttpServer srv([](const FakeRequest& req) { return FakeResponse::text(200, req.path); });
  HttpClient client(insecure_opts());
  std::atomic<int> ok{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; ++t) {
    threads.emplace_back([&, t] {
      for (int i = 0; i < 5; ++i) {
        const std::string path = "/t" + std::to_string(t) + "/" + std::to_string(i);
        if (client.send(get(srv.base_url() + path)).body == path) ++ok;
      }
    });
  }
  for (auto& th : threads) th.join();
  CHECK(ok == 40);
  CHECK(srv.request_count() == 40);
}

TEST_CASE("http client: TLS with peer and host-name verification", "[http_client][tls]") {
  test::TempDir td;
  const auto good = test::make_self_signed_cert("localhost", "DNS:localhost,IP:127.0.0.1");
  write_file(td / "ca.pem", good.cert_pem);
  FakeHttpServer srv(echo, good);

  ClientOptions opts;
  opts.ca_file = (td / "ca.pem").string();
  opts.user_agent = "azmail-tls";
  HttpClient client(opts);
  for (std::string host : {"127.0.0.1", "localhost"}) {
    const auto resp = client.send(get(srv.base_url(host) + "/secure"));
    CHECK(resp.status == 200);
    const auto body = boost::json::parse(resp.body).as_object();
    CHECK(body.at("target").as_string() == "/secure");
  }
  const auto last = srv.requests().back();
  CHECK(last.header("user-agent") == std::optional<std::string>("azmail-tls"));
  CHECK(last.header("host") == std::optional<std::string>("localhost:" + std::to_string(srv.port())));

  // The system trust store does not know our self-signed certificate.
  HttpClient system_store{ClientOptions{}};
  try {
    (void)system_store.send(get(srv.base_url() + "/secure"));
    FAIL("expected TLS failure");
  } catch (const NetError& e) {
    CHECK(e.kind == NetError::Kind::Tls);
    CHECK_FALSE(e.retryable());
  }

  // Trusted certificate, wrong name.
  const auto other = test::make_self_signed_cert("other.example", "DNS:other.example");
  write_file(td / "other.pem", other.cert_pem);
  FakeHttpServer wrong(echo, other);
  ClientOptions o2;
  o2.ca_file = (td / "other.pem").string();
  HttpClient c2(o2);
  try {
    (void)c2.send(get(wrong.base_url() + "/"));
    FAIL("expected host name mismatch");
  } catch (const NetError& e) {
    CHECK(e.kind == NetError::Kind::Tls);
  }

  // Unreadable CA file: the client still constructs; https requests fail clearly.
  ClientOptions bad;
  bad.ca_file = (td / "missing.pem").string();
  HttpClient c3(bad);
  try {
    (void)c3.send(get(srv.base_url() + "/"));
    FAIL("expected CA load failure");
  } catch (const NetError& e) {
    CHECK(e.kind == NetError::Kind::Tls);
    CHECK_THAT(std::string(e.what()), ContainsSubstring("CA file"));
  }
}

TEST_CASE("http client: a silent peer times out during the TLS handshake", "[http_client][tls][timeout]") {
  // Accepts TCP connections but never speaks: the handshake must hit connect_timeout.
  boost::asio::io_context ioc;
  boost::asio::ip::tcp::acceptor acceptor(ioc, {boost::asio::ip::make_address("127.0.0.1"), 0});
  const auto port = acceptor.local_endpoint().port();
  std::thread holder([&] {
    boost::system::error_code ec;
    boost::asio::ip::tcp::socket s(ioc);
    acceptor.accept(s, ec);
    std::this_thread::sleep_for(1500ms);
  });
  ClientOptions opts;
  opts.connect_timeout = 200ms;
  HttpClient client(opts);
  auto req = get("https://127.0.0.1:" + std::to_string(port) + "/");
  const auto t0 = std::chrono::steady_clock::now();
  try {
    (void)client.send(req);
    FAIL("expected handshake timeout");
  } catch (const NetError& e) {
    CHECK(e.kind == NetError::Kind::Timeout);
    CHECK_THAT(std::string(e.what()), ContainsSubstring("TLS handshake"));
  }
  CHECK(std::chrono::steady_clock::now() - t0 < 1400ms);
  holder.join();
}

TEST_CASE("http client: options from Config", "[http_client]") {
  Config cfg;
  cfg.resend_user_agent = "azmail/9.9";
  cfg.ca_file = "/etc/ssl/custom.pem";
  cfg.allow_insecure_http = true;
  cfg.resend_timeout_sec = 12;
  const auto o = client_options_from(cfg);
  CHECK(o.user_agent == "azmail/9.9");
  CHECK(o.ca_file == "/etc/ssl/custom.pem");
  CHECK(o.allow_insecure_http);
  CHECK(o.read_timeout == 12s);
}

// ---- RT-1: HEAD of a large object --------------------------------------------------------------

TEST_CASE("http client: HEAD of an object over max_body is not TooLarge (RT-1)", "[http_client]") {
  FakeHttpServer srv([](const FakeRequest&) { return FakeResponse::text(200, std::string(200000, 'h')); });
  HttpClient client(insecure_opts());
  auto head = get(srv.base_url() + "/obj");
  head.method = bhttp::verb::head;
  head.max_body = 64u << 10;  // R2BlobStore's error-body limit, smaller than the object
  const auto r = client.send(head);
  CHECK(r.status == 200);
  CHECK(r.body.empty());
  CHECK(r.body_size == 0);
  CHECK(r.header("content-length") == std::optional<std::string>("200000"));
  // A GET of the same object still enforces the limit.
  auto g = get(srv.base_url() + "/obj");
  g.max_body = 64u << 10;
  CHECK(kind_of(client, g) == NetError::Kind::TooLarge);
}

// ---- RT-7: shutdown abort ---------------------------------------------------------------------

TEST_CASE("http client: CancelSignal aborts sends in flight and fails later ones fast (RT-7)",
          "[http_client][timeout]") {
  FakeHttpServer srv([](const FakeRequest& req) {
    if (req.path == "/stall") {
      auto r = FakeResponse::text(200, "late");
      r.delay_before_headers = 20s;
      return r;
    }
    if (req.path == "/partial") {
      auto r = FakeResponse::text(200, std::string(100000, 'p'));
      r.partial_bytes = 1000;
      r.stall_after_partial = 20s;
      return r;
    }
    return FakeResponse::text(200, "ok");
  });
  auto opts = insecure_opts();
  opts.read_timeout = 30s;
  auto cancel = std::make_shared<CancelSignal>();
  opts.cancel = cancel;
  HttpClient client(opts);
  CHECK(client.send(get(srv.base_url() + "/ok")).status == 200);
  CHECK_FALSE(client.cancelled());

  test::TempDir td;
  const auto sink = td / "download.bin";
  std::atomic<int> aborted{0};
  auto run = [&](HttpRequest r) {
    r.timeout = 60s;
    try {
      (void)client.send(r);
    } catch (const NetError& e) {
      if (e.kind == NetError::Kind::Aborted) ++aborted;
    }
  };
  HttpRequest partial = get(srv.base_url() + "/partial");
  partial.sink = sink;
  const auto t0 = std::chrono::steady_clock::now();
  std::thread a(run, get(srv.base_url() + "/stall"));
  std::thread b(run, partial);
  for (int i = 0; i < 500 && srv.request_count() < 3; ++i) std::this_thread::sleep_for(10ms);
  REQUIRE(srv.request_count() == 3);  // both are waiting on the server now
  std::this_thread::sleep_for(100ms);
  cancel->cancel();
  a.join();
  b.join();
  CHECK(aborted == 2);
  CHECK(std::chrono::steady_clock::now() - t0 < 5s);  // not the 20 s stalls nor the 60 s deadline
  CHECK_FALSE(std::filesystem::exists(sink));          // the partial download is removed
  CHECK(client.cancelled());

  // Later sends fail before connecting.
  CHECK(kind_of(client, get(srv.base_url() + "/ok")) == NetError::Kind::Aborted);
  CHECK(srv.request_count() == 3);
  CHECK(NetError(NetError::Kind::Aborted, "x").retryable());
  // A client without the signal is unaffected.
  HttpClient other(insecure_opts());
  CHECK(other.send(get(srv.base_url() + "/ok")).status == 200);
  CHECK_FALSE(other.cancelled());
}
