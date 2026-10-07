// Owner: WP-A
//
// Pattern router: "/api/threads/:id" style routes with 404 / 405 discrimination (DESIGN §3).
// Matching rules:
//  * Paths are compared segment by segment ('/'-separated, already percent-decoded by the
//    session). No trailing-slash normalisation: "/api/labels/" does not match "/api/labels".
//  * ":name" matches exactly one non-empty segment and captures it into Match::params["name"].
//  * At each position a literal segment wins over a ":param" segment, so "/api/files/raw/:id"
//    beats "/api/files/:id" for "/api/files/raw/7", and "/api/threads/actions" beats
//    "/api/threads/:id" for POST.
//  * Match::route is null when nothing matches the (method, path); path_exists then tells the
//    session to answer 405 (with Allow) instead of 404.
// Thread-safety: add() during startup only; match() is const and safe from any thread.
#pragma once

#include "http/types.hpp"

#include <boost/beast/http/verb.hpp>

#include <cstddef>
#include <memory>
#include <string_view>
#include <vector>

namespace azm::http {

class Router {
 public:
  Router();
  ~Router();
  Router(Router&&) noexcept;
  Router& operator=(Router&&) noexcept;
  Router(const Router&) = delete;
  Router& operator=(const Router&) = delete;

  // Registers a route. Throws std::invalid_argument when the pattern does not start with '/',
  // has an empty segment or an empty ":" name, or when (method, pattern) is already registered.
  void add(Route);

  struct Match {
    const Route* route = nullptr;  // null → 404 (or 405 when path_exists)
    Params params;                 // captured ":name" segments
    bool path_exists = false;      // some route matches the path with another method
  };
  Match match(beast::http::verb, std::string_view path) const;

  // Methods registered for routes matching `path` (for the 405 Allow header), in registration order.
  std::vector<beast::http::verb> allowed_methods(std::string_view path) const;

  std::size_t size() const;  // number of registered routes

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace azm::http
