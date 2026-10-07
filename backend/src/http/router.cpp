// Owner: WP-A
// WP0 stub: compiles and links; WP-A implements matching.
#include "http/router.hpp"

namespace azm::http {

struct Router::Impl {};

Router::Router() : impl_(std::make_unique<Impl>()) {}
Router::~Router() = default;
Router::Router(Router&&) noexcept = default;
Router& Router::operator=(Router&&) noexcept = default;

void Router::add(Route) { throw NotImplemented("http::Router::add"); }

Router::Match Router::match(beast::http::verb, std::string_view) const {
  throw NotImplemented("http::Router::match");
}

std::vector<beast::http::verb> Router::allowed_methods(std::string_view) const {
  throw NotImplemented("http::Router::allowed_methods");
}

std::size_t Router::size() const { throw NotImplemented("http::Router::size"); }

}  // namespace azm::http
