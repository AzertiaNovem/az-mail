// Owner: WP-A — http::Router: params, 404/405, precedence, registration errors, real route table.
#include "api/routes.hpp"
#include "http/router.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

using namespace azm;
using boost::beast::http::verb;

namespace {

http::Response ok(http::Ctx&) { return http::Response::no_content(); }

http::Route route(verb m, std::string pattern) {
  return {m, std::move(pattern), http::AuthReq::None, http::BodyMode::None, 0, http::Exec::Db, ok};
}

}  // namespace

TEST_CASE("router: literal and parameter matching", "[router]") {
  http::Router r;
  r.add(route(verb::get, "/api/threads"));
  r.add(route(verb::get, "/api/threads/:id"));
  r.add(route(verb::post, "/api/threads/actions"));
  r.add(route(verb::get, "/api/messages/:id/events"));
  r.add(route(verb::get, "/api/admin/domains/:id/status"));
  CHECK(r.size() == 5);

  auto m = r.match(verb::get, "/api/threads");
  REQUIRE(m.route != nullptr);
  CHECK(m.route->pattern == "/api/threads");
  CHECK(m.params.empty());

  m = r.match(verb::get, "/api/threads/42");
  REQUIRE(m.route != nullptr);
  CHECK(m.route->pattern == "/api/threads/:id");
  CHECK(m.params.at("id") == "42");

  m = r.match(verb::get, "/api/messages/7/events");
  REQUIRE(m.route != nullptr);
  CHECK(m.params.at("id") == "7");

  m = r.match(verb::get, "/api/admin/domains/3/status");
  REQUIRE(m.route != nullptr);
  CHECK(m.params.at("id") == "3");

  // Parameters capture exactly one non-empty segment.
  CHECK(r.match(verb::get, "/api/threads/1/2").route == nullptr);
  CHECK(r.match(verb::get, "/api/threads/").route == nullptr);
  CHECK(r.match(verb::get, "/api/messages//events").route == nullptr);
}

TEST_CASE("router: literal beats parameter at each position", "[router]") {
  http::Router r;
  // Registered param-first on purpose: precedence must not depend on registration order.
  r.add(route(verb::get, "/api/files/:id"));
  r.add(route(verb::get, "/api/files/raw/:messageId"));
  r.add(route(verb::get, "/api/threads/:id"));
  r.add(route(verb::post, "/api/threads/actions"));
  r.add(route(verb::post, "/api/threads/:id"));

  auto m = r.match(verb::get, "/api/files/raw/9");
  REQUIRE(m.route != nullptr);
  CHECK(m.route->pattern == "/api/files/raw/:messageId");
  CHECK(m.params.at("messageId") == "9");
  CHECK(m.params.count("id") == 0);

  m = r.match(verb::get, "/api/files/12");
  REQUIRE(m.route != nullptr);
  CHECK(m.route->pattern == "/api/files/:id");

  m = r.match(verb::post, "/api/threads/actions");
  REQUIRE(m.route != nullptr);
  CHECK(m.route->pattern == "/api/threads/actions");

  // Per method: GET has only the parameter route, which then captures "actions".
  m = r.match(verb::get, "/api/threads/actions");
  REQUIRE(m.route != nullptr);
  CHECK(m.route->pattern == "/api/threads/:id");
  CHECK(m.params.at("id") == "actions");

  m = r.match(verb::post, "/api/threads/5");
  REQUIRE(m.route != nullptr);
  CHECK(m.route->pattern == "/api/threads/:id");
}

TEST_CASE("router: 404 vs 405 and Allow methods", "[router]") {
  http::Router r;
  r.add(route(verb::get, "/api/labels"));
  r.add(route(verb::post, "/api/labels"));
  r.add(route(verb::get, "/api/labels/:id"));
  r.add(route(verb::patch, "/api/labels/:id"));
  r.add(route(verb::delete_, "/api/labels/:id"));

  auto m = r.match(verb::put, "/api/labels");
  CHECK(m.route == nullptr);
  CHECK(m.path_exists);
  CHECK(r.allowed_methods("/api/labels") == std::vector<verb>{verb::get, verb::post});

  m = r.match(verb::post, "/api/labels/3");
  CHECK(m.route == nullptr);
  CHECK(m.path_exists);
  CHECK(r.allowed_methods("/api/labels/3") == std::vector<verb>{verb::get, verb::patch, verb::delete_});

  m = r.match(verb::get, "/api/nope");
  CHECK(m.route == nullptr);
  CHECK_FALSE(m.path_exists);
  CHECK(r.allowed_methods("/api/nope").empty());

  // No trailing-slash normalisation, no relative paths.
  CHECK_FALSE(r.match(verb::get, "/api/labels/").path_exists);
  CHECK_FALSE(r.match(verb::get, "api/labels").path_exists);
  CHECK_FALSE(r.match(verb::get, "").path_exists);
  // Matching is case-sensitive.
  CHECK(r.match(verb::get, "/API/labels").route == nullptr);
}

TEST_CASE("router: registration errors", "[router]") {
  http::Router r;
  CHECK_THROWS_AS(r.add(route(verb::get, "api/x")), std::invalid_argument);
  CHECK_THROWS_AS(r.add(route(verb::get, "/api//x")), std::invalid_argument);
  CHECK_THROWS_AS(r.add(route(verb::get, "/api/x/")), std::invalid_argument);
  CHECK_THROWS_AS(r.add(route(verb::get, "/api/:")), std::invalid_argument);
  http::Route no_handler = route(verb::get, "/api/y");
  no_handler.handler = nullptr;
  CHECK_THROWS_AS(r.add(no_handler), std::invalid_argument);

  r.add(route(verb::get, "/api/x/:id"));
  CHECK_THROWS_AS(r.add(route(verb::get, "/api/x/:id")), std::invalid_argument);
  // Same shape, different parameter name: ambiguous, rejected.
  CHECK_THROWS_AS(r.add(route(verb::get, "/api/x/:other")), std::invalid_argument);
  // Other method on the same pattern is fine.
  CHECK_NOTHROW(r.add(route(verb::delete_, "/api/x/:id")));
  CHECK(r.size() == 2);

  // The root path is a valid pattern.
  CHECK_NOTHROW(r.add(route(verb::get, "/")));
  CHECK(r.match(verb::get, "/").route != nullptr);
}

TEST_CASE("router: move keeps routes and match pointers valid", "[router]") {
  http::Router a;
  a.add(route(verb::get, "/api/health"));
  http::Router b(std::move(a));
  auto m = b.match(verb::get, "/api/health");
  REQUIRE(m.route != nullptr);
  CHECK(m.route->pattern == "/api/health");
  http::Router c;
  c = std::move(b);
  CHECK(c.size() == 1);
}

TEST_CASE("router: the real route table registers and resolves", "[router]") {
  http::Router r;
  api::register_routes(r);
  CHECK(r.size() == api::route_table().size());

  struct Case {
    verb m;
    const char* path;
    const char* pattern;
  };
  const Case cases[] = {
      {verb::post, "/api/auth/login", "/api/auth/login"},
      {verb::get, "/api/threads/17", "/api/threads/:id"},
      {verb::post, "/api/threads/actions", "/api/threads/actions"},
      {verb::post, "/api/messages/3/undo-send", "/api/messages/:id/undo-send"},
      {verb::get, "/api/files/raw/5", "/api/files/raw/:messageId"},
      {verb::get, "/api/files/5", "/api/files/:id"},
      {verb::get, "/api/admin/domains/2/status", "/api/admin/domains/:id/status"},
      {verb::post, "/api/admin/outbox/8/retry", "/api/admin/outbox/:id/retry"},
      {verb::delete_, "/api/drafts/4", "/api/drafts/:id"},
  };
  for (const auto& c : cases) {
    INFO(c.path);
    auto m = r.match(c.m, c.path);
    REQUIRE(m.route != nullptr);
    CHECK(m.route->pattern == c.pattern);
  }
  auto m = r.match(verb::put, "/api/auth/login");
  CHECK(m.route == nullptr);
  CHECK(m.path_exists);
  CHECK(r.allowed_methods("/api/settings") == std::vector<verb>{verb::get, verb::put});
  // /api/ws is not a route (the session upgrades it before routing).
  CHECK_FALSE(r.match(verb::get, "/api/ws").path_exists);
}
