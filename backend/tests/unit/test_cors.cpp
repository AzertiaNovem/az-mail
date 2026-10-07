// Owner: WP-A — CORS allowlist, preflight, response decoration, finalize_response.
#include "config.hpp"
#include "core/strings.hpp"
#include "http/cors.hpp"
#include "http/session.hpp"

#include <catch2/catch_test_macros.hpp>

#include <boost/json/parse.hpp>

using namespace azm;

namespace {

std::vector<std::string> headers_named(const http::Response& r, std::string_view name) {
  std::vector<std::string> out;
  for (const auto& [k, v] : r.headers)
    if (iequals(k, name)) out.push_back(v);
  return out;
}

http::CorsPolicy policy() {
  Config cfg;
  cfg.cors_origins = {" https://Mail.Example.com/ ", "http://localhost:5173", "not a url", "https://x.com/path",
                      "*", "http://[::1]:8080", "https://mail.example.com"};
  return http::cors_policy_from(cfg);
}

}  // namespace

TEST_CASE("cors: normalize_origin", "[cors]") {
  CHECK(http::normalize_origin("https://Mail.Example.COM") == std::optional<std::string>("https://mail.example.com"));
  CHECK(http::normalize_origin("HTTP://localhost:5173") == std::optional<std::string>("http://localhost:5173"));
  CHECK(http::normalize_origin("http://[::1]:8080") == std::optional<std::string>("http://[::1]:8080"));
  CHECK_FALSE(http::normalize_origin("null"));
  CHECK_FALSE(http::normalize_origin(""));
  CHECK_FALSE(http::normalize_origin("ftp://example.com"));
  CHECK_FALSE(http::normalize_origin("https://example.com/"));
  CHECK_FALSE(http::normalize_origin("https://example.com/app"));
  CHECK_FALSE(http::normalize_origin("https://user@example.com"));
  CHECK_FALSE(http::normalize_origin("https://example.com?x=1"));
  CHECK_FALSE(http::normalize_origin("https://example.com:0"));
  CHECK_FALSE(http::normalize_origin("https://example.com:99999"));
  CHECK_FALSE(http::normalize_origin("https://example.com:"));
  CHECK_FALSE(http::normalize_origin("example.com"));
}

TEST_CASE("cors: policy from config skips invalid entries and dedupes", "[cors]") {
  auto p = policy();
  CHECK(p.allowed_origins ==
        std::vector<std::string>{"https://mail.example.com", "http://localhost:5173", "http://[::1]:8080"});
  CHECK(p.allow_headers.find("Idempotency-Key") != std::string::npos);
  CHECK(p.allow_headers.find("Authorization") != std::string::npos);
  CHECK(p.expose_headers.find("X-Request-Id") != std::string::npos);
}

TEST_CASE("cors: origin_allowed is exact (scheme, host case-insensitive; port exact)", "[cors]") {
  auto p = policy();
  CHECK(http::origin_allowed(p, "https://mail.example.com"));
  CHECK(http::origin_allowed(p, "HTTPS://MAIL.EXAMPLE.COM"));
  CHECK(http::origin_allowed(p, "http://localhost:5173"));
  CHECK_FALSE(http::origin_allowed(p, "http://mail.example.com"));       // scheme differs
  CHECK_FALSE(http::origin_allowed(p, "https://mail.example.com:8443"));  // port differs
  CHECK_FALSE(http::origin_allowed(p, "https://evil-mail.example.com"));
  CHECK_FALSE(http::origin_allowed(p, "https://mail.example.com.evil.com"));
  CHECK_FALSE(http::origin_allowed(p, "http://localhost:5174"));
  CHECK_FALSE(http::origin_allowed(p, "null"));
  CHECK_FALSE(http::origin_allowed(p, ""));
  // A hand-built (non-canonical) policy still matches.
  http::CorsPolicy hand;
  hand.allowed_origins = {"HTTP://LocalHost:3000"};
  CHECK(http::origin_allowed(hand, "http://localhost:3000"));
}

TEST_CASE("cors: apply_cors", "[cors]") {
  auto p = policy();
  http::Response r = http::Response::json(boost::json::object{{"ok", true}});
  http::apply_cors(p, std::string_view("https://mail.example.com"), r);
  CHECK(headers_named(r, "Access-Control-Allow-Origin") == std::vector<std::string>{"https://mail.example.com"});
  CHECK(headers_named(r, "Vary") == std::vector<std::string>{"Origin"});
  CHECK(headers_named(r, "Access-Control-Expose-Headers").size() == 1);
  // Idempotent: applying twice duplicates nothing.
  http::apply_cors(p, std::string_view("https://mail.example.com"), r);
  CHECK(headers_named(r, "Access-Control-Allow-Origin").size() == 1);
  CHECK(headers_named(r, "Vary") == std::vector<std::string>{"Origin"});

  http::Response d = http::Response::no_content();
  http::apply_cors(p, std::string_view("https://evil.com"), d);
  CHECK(headers_named(d, "Access-Control-Allow-Origin").empty());
  CHECK(headers_named(d, "Vary") == std::vector<std::string>{"Origin"});

  http::Response n = http::Response::no_content();
  http::apply_cors(p, std::nullopt, n);
  CHECK(headers_named(n, "Access-Control-Allow-Origin").empty());
  CHECK(headers_named(n, "Vary") == std::vector<std::string>{"Origin"});

  // An existing Vary gains Origin instead of a second header.
  http::Response v = http::Response::no_content();
  v.add_header("Vary", "Accept-Encoding");
  http::apply_cors(p, std::nullopt, v);
  CHECK(headers_named(v, "Vary") == std::vector<std::string>{"Accept-Encoding, Origin"});
}

TEST_CASE("cors: preflight", "[cors]") {
  auto p = policy();
  auto ok = http::preflight(p, std::string_view("http://localhost:5173"), std::string_view("PUT"),
                            std::string_view("authorization, content-type, x-evil"));
  CHECK(ok.status == 204);
  CHECK(ok.find_header("Access-Control-Allow-Origin") == std::optional<std::string_view>("http://localhost:5173"));
  CHECK(ok.find_header("Access-Control-Allow-Methods").value().find("PATCH") != std::string_view::npos);
  CHECK(ok.find_header("Access-Control-Allow-Headers") == std::optional<std::string_view>(p.allow_headers));
  CHECK(ok.find_header("Access-Control-Max-Age") == std::optional<std::string_view>("600"));
  const auto vary = std::string(ok.find_header("Vary").value());
  CHECK(vary.find("Origin") != std::string::npos);
  CHECK(vary.find("Access-Control-Request-Method") != std::string::npos);
  // Unknown requested headers are never echoed back.
  CHECK(ok.find_header("Access-Control-Allow-Headers").value().find("x-evil") == std::string_view::npos);

  auto bad = http::preflight(p, std::string_view("https://evil.com"), std::string_view("GET"), std::nullopt);
  CHECK(bad.status == 403);
  CHECK_FALSE(bad.find_header("Access-Control-Allow-Origin"));
  CHECK(bad.find_header("Vary") == std::optional<std::string_view>("Origin"));
  auto body = boost::json::parse(std::get<std::string>(bad.body)).as_object();
  CHECK(body["error"].as_object()["code"] == "forbidden");

  auto none = http::preflight(p, std::nullopt, std::nullopt, std::nullopt);
  CHECK(none.status == 403);
}

TEST_CASE("cors: finalize_response adds security headers once", "[cors][session]") {
  auto p = policy();
  http::Response r = http::Response::json(boost::json::object{});
  http::finalize_response(r, "abc123", p, std::string_view("https://mail.example.com"));
  CHECK(r.find_header("X-Request-Id") == std::optional<std::string_view>("abc123"));
  CHECK(r.find_header("X-Content-Type-Options") == std::optional<std::string_view>("nosniff"));
  CHECK(r.find_header("Cache-Control") == std::optional<std::string_view>("no-store"));
  CHECK(r.find_header("Referrer-Policy") == std::optional<std::string_view>("no-referrer"));
  CHECK(r.find_header("Access-Control-Allow-Origin") == std::optional<std::string_view>("https://mail.example.com"));
  http::finalize_response(r, "def456", p, std::string_view("https://mail.example.com"));
  CHECK(headers_named(r, "X-Request-Id") == std::vector<std::string>{"def456"});
  CHECK(headers_named(r, "Cache-Control").size() == 1);

  // A handler's own Cache-Control wins (signed file responses are cacheable).
  http::Response f = http::Response::file("/tmp/x", "image/png", "inline");
  f.add_header("Cache-Control", "private, max-age=3600");
  http::finalize_response(f, "r", p, std::nullopt);
  CHECK(headers_named(f, "Cache-Control") == std::vector<std::string>{"private, max-age=3600"});
}
